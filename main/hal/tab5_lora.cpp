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

// ROTA PADRAO, toda reunida aqui para ser facil trocar depois da sondagem.
// E a posicao demonstrada pela M5Stack no CoreS3, transposta pelos numeros
// fisicos do M5-Bus: IRQ=2, BUSY=19, NSS=20.
static constexpr int PINO_IRQ_PADRAO  = 16;
static constexpr int PINO_BUSY_PADRAO = 3;
static constexpr int PINO_NSS_PADRAO  = 4;

static constexpr uint32_t I2C_HZ = 100000;

// Registradores do M5IOE1. So usamos a pequena parte GPIO para nao criar um
// segundo dono do barramento nem puxar outro driver I2C em paralelo.
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

static tab5_lora_pin_t s_pinos[] = {
  {  2, 16, TAB5_LORA_PIN_BAIXO }, {  4, 17, TAB5_LORA_PIN_BAIXO },
  {  8, 45, TAB5_LORA_PIN_BAIXO }, { 10, 52, TAB5_LORA_PIN_BAIXO },
  { 16,  6, TAB5_LORA_PIN_BAIXO }, { 17, 31, TAB5_LORA_PIN_I2C_ALTO },
  { 18, 32, TAB5_LORA_PIN_I2C_ALTO }, { 19,  3, TAB5_LORA_PIN_BAIXO },
  { 20,  4, TAB5_LORA_PIN_BAIXO }, { 21,  2, TAB5_LORA_PIN_BAIXO },
  { 22, 48, TAB5_LORA_PIN_BAIXO }, { 23, 47, TAB5_LORA_PIN_BAIXO },
  { 24, 35, TAB5_LORA_PIN_BAIXO }, { 26, 51, TAB5_LORA_PIN_BAIXO },
};

static EspHal s_hal(PINO_SCK, PINO_MISO, PINO_MOSI, SPI2_HOST, 2000000);
static Module s_modulo(&s_hal, PINO_NSS_PADRAO, PINO_IRQ_PADRAO,
                       RADIOLIB_NC, PINO_BUSY_PADRAO);
static SX1262 s_radio(&s_modulo);

static bool ler16(uint8_t endereco, uint8_t reg, uint16_t* valor)
{
  uint8_t b[2] = { };
  if (!M5.In_I2C.readRegister(endereco, reg, b, sizeof(b), I2C_HZ)) { return false; }
  *valor = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  return true;
}

static bool escrever16(uint8_t endereco, uint8_t reg, uint16_t valor)
{
  const uint8_t b[2] = { (uint8_t)valor, (uint8_t)(valor >> 8) };
  return M5.In_I2C.writeRegister(endereco, reg, b, sizeof(b), I2C_HZ);
}

static bool alterar16(uint8_t endereco, uint8_t reg, uint16_t ligar, uint16_t desligar)
{
  uint16_t valor;
  if (!ler16(endereco, reg, &valor)) { return false; }
  valor = (uint16_t)((valor | ligar) & ~desligar);
  return escrever16(endereco, reg, valor);
}

static bool saidas_controle(uint8_t endereco)
{
  // MODO=1 e saida; DRIVE=0 e push-pull. Tirar pulls evita uma corrente
  // escondida enquanto o trilho do radio esta desligado. O valor seguro vai
  // primeiro ao latch, para a troca para saida nao criar um pulso de energia.
  return alterar16(endereco, REG_PULLUP_L, 0, IO_CONTROLE)
      && alterar16(endereco, REG_PULLDN_L, 0, IO_CONTROLE)
      && alterar16(endereco, REG_DRIVE_L, 0, IO_CONTROLE)
      && alterar16(endereco, REG_SAIDA_L, IO_BYPASS,
                   IO_CONTROLE & ~IO_BYPASS)
      && alterar16(endereco, REG_MODO_L, IO_CONTROLE, 0);
}

static bool escrever_controle(uint8_t endereco, uint16_t altos)
{
  return alterar16(endereco, REG_SAIDA_L, altos & IO_CONTROLE,
                   IO_CONTROLE & ~altos);
}

static void sondar_ioe(void)
{
  s_status.ioe_endereco = 0;
  s_status.ioe_mascara = 0;
  s_status.ioe_uid = 0;
  s_status.ioe_revisao = 0;

  for (uint8_t endereco = 0x71; endereco <= 0x74; ++endereco) {
    if (!M5.In_I2C.scanID(endereco, I2C_HZ)) { continue; }
    // UID e revisao distinguem resposta real do M5IOE1 de outro dispositivo
    // improvavel que tenha caido na mesma faixa.
    uint16_t uid;
    uint8_t rev;
    if (!ler16(endereco, REG_UID_L, &uid)
     || !M5.In_I2C.readRegister(endereco, REG_REV, &rev, 1, I2C_HZ)) { continue; }
    s_status.ioe_mascara |= (uint8_t)(1u << (endereco - 0x71));
    if (!s_status.ioe_endereco) {
      s_status.ioe_endereco = endereco;
      s_status.ioe_uid = uid;
      s_status.ioe_revisao = rev;
    }
    ESP_LOGI(TAG, "M5IOE1 respondeu em 0x%02X (UID %04X, rev %02X)",
             endereco, uid, rev);
  }
  if (!s_status.ioe_endereco) {
    ESP_LOGW(TAG, "nenhum M5IOE1 respondeu entre 0x71 e 0x74");
  }
}

