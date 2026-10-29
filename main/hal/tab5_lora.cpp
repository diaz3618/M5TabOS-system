#include "tab5_lora.hpp"

#include <M5Unified.h>
#include <RadioLib.h>
#include <hal/ESP-IDF/EspHal.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <string.h>

static const char* TAG = "lora";

// Fixed SPI wiring on the Tab5's M5-Bus.
static constexpr int PIN_SCK  = 5;   // M-Bus 11
static constexpr int PIN_MOSI = 18;  // M-Bus 7
static constexpr int PIN_MISO = 19;  // M-Bus 9

// DEFAULT ROUTE - CONFIRMED against the schematic (see the big warning in the
// .hpp), no longer a guess: switches 2, 5, and 9 on (the same ones this
// Tab5's owner read off their own board) give exactly NSS=M-Bus20/G4,
// BUSY=M-Bus19/G3, IRQ=M-Bus2/G16 - the same route that was already here,
// now confirmed.
static constexpr int PIN_IRQ_DEFAULT  = 16;
static constexpr int PIN_BUSY_DEFAULT = 3;
static constexpr int PIN_NSS_DEFAULT  = 4;

static constexpr uint32_t I2C_HZ = 100000;

// M5IOE1 registers. We only use the small GPIO subset, to avoid creating a
// second bus owner or pulling in another I2C driver in parallel.
static constexpr uint8_t REG_UID_L     = 0x00;
static constexpr uint8_t REG_REV       = 0x02;
static constexpr uint8_t REG_MODE_L    = 0x03;
static constexpr uint8_t REG_OUTPUT_L  = 0x05;
static constexpr uint8_t REG_PULLUP_L  = 0x09;
static constexpr uint8_t REG_PULLDN_L  = 0x0B;
static constexpr uint8_t REG_DRIVE_L   = 0x13;

// The library numbers IO1 as bit zero.
static constexpr uint16_t IO_RST    = 1u << 1;  // IO2
static constexpr uint16_t IO_BYPASS = 1u << 2;  // IO3
static constexpr uint16_t IO_PWR_EN = 1u << 4;  // IO5
static constexpr uint16_t IO_CONTROL = IO_RST | IO_BYPASS | IO_PWR_EN;

static tab5_lora_status_t s_status = {
  .gpio_nss = PIN_NSS_DEFAULT,
  .gpio_irq = PIN_IRQ_DEFAULT,
  .gpio_busy = PIN_BUSY_DEFAULT,
};

// THE REAL CANDIDATES, one per switch (switches 1 to 11) - see the warning in
// the .hpp. Switch 1 (NSS, "ISO_G0" on the module's schematic) has no M5-Bus
// pin: it's left out of this list on purpose, and it's not a 12th "missing"
// candidate - it's just the position that never connects to anything useful
// here.
static tab5_lora_pin_t s_pins[] = {
  // fixed (on switch): M5IOE1 I2C
  { 17, 31, 0, TAB5_LORA_SIGNAL_I2C, TAB5_LORA_PIN_I2C_HIGH },
  { 18, 32, 0, TAB5_LORA_SIGNAL_I2C, TAB5_LORA_PIN_I2C_HIGH },
  // SW1, switches 2-4: NSS
  { 20,  4, 2, TAB5_LORA_SIGNAL_NSS, TAB5_LORA_PIN_LOW },
  { 21,  2, 3, TAB5_LORA_SIGNAL_NSS, TAB5_LORA_PIN_LOW },
  { 23, 47, 4, TAB5_LORA_SIGNAL_NSS, TAB5_LORA_PIN_LOW },
  // SW1, switches 5-8: BUSY
  { 19,  3, 5, TAB5_LORA_SIGNAL_BUSY, TAB5_LORA_PIN_LOW },
  {  4, 17, 6, TAB5_LORA_SIGNAL_BUSY, TAB5_LORA_PIN_LOW },
  { 22, 48, 7, TAB5_LORA_SIGNAL_BUSY, TAB5_LORA_PIN_LOW },
  {  8, 45, 8, TAB5_LORA_SIGNAL_BUSY, TAB5_LORA_PIN_LOW },
  // SW3, switches 9-11: IRQ
  {  2, 16,  9, TAB5_LORA_SIGNAL_IRQ, TAB5_LORA_PIN_LOW },
  { 26, 51, 10, TAB5_LORA_SIGNAL_IRQ, TAB5_LORA_PIN_LOW },
  { 10, 52, 11, TAB5_LORA_SIGNAL_IRQ, TAB5_LORA_PIN_LOW },
};

