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

// Mesmos pins do Azimutal - o Grove e um so.
#define GROVE_UART   UART_NUM_1
#define GROVE_RX     54
#define GROVE_TX     53

// Tempo para o modulo start depois de receber energia. O C6L tem um ESP32-C6
// que boota o Meshtastic inteiro; 1,2s e folgado para ele chegar ao point de
// responder. Curto demais aqui faz o C6L parecer ausente.
// Tempo para o modulo start DEPOIS de receber energia.
//
// 1,2s bastava para um GPS, que comeca a falar quase na hora. NAO basta para o
// Unit C6L: ele roda o Meshtastic inteiro num ESP32-C6 e leva segundos. Com a
// espera curta ele era reiniciado a cada sondagem e nunca chegava a responder -
// a sonda dizia "nada" com o modulo plugado e funcionando.
//
// Se o trilho JA estiver on (ajuste "Grove sempre on"), nao ha boot
// nenhum a esperar e a sonda e imediata.
#define ESPERA_BOOT_MS   4000

// Quanto escutar em cada baud. O GPS manda uma sentenca por segundo no pior
// caso, entao 1,5s garante pelo menos uma.
#define ESCUTA_MS        1500

static grove_type_t s_tipo = GROVE_NOTHING;
static grove_owner_t s_dono = GROVE_OWNER_NONE;

const char* grove_owner_name(grove_owner_t d)
{
  switch (d) {
    case GROVE_OWNER_MESH: return "service Meshtastic";
    case GROVE_OWNER_GPS:  return "GPS";
    default:              return "ninguem";
  }
}

grove_owner_t tab5_grove_owner(void) { return s_dono; }

bool tab5_grove_acquire(grove_owner_t who)
{
  if (s_dono != GROVE_OWNER_NONE && s_dono != who) {
    ESP_LOGW(TAG, "Grove ja e do %s - %s nao pode assumir",
             grove_owner_name(s_dono), grove_owner_name(who));
    return false;
  }
  s_dono = who;
  ESP_LOGI(TAG, "Grove agora e do %s", grove_owner_name(who));
  return true;
}

