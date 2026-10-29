#include "tab5_grove.hpp"
#include "tab5_marks.hpp"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

#include "tab5_power.hpp"

static const char *TAG = "grove";

// Same pins of Azimutal - the Grove and one so.
#define GROVE_UART   UART_NUM_1
#define GROVE_RX     54
#define GROVE_TX     53

// Time for the module to start after receiving power. The C6L has one ESP32-C6
// which boots the entire Meshtastic; 1.2s and a long time for it to reach the point of
// respond. Too short here makes the C6L seem absent.
// Time for module to start AFTER receiving power.
//
// 1.2s was enough for a GPS, which starts talking almost immediately. NOT enough for
// Unit C6L: it runs the entire Meshtastic on an ESP32-C6 and takes seconds. With the
// short wait, it restarted with each poll and never responded -
// the probe said "nothing" with the module plugged in and working.
//
// If the JA rail is on ("Grove always on" setting), there is no boot
// none of the wait and the probe is immediate.
#define MODULE_BOOT_WAIT_MS   4000

// How much to hear at each baud. GPS sends one sentence per second at worst
// case, then 1.5s guarantees at least one.
#define ESCUTA_MS        1500

static grove_type_t s_detected_type = GROVE_NOTHING;
static grove_owner_t s_owner = GROVE_OWNER_NONE;

const char* grove_owner_name(grove_owner_t d)
{
  switch (d) {
    case GROVE_OWNER_MESH: return "service Meshtastic";
    case GROVE_OWNER_GPS:  return "GPS";
    default:              return "ninguem";
  }
}

grove_owner_t tab5_grove_owner(void) { return s_owner; }

bool tab5_grove_acquire(grove_owner_t who)
{
  if (s_owner != GROVE_OWNER_NONE && s_owner != who) {
    ESP_LOGW(TAG, "Grove is already owned by %s - %s cannot take it over",
             grove_owner_name(s_owner), grove_owner_name(who));
    return false;
  }
  s_owner = who;
  ESP_LOGI(TAG, "Grove agora e do %s", grove_owner_name(who));
  return true;
}

void tab5_grove_release(grove_owner_t who)
{
  // Only the owner released. Without this guard, an app that would close after having
  // FAILED acquire would release the Grove of whoever was using it.
  if (s_owner != who) { return; }
  s_owner = GROVE_OWNER_NONE;
  ESP_LOGI(TAG, "Grove liberado por %s", grove_owner_name(who));
}

const char* grove_name(grove_type_t t)
{
  switch (t) {
    case GROVE_GPS:          return "GPS (NMEA)";
    case GROVE_MESHTASTIC:   return "Meshtastic (C6L)";
    case GROVE_UNKNOWN: return "unknown";
    default:                 return "none";
  }
}

grove_type_t tab5_grove_type(void) { return s_detected_type; }