static EspHal s_hal(PIN_SCK, PIN_MISO, PIN_MOSI, SPI2_HOST, 2000000);
static Module s_module(&s_hal, PIN_NSS_DEFAULT, PIN_IRQ_DEFAULT,
                       RADIOLIB_NC, PIN_BUSY_DEFAULT);
static SX1262 s_radio(&s_module);

static bool read16(uint8_t address, uint8_t reg, uint16_t* value)
{
  uint8_t b[2] = { };
  if (!M5.In_I2C.readRegister(address, reg, b, sizeof(b), I2C_HZ)) { return false; }
  *value = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  return true;
}

static bool write16(uint8_t address, uint8_t reg, uint16_t value)
{
  const uint8_t b[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
  return M5.In_I2C.writeRegister(address, reg, b, sizeof(b), I2C_HZ);
}

static bool modify16(uint8_t address, uint8_t reg, uint16_t set_bits, uint16_t clear_bits)
{
  uint16_t value;
  if (!read16(address, reg, &value)) { return false; }
  value = (uint16_t)((value | set_bits) & ~clear_bits);
  return write16(address, reg, value);
}

static bool configure_outputs(uint8_t address)
{
  // MODE=1 is output; DRIVE=0 is push-pull. Removing the pulls avoids a
  // hidden current draw while the radio rail is disabled. The safe value
  // goes to the output latch first, so switching to output doesn't create a
  // power glitch.
  return modify16(address, REG_PULLUP_L, 0, IO_CONTROL)
      && modify16(address, REG_PULLDN_L, 0, IO_CONTROL)
      && modify16(address, REG_DRIVE_L, 0, IO_CONTROL)
      && modify16(address, REG_OUTPUT_L, IO_BYPASS,
                   IO_CONTROL & ~IO_BYPASS)
      && modify16(address, REG_MODE_L, IO_CONTROL, 0);
}

static bool write_control(uint8_t address, uint16_t high_bits)
{
  return modify16(address, REG_OUTPUT_L, high_bits & IO_CONTROL,
                   IO_CONTROL & ~high_bits);
}

static void probe_ioe(void)
{
  s_status.ioe_address = 0;
  s_status.ioe_mask = 0;
  s_status.ioe_uid = 0;
  s_status.ioe_revision = 0;

  for (uint8_t address = 0x71; address <= 0x74; ++address) {
    if (!M5.In_I2C.scanID(address, I2C_HZ)) { continue; }
    // UID and revision tell a real M5IOE1 reply apart from some other
    // device unlikely to have landed in the same range.
    uint16_t uid;
    uint8_t rev;
    if (!read16(address, REG_UID_L, &uid)
     || !M5.In_I2C.readRegister(address, REG_REV, &rev, 1, I2C_HZ)) { continue; }
    s_status.ioe_mask |= (uint8_t)(1u << (address - 0x71));
    if (!s_status.ioe_address) {
      s_status.ioe_address = address;
      s_status.ioe_uid = uid;
      s_status.ioe_revision = rev;
    }
    ESP_LOGI(TAG, "M5IOE1 answered at 0x%02X (UID %04X, rev %02X)",
             address, uid, rev);
  }
  if (!s_status.ioe_address) {
    ESP_LOGW(TAG, "no M5IOE1 answered between 0x71 and 0x74");
  }
}

static tab5_lora_pin_state_t measure_pin(int gpio)
{
  gpio_config_t c = { };
  c.pin_bit_mask = 1ULL << gpio;
  c.mode = GPIO_MODE_INPUT;
  c.intr_type = GPIO_INTR_DISABLE;

  c.pull_down_en = GPIO_PULLDOWN_ENABLE;
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  if (gpio_config(&c) != ESP_OK) { return TAB5_LORA_PIN_FLOATING; }
  vTaskDelay(pdMS_TO_TICKS(1));
  const int with_pulldown = gpio_get_level((gpio_num_t)gpio);

  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  c.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&c);
  vTaskDelay(pdMS_TO_TICKS(1));
  const int with_pullup = gpio_get_level((gpio_num_t)gpio);

  // Leave the candidates floating. The three used by RadioLib will be
  // configured right after, if the caller requests initialization.
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&c);

  if (!with_pulldown && with_pullup) { return TAB5_LORA_PIN_FLOATING; }
  return with_pulldown ? TAB5_LORA_PIN_HIGH : TAB5_LORA_PIN_LOW;
}