void tab5_grove_release(grove_owner_t who)
{
  // So o owner solta. Sem esta guarda, um app que fechasse depois de ter
  // FALHADO em acquire liberaria o Grove de who estava usando de verdade.
  if (s_dono != who) { return; }
  s_dono = GROVE_OWNER_NONE;
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

grove_type_t tab5_grove_type(void) { return s_tipo; }

static bool abrir(int baud)
{
  // Cala o driver da UART durante a sonda.
  //
  // O M5.Ex_I2C e inicializado no boot nos pins do Grove (54/53) e RESERVA os
  // GPIOs. Quando o tab5_keyboard reaponta esse barramento para o ExtPort1, o
  // release() do M5GFX solta o barramento mas nao devolve a reserva - ela fica
  // pendurada. O uart_set_pin entao avisa "GPIO 53 is not usable" e configura o
  // pino assim mesmo (a sonda funciona: medimos 209 bytes de NMEA com o aviso
  // aparecendo). E ruido conhecido, calado so aqui.
  esp_log_level_set("uart", ESP_LOG_ERROR);

  uart_config_t cfg = { };
  cfg.baud_rate = baud;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity    = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  tab5_mark("grove: UART a", baud);   // instala o driver - e a trava de PM dele
  if (uart_driver_install(GROVE_UART, 2048, 512, 0, NULL, 0) != ESP_OK) {
    esp_log_level_set("uart", ESP_LOG_WARN); return false;
  }
  if (uart_param_config(GROVE_UART, &cfg) != ESP_OK) {
    uart_driver_delete(GROVE_UART); esp_log_level_set("uart", ESP_LOG_WARN); return false;
  }
  // Atencao a ordem: uart_set_pin recebe TX antes de RX.
  if (uart_set_pin(GROVE_UART, GROVE_TX, GROVE_RX,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
  {
    uart_driver_delete(GROVE_UART);
    esp_log_level_set("uart", ESP_LOG_WARN);
    return false;
  }
  return true;
}

static void fechar(void)
{
  uart_driver_delete(GROVE_UART);
  esp_log_level_set("uart", ESP_LOG_WARN);   // fora da sonda o aviso volta a valer
}

// Le o que chegar na janela e devolve quantos bytes pegou.
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

// ---------------------------------------------------------------------------
// Sonda do Meshtastic (115200, ativa)
// ---------------------------------------------------------------------------
// Pede ao Meshtastic que se apresente. Chamada com a UART JA open.
//
// Isto e obrigatorio: em mode PROTO o radio NAO fala sozinho, so responde. Uma
// escuta passiva devolve zero bytes mesmo com tudo funcionando - foi o que me
// enganou ao usar o despejo raw para testar o C6L.
//
// Mandar estes bytes para um GPS e inofensivo: ele ignora o que nao entende.
static void pedir_apresentacao(void)
{
  // O StreamAPI do Meshtastic pede que o cliente acorde a porta com 32 bytes
  // de START1 antes do primeiro pedido - o radio pode estar em mode de baixo
  // consumo na serial e perder os primeiros bytes.
  uint8_t acorda[32];
  memset(acorda, 0x94, sizeof(acorda));
  uart_write_bytes(GROVE_UART, acorda, sizeof(acorda));
  vTaskDelay(pdMS_TO_TICKS(120));

  // ToRadio{ want_config_id = 0x2A }  -- protobuf raw, mounted a mao:
  //   campo 3 (want_config_id), varint  -> tag 0x18, value 0x2A
  // Enquadrado: 0x94 0xC3 <tam_hi> <tam_lo> <payload>
  const uint8_t quadro[] = { 0x94, 0xC3, 0x00, 0x02, 0x18, 0x2A };
  uart_write_bytes(GROVE_UART, quadro, sizeof(quadro));
}

static bool sondar_meshtastic(void)
{
  if (!abrir(115200)) { return false; }
  pedir_apresentacao();

  uint8_t buf[512];
  const int n = escutar(buf, sizeof(buf), ESCUTA_MS);
  fechar();

  ESP_LOGI(TAG, "sondagem a 115200 baud (Meshtastic?): %d byte(s) recebidos", n);
  for (int i = 0; i + 1 < n; ++i)
  {
    if (buf[i] == 0x94 && buf[i+1] == 0xC3) { return true; }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Sonda do GPS (9600, passiva)
// ---------------------------------------------------------------------------
static bool sondar_gps(int* bytes_vistos)
{
  if (!abrir(9600)) { return false; }
  uint8_t buf[512];
  const int n = escutar(buf, sizeof(buf), ESCUTA_MS);
  fechar();

  *bytes_vistos = n;
  ESP_LOGI(TAG, "sondagem a 9600 baud (GPS?): %d byte(s) recebidos", n);

  // Assinatura NMEA: '$' seguido de duas letras maiusculas (GP, GL, GN, GA...).
  // Exigir as letras evita casar com um '$' que caiu no storage de lixo binario.
  for (int i = 0; i + 2 < n; ++i)
  {
    if (buf[i] == '$' && buf[i+1] >= 'A' && buf[i+1] <= 'Z'
                      && buf[i+2] >= 'A' && buf[i+2] <= 'Z') { return true; }
  }
  return false;
}

// ---------------------------------------------------------------------------

void tab5_grove_dump(int baud, int ms)
{
  // PERGUNTA ANTES DE LIGAR - senao a resposta seria sempre "sim" (o proprio
  // tab5_power_grove(true) logo abaixo). Estado de hardware de verdade, nao
  // preferencia (ver o comentario em tab5_power_grove_on()).
  const bool ja_ligado = tab5_power_grove_on();
  tab5_power_grove(true);
  // Se o trilho acabou de start, o modulo esta bootando: esperar. Ver o
  // comentario de ESPERA_BOOT_MS - foi isso que fez o C6L parecer mudo.
  vTaskDelay(pdMS_TO_TICKS(ja_ligado ? 300 : ESPERA_BOOT_MS));

  if (!abrir(baud)) { printf("nao consegui abrir a UART a %d\n", baud); return; }

  // Pede apresentacao SEMPRE. Para o Meshtastic e obrigatorio (ele nao fala
  // sozinho); para um GPS e inofensivo. Sem isto o despejo mente: mostra zero
  // bytes num modulo que esta perfeito, so calado.
  pedir_apresentacao();

  static uint8_t buf[512];
  const int n = escutar(buf, sizeof(buf), ms);
  fechar();

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

// TEM ALGUEM NO CABO? Uma leitura de GPIO, e so.
//
// A sondagem de verdade custa 3 SEGUNDOS - liga a UART, escuta 1,5s a 9600,
// troca de baud, pede apresentacao e escuta mais 1,5s a 115200. Isso nao pode
// rodar em loop: e o que fez a deteccao virar decisao de boot, feita uma vez e
// nunca mais revista.
//
// Mas identificar QUEM esta la e caro; perceber que ALGUEM chegou e barato. A
// linha de data denuncia sozinha: em UART, sleep e nivel ALTO, e todo modulo
// mantem o TX dele nesse nivel mesmo sem falar nada. Sem modulo, o pino fica
// solto.
//
// Entao: pull-down interno on e uma leitura.
//   modulo present -> o TX dele (push-pull) vence o pull-down  -> ALTO
//   nada no cabo    -> so o pull-down sustenta o pino           -> BAIXO
//
// Custo: um gpio_config e um gpio_get_level. A 0,5Hz e ruido estatistico ao
// lado de qualquer coisa que a tela faca.
//
// SO COM A PORTA LIVRE. Com a UART instalada o pino e do driver dela, e
// reconfigurar por baixo quebraria a leitura de who esta usando.
bool tab5_grove_present(void)
{
  gpio_config_t c = { };
  c.pin_bit_mask = (1ULL << GROVE_RX);
  c.mode         = GPIO_MODE_INPUT;
  c.pull_up_en   = GPIO_PULLUP_DISABLE;
  c.pull_down_en = GPIO_PULLDOWN_ENABLE;
  c.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&c);

  // O pull-down e fraco (dezenas de kohm) e precisa descarregar a capacitancia
  // do pino e do cabo. Sem esta pausa a primeira leitura pega o nivel VELHO -
  // e um modulo recem-tirado ainda pareceria present.
  esp_rom_delay_us(500);

  return gpio_get_level((gpio_num_t)GROVE_RX) != 0;
}

// Esquece o que foi detectado. Para quando o cabo fica vazio: nao ha o que
// identificar, entao nao se paga sondagem nenhuma para concluir "nada".
void tab5_grove_forget(void) { s_tipo = GROVE_NOTHING; }

grove_type_t tab5_grove_detect(void)
{
  ESP_LOGI(TAG, "sondando o Grove...");

  // Energia primeiro: sem os 5V nenhum modulo se apresenta, e a sonda diria
  // "nada" com o aparelho plugado - o pior type de resposta errada.
  //
  // ESTADO DE HARDWARE, nao a preferencia "manter sempre on" - as duas
  // costumavam coincidir, mas nao sao a mesma pergunta (o trilho pode estar
  // on agora por outro motivo mesmo com a preferencia desligada). Corrigido
  // junto com a auditoria de camadas - HAL pergunta a HAL, nunca
  // a um service (ver WRITING_APPS.md secao 8).
  if (tab5_power_grove_on()) {
    ESP_LOGI(TAG, "trilho ja estava on - sondando sem esperar boot");
  } else {
    tab5_power_grove(true);
    ESP_LOGI(TAG, "liguei o trilho agora - esperando %dms o modulo bootar", ESPERA_BOOT_MS);
    vTaskDelay(pdMS_TO_TICKS(ESPERA_BOOT_MS));
  }

  // Meshtastic ANTES do GPS. Motivo: a sonda dele escreve na linha, e escrever
  // num GPS e inofensivo (ele ignora), enquanto o contrario nao vale - o
  // Meshtastic calado passaria por "nada" se testassemos so escutando.
  if (sondar_meshtastic()) { s_tipo = GROVE_MESHTASTIC; }
  else
  {
    int seen_devices = 0;
    if (sondar_gps(&seen_devices))  { s_tipo = GROVE_GPS; }
    else if (seen_devices > 0)      { s_tipo = GROVE_UNKNOWN; }
    else                      { s_tipo = GROVE_NOTHING; }
  }

  ESP_LOGI(TAG, "Grove: %s", grove_name(s_tipo));
  return s_tipo;
}