static tab5_lora_pin_estado_t medir_pino(int gpio)
{
  gpio_config_t c = { };
  c.pin_bit_mask = 1ULL << gpio;
  c.mode = GPIO_MODE_INPUT;
  c.intr_type = GPIO_INTR_DISABLE;

  c.pull_down_en = GPIO_PULLDOWN_ENABLE;
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  if (gpio_config(&c) != ESP_OK) { return TAB5_LORA_PIN_FLUTUANTE; }
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

  if (!com_baixo && com_alto) { return TAB5_LORA_PIN_FLUTUANTE; }
  return com_baixo ? TAB5_LORA_PIN_ALTO : TAB5_LORA_PIN_BAIXO;
}

static void sondar_pinos(void)
{
  for (size_t i = 0; i < sizeof(s_pinos) / sizeof(s_pinos[0]); ++i) {
    // SDA/SCL ja sustentam touch, IMU, energia e este proprio expansor.
    // Reconfigura-los para testar flutuacao derrubaria o barramento.
    if (s_pinos[i].mbus == 17 || s_pinos[i].mbus == 18) {
      s_pinos[i].estado = gpio_get_level((gpio_num_t)s_pinos[i].gpio)
                           ? TAB5_LORA_PIN_I2C_ALTO
                           : TAB5_LORA_PIN_I2C_BAIXO;
    } else {
      s_pinos[i].estado = medir_pino(s_pinos[i].gpio);
    }
  }
}

void tab5_lora_sondar(void)
{
  sondar_pinos();
  sondar_ioe();
  s_status.sondado = true;
}

bool tab5_lora_iniciar(void)
{
  tab5_lora_sondar();
  s_status.radio_presente = false;
  s_status.erro_estado = RADIOLIB_ERR_UNKNOWN;

  if (!s_status.ioe_endereco) {
    s_status.erro_radio = RADIOLIB_ERR_CHIP_NOT_FOUND;
    return false;
  }

  const uint8_t e = s_status.ioe_endereco;
  if (!saidas_controle(e)
   || !escrever_controle(e, IO_BYPASS)) {  // reset baixo, energia baixa
    s_status.erro_radio = RADIOLIB_ERR_SPI_WRITE_FAILED;
    ESP_LOGE(TAG, "falhou a preparacao do M5IOE1 em 0x%02X", e);
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(25));
  if (!escrever_controle(e, IO_BYPASS | IO_PWR_EN)) {
    s_status.erro_radio = RADIOLIB_ERR_SPI_WRITE_FAILED;
    ESP_LOGE(TAG, "falhou ao ligar a energia do radio pelo M5IOE1");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(120));
  if (!escrever_controle(e, IO_BYPASS | IO_PWR_EN | IO_RST)) {
    s_status.erro_radio = RADIOLIB_ERR_SPI_WRITE_FAILED;
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(120));

  // O exemplo oficial do proprio modulo usa TCXO a 3,0 V e reset externo
  // pelo M5IOE1. O begin tambem faz a leitura que prova que ha um SX1262.
  s_radio.tcxoVoltage = 3.0f;
  s_status.erro_radio = s_radio.begin(868.0f, 125.0f, 12, 5, 0x34,
                                      10, 20, 3.0f, true);
  s_status.radio_presente = (s_status.erro_radio == RADIOLIB_ERR_NONE);
  if (s_status.radio_presente) {
    s_status.erro_estado = s_radio.standby();
    ESP_LOGI(TAG, "SX1262 respondeu (standby=%d)", s_status.erro_estado);
  } else {
    ESP_LOGW(TAG, "SX1262 nao respondeu nos pinos padrao (erro %d)",
             s_status.erro_radio);
  }
  return s_status.radio_presente;
}

bool tab5_lora_presente(void) { return s_status.radio_presente; }

tab5_lora_status_t tab5_lora_status(void) { return s_status; }

size_t tab5_lora_pinos(tab5_lora_pin_t* saida, size_t capacidade)
{
  const size_t n = sizeof(s_pinos) / sizeof(s_pinos[0]);
  if (saida && capacidade) {
    const size_t copiar = capacidade < n ? capacidade : n;
    memcpy(saida, s_pinos, copiar * sizeof(*saida));
  }
  return n;
}

const char* tab5_lora_pin_estado_nome(tab5_lora_pin_estado_t estado)
{
  switch (estado) {
    case TAB5_LORA_PIN_BAIXO:     return "baixo";
    case TAB5_LORA_PIN_ALTO:      return "alto";
    case TAB5_LORA_PIN_FLUTUANTE: return "flutuante";
    case TAB5_LORA_PIN_I2C_BAIXO: return "I2C baixo";
    case TAB5_LORA_PIN_I2C_ALTO:  return "I2C alto";
    default:                      return "?";
  }
}
