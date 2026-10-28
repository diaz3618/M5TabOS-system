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

// Fiacao fixa do SPI no M5-Bus do Tab5.
static constexpr int PINO_SCK  = 5;   // M-Bus 11
static constexpr int PINO_MOSI = 18;  // M-Bus 7
static constexpr int PINO_MISO = 19;  // M-Bus 9

// ROTA PADRAO - CONFERIDA no esquematico (ver o aviso grande no .hpp), nao
// mais um palpite: switches 2, 5 e 9 ligados (os mesmos que o owner deste
// Tab5 leu na propria placa) dao exatamente NSS=M-Bus20/G4, BUSY=M-Bus19/G3,
// IRQ=M-Bus2/G16 - a mesma rota que ja estava aqui, agora com certeza.
static constexpr int PINO_IRQ_PADRAO  = 16;
static constexpr int PINO_BUSY_PADRAO = 3;
static constexpr int PINO_NSS_PADRAO  = 4;

static constexpr uint32_t I2C_HZ = 100000;

// Registradores do M5IOE1. So usamos a pequena parte GPIO para nao criar um
// segundo owner do barramento nem puxar outro driver I2C em paralelo.
static constexpr uint8_t REG_UID_L     = 0x00;
static constexpr uint8_t REG_REV       = 0x02;
static constexpr uint8_t REG_MODO_L    = 0x03;
static constexpr uint8_t REG_SAIDA_L   = 0x05;
static constexpr uint8_t REG_PULLUP_L  = 0x09;
static constexpr uint8_t REG_PULLDN_L  = 0x0B;
static constexpr uint8_t REG_DRIVE_L   = 0x13;

// A biblioteca numera IO1 como bit zero.
static constexpr uint16_t IO_RST    = 1u << 1;  // IO2
static constexpr uint16_t IO_BYPASS = 1u << 2;  // IO3
static constexpr uint16_t IO_PWR_EN = 1u << 4;  // IO5
static constexpr uint16_t IO_CONTROLE = IO_RST | IO_BYPASS | IO_PWR_EN;

static tab5_lora_status_t s_status = {
  .gpio_nss = PINO_NSS_PADRAO,
  .gpio_irq = PINO_IRQ_PADRAO,
  .gpio_busy = PINO_BUSY_PADRAO,
};

// OS CANDIDATOS DE VERDADE, um por switch_number (switches 1 a 11) - ver o aviso no
// .hpp. O switch 1 (NSS, "ISO_G0" no esquematico do modulo) nao tem pino de
// M5-Bus: fica de fora desta lista de proposito, e nao e um 12o candidato
// "que falta" - e so a position que nunca liga nada util aqui.
static tab5_lora_pin_t s_pinos[] = {
  // fixos (sem switch_number): I2C do M5IOE1
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

static EspHal s_hal(PINO_SCK, PINO_MISO, PINO_MOSI, SPI2_HOST, 2000000);
static Module s_modulo(&s_hal, PINO_NSS_PADRAO, PINO_IRQ_PADRAO,
                       RADIOLIB_NC, PINO_BUSY_PADRAO);
static SX1262 s_radio(&s_modulo);

static bool ler16(uint8_t address, uint8_t reg, uint16_t* value)
{
  uint8_t b[2] = { };
  if (!M5.In_I2C.readRegister(address, reg, b, sizeof(b), I2C_HZ)) { return false; }
  *value = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  return true;
}

static bool escrever16(uint8_t address, uint8_t reg, uint16_t value)
{
  const uint8_t b[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
  return M5.In_I2C.writeRegister(address, reg, b, sizeof(b), I2C_HZ);
}

static bool alterar16(uint8_t address, uint8_t reg, uint16_t turn_on, uint16_t turn_off)
{
  uint16_t value;
  if (!ler16(address, reg, &value)) { return false; }
  value = (uint16_t)((value | turn_on) & ~turn_off);
  return escrever16(address, reg, value);
}

static bool saidas_controle(uint8_t address)
{
  // MODO=1 e output; DRIVE=0 e push-pull. Tirar pulls evita uma corrente
  // escondida enquanto o trilho do radio esta desligado. O value seguro vai
  // primeiro ao latch, para a troca para output nao criar um pulso de energia.
  return alterar16(address, REG_PULLUP_L, 0, IO_CONTROLE)
      && alterar16(address, REG_PULLDN_L, 0, IO_CONTROLE)
      && alterar16(address, REG_DRIVE_L, 0, IO_CONTROLE)
      && alterar16(address, REG_SAIDA_L, IO_BYPASS,
                   IO_CONTROLE & ~IO_BYPASS)
      && alterar16(address, REG_MODO_L, IO_CONTROLE, 0);
}

static bool escrever_controle(uint8_t address, uint16_t altos)
{
  return alterar16(address, REG_SAIDA_L, altos & IO_CONTROLE,
                   IO_CONTROLE & ~altos);
}

static void sondar_ioe(void)
{
  s_status.ioe_address = 0;
  s_status.ioe_mask = 0;
  s_status.ioe_uid = 0;
  s_status.ioe_revision = 0;

  for (uint8_t address = 0x71; address <= 0x74; ++address) {
    if (!M5.In_I2C.scanID(address, I2C_HZ)) { continue; }
    // UID e revisao distinguem resposta real do M5IOE1 de outro dispositivo
    // improvavel que tenha caido na mesma faixa.
    uint16_t uid;
    uint8_t rev;
    if (!ler16(address, REG_UID_L, &uid)
     || !M5.In_I2C.readRegister(address, REG_REV, &rev, 1, I2C_HZ)) { continue; }
    s_status.ioe_mask |= (uint8_t)(1u << (address - 0x71));
    if (!s_status.ioe_address) {
      s_status.ioe_address = address;
      s_status.ioe_uid = uid;
      s_status.ioe_revision = rev;
    }
    ESP_LOGI(TAG, "M5IOE1 respondeu em 0x%02X (UID %04X, rev %02X)",
             address, uid, rev);
  }
  if (!s_status.ioe_address) {
    ESP_LOGW(TAG, "nenhum M5IOE1 respondeu entre 0x71 e 0x74");
  }
}

static tab5_lora_pin_state_t medir_pino(int gpio)
{
  gpio_config_t c = { };
  c.pin_bit_mask = 1ULL << gpio;
  c.mode = GPIO_MODE_INPUT;
  c.intr_type = GPIO_INTR_DISABLE;

  c.pull_down_en = GPIO_PULLDOWN_ENABLE;
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  if (gpio_config(&c) != ESP_OK) { return TAB5_LORA_PIN_FLOATING; }
  vTaskDelay(pdMS_TO_TICKS(1));
  const int com_baixo = gpio_get_level((gpio_num_t)gpio);

  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  c.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&c);
  vTaskDelay(pdMS_TO_TICKS(1));
  const int com_alto = gpio_get_level((gpio_num_t)gpio);

  // Deixa os candidatos em alta impedancia. Os tres usados pela RadioLib
  // serao configurados logo depois, se o usuario pedir a inicializacao.
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&c);

  if (!com_baixo && com_alto) { return TAB5_LORA_PIN_FLOATING; }
  return com_baixo ? TAB5_LORA_PIN_HIGH : TAB5_LORA_PIN_LOW;
}