static bool open(int baud)
{
  // Shuts the UART driver during probe.
  //
  // The M5.Ex_I2C is initialized at boot on the Grove pins (54/53) and RESERVES the
  // GPIOs. When tab5_keyboard repoints this bus to ExtPort1, the
  // release() of M5GFX released the bus but does not return the reservation - it stays
  // hanging. The uart_set_pin then warns "GPIO 53 is not usable" and configures the
  // pin anyway (the probe works: we measured 209 bytes of NMEA with the warning
  // appearing). It's known noise, quiet only here.
  esp_log_level_set("uart", ESP_LOG_ERROR);

  uart_config_t cfg = { };
  cfg.baud_rate = baud;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity    = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  tab5_mark("grove: UART a", baud);   // installs the driver - and the PM locks it
  if (uart_driver_install(GROVE_UART, 2048, 512, 0, NULL, 0) != ESP_OK) {
    esp_log_level_set("uart", ESP_LOG_WARN); return false;
  }
  if (uart_param_config(GROVE_UART, &cfg) != ESP_OK) {
    uart_driver_delete(GROVE_UART); esp_log_level_set("uart", ESP_LOG_WARN); return false;
  }
  // Pay attention to order: uart_set_pin receives TX before RX.
  if (uart_set_pin(GROVE_UART, GROVE_TX, GROVE_RX,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
  {
    uart_driver_delete(GROVE_UART);
    esp_log_level_set("uart", ESP_LOG_WARN);
    return false;
  }
  return true;
}

static void close(void)
{
  uart_driver_delete(GROVE_UART);
  esp_log_level_set("uart", ESP_LOG_WARN);   // out of probe the iteration warning takes effect
}

// Reads what arrives in window and returns how many bytes it took.
static int escutar(uint8_t* buf, int size, int ms)
{
  int n = 0;
  const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
  while (n < size && xTaskGetTickCount() < end)
  {
    const int r = uart_read_bytes(GROVE_UART, buf + n, size - n, pdMS_TO_TICKS(100));
    if (r > 0) { n += r; }
  }
  return n;
}

// -------------------------------------------------------------------------------
// Probe of Meshtastic (115200, active)
// -------------------------------------------------------------------------------
// Ask Meshtastic to introduce himself. Call with UART JA open.
//
// This is mandatory: in PROTO mode the radio DOES NOT speak to itself, it only responds. One
// passive listening returns zero bytes even with everything working - that's what I
// tricked into using the raw dump to test the C6L.
//
// Sending these bytes to a GPS is harmless: it ignores what it doesn't understand.
static void request_apresentacao(void)
{
  // StreamAPI of Meshtastic asks client to wake up port with 32 bytes
  // of START1 before first request - the radio may be in bottom mode
  // consumption in serial and losing the first bytes.
  uint8_t acorda[32];
  memset(acorda, 0x94, sizeof(acorda));
  uart_write_bytes(GROVE_UART, acorda, sizeof(acorda));
  vTaskDelay(pdMS_TO_TICKS(120));

  // ToRadio{ want_config_id = 0x2A } -- protobuf raw, mounted by hand:
  // field 3 (want_config_id), varint -> tag 0x18, value 0x2A
  // Framed: 0x94 0xC3 <size_hi> <size_lo> <payload>
  const uint8_t frame[] = { 0x94, 0xC3, 0x00, 0x02, 0x18, 0x2A };
  uart_write_bytes(GROVE_UART, frame, sizeof(frame));
}

static bool sondar_meshtastic(void)
{
  if (!open(115200)) { return false; }
  request_apresentacao();

  uint8_t buf[512];
  const int n = escutar(buf, sizeof(buf), ESCUTA_MS);
  close();

  ESP_LOGI(TAG, "sondagem a 115200 baud (Meshtastic?): %d byte(s) recebidos", n);
  for (int i = 0; i + 1 < n; ++i)
  {
    if (buf[i] == 0x94 && buf[i+1] == 0xC3) { return true; }
  }
  return false;
}

// -------------------------------------------------------------------------------
// GPS probe (9600, passive)
// -------------------------------------------------------------------------------
static bool sondar_gps(int* bytes_vistos)
{
  if (!open(9600)) { return false; }
  uint8_t buf[512];
  const int n = escutar(buf, sizeof(buf), ESCUTA_MS);
  close();

  *bytes_vistos = n;
  ESP_LOGI(TAG, "sondagem a 9600 baud (GPS?): %d byte(s) recebidos", n);

  // NMEA signature: '$' followed by two capital letters (GP, GL, GN, GA...).
  // Requiring the letters avoids matching one '$' that fell into the storage of binary garbage.
  for (int i = 0; i + 2 < n; ++i)
  {
    if (buf[i] == '$' && buf[i+1] >= 'A' && buf[i+1] <= 'Z'
                      && buf[i+2] >= 'A' && buf[i+2] <= 'Z') { return true; }
  }
  return false;
}

// -------------------------------------------------------------------------------

void tab5_grove_dump(int baud, int ms)
{
  // ASK BEFORE YOU CALL - otherwise the answer would always be "yes" (the
  // tab5_power_grove(true) below). State of hardware of truth, no
  // preference (see comment in tab5_power_grove_on()).
  const bool ja_enabled = tab5_power_grove_on();
  tab5_power_grove(true);
  // If the trail has just started, the module is booting: wait. See the
  // comment of ESPERA_BOOT_MS - this is what made the C6L seem dumb.
  vTaskDelay(pdMS_TO_TICKS(ja_enabled ? 300 : MODULE_BOOT_WAIT_MS));

  if (!open(baud)) { printf("could not open UART at %d\n", baud); return; }

  // ALWAYS ask for an introduction. For Meshtastic it is mandatory (it doesn't say
  // alone); for one GPS and harmless. Without this the eviction lies: it shows zero
  // bytes in a module that is perfect are silent.
  request_apresentacao();

  static uint8_t buf[512];
  const int n = escutar(buf, sizeof(buf), ms);
  close();

  printf("--- %d bytes a %d baud ---\n", n, baud);
  for (int i = 0; i < n; i += 16)
  {
    printf("%04d  ", i);
    for (int j = 0; j < 16; ++j) {
      if (i + j < n) { printf("%02X ", buf[i+j]); } else { printf("   "); }
    }
    printf(" |");
    for (int j = 0; j < 16 && i + j < n; ++j) {
      const uint8_t c = buf[i+j];
      printf("%c", (c >= 32 && c < 127) ? c : '.');
    }
    printf("|\n");
  }
  printf("--- end ---\n");
}

// IS THERE ANYONE ON CABO? One reading of GPIO, and that's it.
//
// True polling costs 3 SECONDS - turns on the UART, listens for 1.5s at 9600,
// changes the baud, asks for a presentation and listens for another 1.5s at 115200. This cannot be done
// run in a loop: this is what made the detection become a decision of boot, made once and
// never reviewed again.
//
// But identifying WHO is there is expensive; realize that SOMEONE has arrived and cheaply. A
// line of data reports alone: in UART, sleep and level HIGH, and every module
// keeps his TX at this level even without saying anything. Without module, the pin stays
// released.
//
// So: internal pull-down on is a read.
// module present -> his TX (push-pull) wins the pull-down -> HIGH
// nothing on the cable -> only the pull-down supports the pin -> LOW
//
// Cost: one gpio_config and one gpio_get_level. At 0.5Hz and statistical noise on
// side of anything the screen does.
//
// ONLY WITH THE DOOR FREE. With the UART installed, its e of driver pin, and
// reconfiguring by bottom would break the reading of who is using it.
bool tab5_grove_present(void)
{
  gpio_config_t c = { };
  c.pin_bit_mask = (1ULL << GROVE_RX);
  c.mode         = GPIO_MODE_INPUT;
  c.pull_up_en   = GPIO_PULLUP_DISABLE;
  c.pull_down_en = GPIO_PULLDOWN_ENABLE;
  c.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&c);

  // The pull-down is weak (tens of kohm) and needs to discharge the capacitance
  // of pin and of cable. Without this pause, the first reading takes the level OLD -
  // and the newly removed module would still appear to be present.
  esp_rom_delay_us(500);

  return gpio_get_level((gpio_num_t)GROVE_RX) != 0;
}

// Forgets what was detected. For when the cable remains empty: there is nothing to
// identify, so you don't pay poll none to conclude "nothing".
void tab5_grove_forget(void) { s_detected_type = GROVE_NOTHING; }

grove_type_t tab5_grove_detect(void)
{
  ESP_LOGI(TAG, "sondando o Grove...");

  // Energy first: without 5V no module appears, and the daily probe
  // "nothing" with the device plugged in - the worst type of wrong answer.
  //
  // HARDWARE STATUS, not the "always on" preference - both
  // used to coincide, but they are not the same question (the track may be
  // on now for another reason even with the preference turned off). Fixed
  // along with the audit of layers - HAL asks HAL, never
  // a one service (see docs/WRITING_APPS.md section 8).
  if (tab5_power_grove_on()) {
    ESP_LOGI(TAG, "power rail was already on - probing without a boot wait");
  } else {
    tab5_power_grove(true);
    ESP_LOGI(TAG, "powered the rail on - waiting %d ms for the module to boot", MODULE_BOOT_WAIT_MS);
    vTaskDelay(pdMS_TO_TICKS(MODULE_BOOT_WAIT_MS));
  }

  // Meshtastic BEFORE GPS. Reason: his probe writes in line, and write
  // on a GPS is harmless (it ignores it), while the opposite is not valid - the
  // Silent Meshtastic would be "nothing" if we tested it just by listening.
  if (sondar_meshtastic()) { s_detected_type = GROVE_MESHTASTIC; }
  else
  {
    int seen_devices = 0;
    if (sondar_gps(&seen_devices))  { s_detected_type = GROVE_GPS; }
    else if (seen_devices > 0)      { s_detected_type = GROVE_UNKNOWN; }
    else                      { s_detected_type = GROVE_NOTHING; }
  }

  ESP_LOGI(TAG, "Grove: %s", grove_name(s_detected_type));
  return s_detected_type;
}
