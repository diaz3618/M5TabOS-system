#include "tab5_gps.hpp"
#include "tab5_mutex.hpp"
#include "tab5_owners.hpp"
#include "task_pool.h"
#include "service_stop.hpp"      // parada cooperativa da tarefa (kernel)
#include "tab5_grove.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tab5_time.hpp"   // tab5_now_ms
#include <string.h>

static const char* TAG = "gps";

// ---- fiacao do GPS no Grove (Port A do Tab5) -------------------------------
// MOVIDA de app_azimutal.cpp - o modulo e o mesmo, so who liga
// a UART mudou de "o app, ao abrir" para "o service, quando o Grove tem um
// GPS de verdade".
static const uart_port_t GPS_UART   = UART_NUM_1;
static const int         GPS_RX_PIN = 54;   // GPIO54 = SCL do Port A, recebe o TX do GPS
static const int         GPS_TX_PIN = 53;   // GPIO53 = SDA do Port A
static const int         GPS_BAUD   = 9600;

static bool     s_servico_ativo = false;
static parada_t s_parada = { };   // pedido de parada e confirmacao de output da tarefa

static nmea_position_t s_pos   = { };
static nmea_time_t   s_tempo = { };
static uint32_t       s_ultima_sentenca_ms = 0;
static uint32_t       s_iniciado_em_ms     = 0;   // ver o aviso em tab5_gps_silence_ms()

static QueueHandle_t s_assinante = nullptr;   // who quiser sentenca crua (Azimutal)

// TRAVA PARA s_pos/s_tempo - achado ao revisar "ready para
// mais de um consumidor ao mesmo time": tab5_gps_position()/tab5_gps_time()
// ja SAO documentadas (ver o .hpp) como "qualquer app pode perguntar onde
// estou", mas devolviam o struct inteiro (dois double, dois float, flags -
// nao cabe numa instrucao so) sem nenhuma protecao contra a tarefa do GPS
// escrever no storage da copia. Com UM leitor de cada vez isso quase nunca se
// nota (a position fica errada por um frame so); a intencao declarada e
// varios apps lendo ao mesmo time, e ai a chance de pegar um value
// "rasgado" (latitude nova com longitude velha) sobe. Lazy-create e nunca
// destruida, MESMO MOLDE de services/notification.cpp - o primeiro uso e
// sempre antes de qualquer disputa de verdade (o boot chama tab5_gps_start
// antes de qualquer app poder read), entao criar aqui e seguro.
static SemaphoreHandle_t s_trava = nullptr;


static inline void travar(void)
{
  tab5_mutex_travar(&s_trava);   // criado sem corrida na primeira vez
}
static inline void destravar(void) { if (s_trava) { xSemaphoreGive(s_trava); } }

nmea_position_t tab5_gps_position(void)
{
  travar();
  nmea_position_t copia = s_pos;
  destravar();
  return copia;
}

nmea_time_t tab5_gps_time(void)
{
  travar();
  nmea_time_t copia = s_tempo;
  destravar();
  return copia;
}

bool tab5_gps_service_active(void) { return s_servico_ativo; }

bool tab5_gps_on_air(void)
{
  return s_ultima_sentenca_ms != 0 && (tab5_now_ms() - s_ultima_sentenca_ms) < 15000;
}

uint32_t tab5_gps_silence_ms(void)
{
  if (s_ultima_sentenca_ms == 0) {
    // NENHUMA sentenca AINDA - mas isso pode ser porque o service acabou de
    // start agora mesmo, nao porque o modulo sumiu. Bug seen em hardware
    // return_card UINT32_MAX aqui incondicionalmente faz a vigia
    // do Grove (kernel/core.cpp, "GPS mudo ha Xs") disparar no PRIMEIRO tique
    // (ate 2s) depois de QUALQUER reinicio - antes do modulo sequer ter
    // tido a CHANCE de falar dentro do prazo de 5s que o code achava que
    // estava dando. Resultado: service sobe, e derrubado 2s depois por
    // "mudo", sobe nvo, e derrubado de novo - um loop sem end, nunca da
    // time real para o GPS responder.
    //
    // A conta certa e a partir de QUANDO O SERVICO SUBIU, nao um value
    // sentinela fixo - assim o chamador (a vigia) ve o silencio crescendo
    // de verdade e so desconfia depois do prazo INTEIRO ter passado, igual
    // ja acontece quando ele fala e depois some.
    return s_servico_ativo ? (tab5_now_ms() - s_iniciado_em_ms) : UINT32_MAX;
  }
  return tab5_now_ms() - s_ultima_sentenca_ms;
}