static void sondar_pinos(void)
{
  for (size_t i = 0; i < sizeof(s_pinos) / sizeof(s_pinos[0]); ++i) {
    // SDA/SCL ja sustentam touch, IMU, energia e este proprio expansor.
    // Reconfigura-los para testar flutuacao derrubaria o barramento.
    if (s_pinos[i].mbus == 17 || s_pinos[i].mbus == 18) {
      s_pinos[i].state = gpio_get_level((gpio_num_t)s_pinos[i].gpio)
                           ? TAB5_LORA_PIN_I2C_HIGH
                           : TAB5_LORA_PIN_I2C_LOW;
    } else {
      s_pinos[i].state = medir_pino(s_pinos[i].gpio);
    }
  }
}

void tab5_lora_probe(void)
{
  sondar_pinos();
  sondar_ioe();
  s_status.probed = true;
}

bool tab5_lora_iniciar(void)
{
  tab5_lora_probe();
  s_status.radio_present = false;
  s_status.state_error = RADIOLIB_ERR_UNKNOWN;

  if (!s_status.ioe_address) {
    s_status.radio_error = RADIOLIB_ERR_CHIP_NOT_FOUND;
    return false;
  }

  const uint8_t e = s_status.ioe_address;
  if (!saidas_controle(e)
   || !escrever_controle(e, IO_BYPASS)) {  // reset baixo, energia low
    s_status.radio_error = RADIOLIB_ERR_SPI_WRITE_FAILED;
    ESP_LOGE(TAG, "failed a preparacao do M5IOE1 em 0x%02X", e);
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(25));
  if (!escrever_controle(e, IO_BYPASS | IO_PWR_EN)) {
    s_status.radio_error = RADIOLIB_ERR_SPI_WRITE_FAILED;
    ESP_LOGE(TAG, "failed ao turn_on a energia do radio pelo M5IOE1");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(120));
  if (!escrever_controle(e, IO_BYPASS | IO_PWR_EN | IO_RST)) {
    s_status.radio_error = RADIOLIB_ERR_SPI_WRITE_FAILED;
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(120));

  // O exemplo oficial do proprio modulo usa TCXO a 3,0 V e reset externo
  // pelo M5IOE1. O begin tambem faz a leitura que prova que ha um SX1262.
  s_radio.tcxoVoltage = 3.0f;
  s_status.radio_error = s_radio.begin(868.0f, 125.0f, 12, 5, 0x34,
                                      10, 20, 3.0f, true);
  s_status.radio_present = (s_status.radio_error == RADIOLIB_ERR_NONE);
  if (s_status.radio_present) {
    s_status.state_error = s_radio.standby();
    ESP_LOGI(TAG, "SX1262 respondeu (standby=%d)", s_status.state_error);
  } else {
    ESP_LOGW(TAG, "SX1262 nao respondeu nos pins padrao (erro %d)",
             s_status.radio_error);
  }
  return s_status.radio_present;
}

bool tab5_lora_present(void) { return s_status.radio_present; }

tab5_lora_status_t tab5_lora_status(void) { return s_status; }

size_t tab5_lora_pins(tab5_lora_pin_t* output, size_t capacity)
{
  const size_t n = sizeof(s_pinos) / sizeof(s_pinos[0]);
  if (output && capacity) {
    const size_t copiar = capacity < n ? capacity : n;
    memcpy(output, s_pinos, copiar * sizeof(*output));
  }
  return n;
}

const char* tab5_lora_pin_state_name(tab5_lora_pin_state_t state)
{
  switch (state) {
    case TAB5_LORA_PIN_LOW:     return "baixo";
    case TAB5_LORA_PIN_HIGH:      return "high";
    case TAB5_LORA_PIN_FLOATING: return "floating";
    case TAB5_LORA_PIN_I2C_LOW: return "I2C baixo";
    case TAB5_LORA_PIN_I2C_HIGH:  return "I2C high";
    default:                      return "?";
  }
}