static void probe_pins(void)
{
  for (size_t i = 0; i < sizeof(s_pins) / sizeof(s_pins[0]); ++i) {
    // SDA/SCL already carry touch, IMU, power, and this very expander.
    // Reconfiguring them to test for floating would take the bus down.
    if (s_pins[i].mbus == 17 || s_pins[i].mbus == 18) {
      s_pins[i].state = gpio_get_level((gpio_num_t)s_pins[i].gpio)
                           ? TAB5_LORA_PIN_I2C_HIGH
                           : TAB5_LORA_PIN_I2C_LOW;
    } else {
      s_pins[i].state = measure_pin(s_pins[i].gpio);
    }
  }
}

void tab5_lora_probe(void)
{
  probe_pins();
  probe_ioe();
  s_status.probed = true;
}

bool tab5_lora_start(void)
{
  tab5_lora_probe();
  s_status.radio_present = false;
  s_status.state_error = RADIOLIB_ERR_UNKNOWN;

  if (!s_status.ioe_address) {
    s_status.radio_error = RADIOLIB_ERR_CHIP_NOT_FOUND;
    return false;
  }

  const uint8_t e = s_status.ioe_address;
  if (!configure_outputs(e)
   || !write_control(e, IO_BYPASS)) {  // reset low, power low
    s_status.radio_error = RADIOLIB_ERR_SPI_WRITE_FAILED;
    ESP_LOGE(TAG, "failed to prepare the M5IOE1 at 0x%02X", e);
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(25));
  if (!write_control(e, IO_BYPASS | IO_PWR_EN)) {
    s_status.radio_error = RADIOLIB_ERR_SPI_WRITE_FAILED;
    ESP_LOGE(TAG, "failed to power on the radio through the M5IOE1");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(120));
  if (!write_control(e, IO_BYPASS | IO_PWR_EN | IO_RST)) {
    s_status.radio_error = RADIOLIB_ERR_SPI_WRITE_FAILED;
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(120));

  // The module's own official example uses a 3.0 V TCXO and external reset
  // through the M5IOE1. begin() also does the reading that proves an SX1262 is
  // actually there.
  s_radio.tcxoVoltage = 3.0f;
  s_status.radio_error = s_radio.begin(868.0f, 125.0f, 12, 5, 0x34,
                                      10, 20, 3.0f, true);
  s_status.radio_present = (s_status.radio_error == RADIOLIB_ERR_NONE);
  if (s_status.radio_present) {
    s_status.state_error = s_radio.standby();
    ESP_LOGI(TAG, "SX1262 answered (standby=%d)", s_status.state_error);
  } else {
    ESP_LOGW(TAG, "SX1262 did not answer on the default pins (error %d)",
             s_status.radio_error);
  }
  return s_status.radio_present;
}

bool tab5_lora_present(void) { return s_status.radio_present; }

tab5_lora_status_t tab5_lora_status(void) { return s_status; }

size_t tab5_lora_pins(tab5_lora_pin_t* output, size_t capacity)
{
  const size_t n = sizeof(s_pins) / sizeof(s_pins[0]);
  if (output && capacity) {
    const size_t to_copy = capacity < n ? capacity : n;
    memcpy(output, s_pins, to_copy * sizeof(*output));
  }
  return n;
}

const char* tab5_lora_pin_state_name(tab5_lora_pin_state_t state)
{
  switch (state) {
    case TAB5_LORA_PIN_LOW:     return "low";
    case TAB5_LORA_PIN_HIGH:      return "high";
    case TAB5_LORA_PIN_FLOATING: return "floating";
    case TAB5_LORA_PIN_I2C_LOW: return "I2C low";
    case TAB5_LORA_PIN_I2C_HIGH:  return "I2C high";
    default:                      return "?";
  }
}