bool tab5_gps_subscribe(QueueHandle_t queue)
{
  if (s_assinante != nullptr) { return false; }   // um assinante so, ver o header
  s_assinante = queue;
  return true;
}

void tab5_gps_unsubscribe(void) { s_assinante = nullptr; }

// ---------------------------------------------------------------------------
// A TAREFA - le bytes da UART, monta sentencas, parseia RMC/GGA e repassa a
// sentenca crua para who assinou. E a MESMA maquina de estados por
// caractere que o app_azimutal usava (laco_gps + o dispatch do laco_parser),
// so que agora e um loop so: nao ha mais uma queue entre "read" e "parsear"
// porque e a MESMA tarefa que faz as duas coisas.
// A TAREFA DORME NA FILA DE EVENTOS DA UART, nao num prazo curto. Era
// uart_read_bytes com 50 ms: 20 despertares por segundo para um modulo que
// manda uma rajada por segundo a 9600 baud. O driver avisa (UART_DATA) quando
// o FIFO enche ou a linha fica quieta - poucos avisos por rajada - e a tarefa
// le tudo o que ja chegou de uma vez. Para stop, o tab5_gps_stop cutuca a
// queue (gps_cutucar) e a tarefa ve o pedido na hora.
static QueueHandle_t s_uart_ev = nullptr;   // criada pelo driver da UART

static void gps_cutucar(void)
{
  if (!s_uart_ev) { return; }
  uart_event_t e = { };
  e.type = UART_EVENT_MAX;   // nao e evento do driver: so acorda a tarefa
  xQueueSend(s_uart_ev, &e, 0);
}

static void vTaskGPS_corpo(void*)
{
  char buffer[NMEA_MAX_LENGTH];
  int  idx = 0;
  bool gravando = false;

  static uint8_t input[256];   // leitura em bloco - ver o comentario que
                                  // existia em laco_gps sobre o cost por byte

  while (!parada_pedida(&s_parada)) {
    uart_event_t ev;
    if (xQueueReceive(s_uart_ev, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) { continue; }
    if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
      // Ninguem leu a time: o que esta no buffer e uma sentenca cortada.
      uart_flush_input(GPS_UART);
      xQueueReset(s_uart_ev);
      gravando = false; idx = 0;
      continue;
    }
    if (ev.type != UART_DATA) { continue; }   // o cutucao da parada, ou outro aviso
    int lidos;
    while ((lidos = uart_read_bytes(GPS_UART, input, sizeof(input), 0)) > 0) {   // tudo o que ja chegou
      for (int k = 0; k < lidos; ++k) {
        const char c = (char)input[k];

        if (c == '$') {
          gravando = true;
          idx = 0;
          buffer[idx++] = c;
          continue;
        }
        if (!gravando) { continue; }
        if (idx >= NMEA_MAX_LENGTH - 1) { gravando = false; idx = 0; continue; }

        buffer[idx++] = c;

        if (c == '\n') {
          buffer[idx] = '\0';
          gravando = false;
          idx = 0;

          if (!nmea_checksum_ok(buffer)) {
            ESP_LOGD(TAG, "checksum invalido: %s", buffer);
            continue;
          }

          s_ultima_sentenca_ms = tab5_now_ms();

          // Os 2 primeiros caracteres depois do '$' mudam por constelacao
          // (GP=GPS, GL=GLONASS...); por isso comparamos a partir do 3o.
          if (strncmp(buffer + 3, "RMC", 3) == 0) {
            nmea_time_t t;
            if (nmea_parse_rmc(buffer, &t)) { travar(); s_tempo = t; destravar(); }
          } else if (strncmp(buffer + 3, "GGA", 3) == 0) {
            nmea_position_t p;
            if (nmea_parse_gga(buffer, &p)) { travar(); s_pos = p; destravar(); }
          }

          // TODA sentenca valid vai para who assinou, inclusive RMC/GGA que
          // acabamos de processar - o Azimutal filtra GSV sozinho, como sempre
          // fez; nao vale a pena este service saber o que cada assinante quer.
          if (s_assinante) {
            NMEARawSentence_t crua;
            strncpy(crua.sentence, buffer, NMEA_MAX_LENGTH - 1);
            crua.sentence[NMEA_MAX_LENGTH - 1] = '\0';
            xQueueSend(s_assinante, &crua, 0);   // nao espera: perder 1 sentenca
                                                  // e melhor que travar a leitura
          }
        }
      }
    }
  }

  parada_confirmar(&s_parada);
  // Retorna: a vaga do pool devolve o owner e volta a dormir (kernel/task_pool.h).
}

bool tab5_gps_stop(void)
{
  if (!s_servico_ativo) { return true; }

  ESP_LOGI(TAG, "parando o service...");
  // A tarefa le a UART em fatias de 50 ms: a output custa isso no pior caso.
  if (!parada_pedir(&s_parada, 1000, gps_cutucar)) {
    ESP_LOGE(TAG, "a tarefa nao confirmou em 1s - segue parando, nada foi desmontado");
    return false;
  }

  uart_driver_delete(GPS_UART);
  s_uart_ev = nullptr;   // a queue de eventos vai junto com o driver
  tab5_grove_release(GROVE_OWNER_GPS);

  s_ultima_sentenca_ms = 0;
  travar();
  memset(&s_pos,   0, sizeof(s_pos));
  memset(&s_tempo, 0, sizeof(s_tempo));
  destravar();

  s_servico_ativo = false;
  ESP_LOGI(TAG, "service parado, Grove free_space");
  return true;
}

bool tab5_gps_start(void)
{
  if (s_servico_ativo) {
    if (parada_em_curso(&s_parada)) {
      ESP_LOGW(TAG, "ainda parando o service anterior - nao vou start por cima");
    } else {
      ESP_LOGI(TAG, "service ja esta de pe - nada a fazer");
    }
    return true;
  }

  if (!tab5_grove_acquire(GROVE_OWNER_GPS)) {
    ESP_LOGE(TAG, "Grove ocupado - service nao vai start");
    return false;
  }

  uart_config_t cfg = { };
  cfg.baud_rate  = GPS_BAUD;
  cfg.data_bits  = UART_DATA_8_BITS;
  cfg.parity     = UART_PARITY_DISABLE;
  cfg.stop_bits  = UART_STOP_BITS_1;
  cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  // NAO mexa no M5.Ex_I2C aqui - ver o mesmo aviso que existia em
  // app_azimutal.cpp: o keyboard do ExtPort1 e who usa esse barramento hoje,
  // e release/reiniciar o Ex_I2C derrubaria ele.
  if (uart_driver_install(GPS_UART, 2048, 0, 16, &s_uart_ev, 0) != ESP_OK
   || uart_param_config(GPS_UART, &cfg) != ESP_OK
   || uart_set_pin(GPS_UART, GPS_TX_PIN, GPS_RX_PIN,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
  {
    ESP_LOGE(TAG, "nao consegui abrir a UART do Grove");
    uart_driver_delete(GPS_UART);
    s_uart_ev = nullptr;   // a queue de eventos vai junto com o driver
    tab5_grove_release(GROVE_OWNER_GPS);
    return false;
  }

  travar();
  memset(&s_pos,   0, sizeof(s_pos));
  memset(&s_tempo, 0, sizeof(s_tempo));
  destravar();
  s_ultima_sentenca_ms = 0;

  if (!parada_preparar(&s_parada)) {
    uart_driver_delete(GPS_UART);
    s_uart_ev = nullptr;   // a queue de eventos vai junto com o driver
    tab5_grove_release(GROVE_OWNER_GPS);
    ESP_LOGE(TAG, "sem memoria para a tarefa do GPS");
    return false;
  }
  s_iniciado_em_ms = tab5_now_ms();   // ver o aviso em tab5_gps_silence_ms()
  s_servico_ativo  = true;

  // Prioridade 3, mesma do service Mesh: abaixo da interface (4), acima do
  // registro (1) - perder byte de serial e irreversivel.
  if (!pool_criar(nullptr, vTaskGPS_corpo, nullptr, "gps", 4096, 3, OWNER_SERVICE, "GPS")) {
    parada_desistir(&s_parada);
    s_servico_ativo = false;
    uart_driver_delete(GPS_UART);
    s_uart_ev = nullptr;   // a queue de eventos vai junto com o driver
    tab5_grove_release(GROVE_OWNER_GPS);
    ESP_LOGE(TAG, "sem memoria para a tarefa do GPS");
    return false;
  }
  ESP_LOGI(TAG, "service no ar (Grove %d baud, RX=%d TX=%d)",
           GPS_BAUD, GPS_RX_PIN, GPS_TX_PIN);
  return true;
}
