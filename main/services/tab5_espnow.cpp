#include "tab5_espnow.hpp"
#include "tab5_sniffer.hpp"   // repassa o PRONTO (versao e capacidades of C6)
#include "tab5_owners.hpp"
#include "tab5_wifi.hpp"
#include "tab5_sd.hpp"
#include "tab5_power.hpp"   // tab5_power_is_sleeping
#include "notification.h"
#include "settings.h"
#include "app.h"
#include "tab5_time.hpp"

#include "eh_host_feat_peer_data.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <cstdio>
#include <cstring>
#include <ctime>

static const char* TAG = "espnow";

#define ESPNOW_PASTA     "ESPNOW"
#define ESPNOW_ARQUIVO   "historico.txt"
#define REPEDIR_MS       5000     // sem PRONTO, pede of novo
#define CANAL_MS         10000    // o canal segue o roteador: confere of tempos em tempos

// ---------------------------------------------------------------------------
// ESTADO - escrito pela tarefa of servico, lido pela interface (copia, trava)
// ---------------------------------------------------------------------------
static EXT_RAM_BSS_ATTR espnow_msg_t s_hist[ESPNOW_HIST];   // anel: s_ini e a mais antiga
static int      s_n = 0, s_ini = 0;
static uint32_t s_total = 0;
// Carimbada pelo clock DESTA sessao (as of cartao nao): so essas sao
// corrigidas when o clock salta - ver ao_saltar_relogio().
static bool     s_of_sessao[ESPNOW_HIST];
static bool     s_lendo_cartao = false;
static int      s_nao_lidas = 0;
static SemaphoreHandle_t s_trava = nullptr;

static volatile bool    s_pronto = false;
static volatile uint8_t s_canal  = 0;
static uint8_t          s_mac[6] = { };
static int              s_notif  = -1;

// Aparelhos ouvidos (so para count "quantos"), e o ultimo frame.
#define VISTOS_MAX 16
static uint8_t  s_vistos[VISTOS_MAX][6];
static int      s_n_vistos = 0;
static int64_t  s_ultimo_rx_us = 0;

// ENERGIA. O receptor of C6 so stays enabled o time todo when o
// ESP-NOW esta of fato em uso - ver quer_receptor_ligado(). Fora disso, modem
// sleep, como qualquer WiFi.
#define CONVERSA_VIVA_US (5LL * 60 * 1000000)   // 5 min since a ultima message
static int64_t       s_ultimo_tx_us = 0;
static volatile bool s_window = false;          // o app ESP-NOW esta open

static inline void lock(void)    { xSemaphoreTake(s_trava, portMAX_DELAY); }
static inline void destravar(void) { xSemaphoreGive(s_trava); }

// ---------------------------------------------------------------------------
// A QUEUE DO SERVICO - uma so, para o que chega of C6 e o que a interface pede
// ---------------------------------------------------------------------------
// Os notices of peer_data chegam in tarefa of recepcao of esp_hosted, que nao
// pode wait por nada (e ela que entrega TODO o trafego of WiFi). O callback
// so copia para a queue; quem pensa e a tarefa of servico. O send_state vem of
// interface pela MESMA queue, porque eh_host_peer_data_send() BLOQUEIA ate o C6
// responder (ate 5 s) - in interface isso seria screen congelada.
typedef struct {
  uint32_t type;                         // C6_MSG_* recebido, ou EV_ENVIAR
  uint16_t n;
  uint8_t  data[7 + C6_TEXTO_MAX + 1];
} evento_t;

#define EV_ENVIAR 0x1000
#define EV_CANAL  0x1001   // data[0] = canal fixo (0 = o of roteador)
#define EV_NADA   0x1002   // so acorda o loop (reavaliar o receptor)

static volatile uint8_t s_canal_fixo = 0;
static int s_notif_canal = -1;   // pastilha cinza enquanto o WiFi esta released of roteador

static QueueHandle_t s_fila = nullptr;

static void on_peer_data(uint32_t msg_id, const uint8_t* data, size_t n, void*)
{
  if (!s_fila) { return; }
  // ESTATICO: este callback roda in tarefa of recepcao of esp_hosted, cuja
  // stack nao e nossa - 270 bytes a mais nela e aposta. So essa tarefa chama
  // aqui (uma por vez), e o xQueueSend copia antes of voltar.
  static evento_t ev;
  ev.type = msg_id;
  ev.n = (uint16_t)(n > sizeof(ev.data) ? sizeof(ev.data) : n);
  if (data && ev.n) { memcpy(ev.data, data, ev.n); }
  xQueueSend(s_fila, &ev, 0);   // nunca espera: perder one frame e melhor que lock o WiFi
}

// ---------------------------------------------------------------------------
// HISTORICO
// ---------------------------------------------------------------------------
static void publicar_notif(void)
{
  if (s_notif < 0) { return; }
  char summary[NOTIF_RESUMO];
  if (s_nao_lidas > 0) {
    // Quem mandou a ultima: os dois ultimos bytes of MAC bastam para distinguir
    // os aparelhos of uma mesa.
    const espnow_msg_t* m = &s_hist[(s_ini + s_n - 1) % ESPNOW_HIST];
    snprintf(summary, sizeof(summary), "%d de %02X%02X", s_nao_lidas, m->mac[4], m->mac[5]);
  } else {
    summary[0] = 0;
  }
  notification_publicar(s_notif, s_nao_lidas, NOTIF_INFO, summary);
}

// Com a trava tomada. Devolve o indice (no anel) of message guardada.
static int guardar(const espnow_msg_t* m)
{
  int k;
  if (s_n < ESPNOW_HIST) { k = (s_ini + s_n) % ESPNOW_HIST; ++s_n; }
  else                   { k = s_ini; s_ini = (s_ini + 1) % ESPNOW_HIST; }
  s_hist[k] = *m;
  s_of_sessao[k] = !s_lendo_cartao;
  ++s_total;
  return k;
}

// Uma line por message: epoch, MAC, flags e o text - sem TAB nem quebra of
// line dentro dele. Flags: m/r (minha/recebida), d/u (difusao/direta) - EM
// MAIUSCULA (D/U) when veio no formato Espressif, o que mantem legivel o
// historico gravado antes of existir o dialeto -, e o estado of send_state.
// Pelo gravador of sistema (tab5_sd_enqueue_append, urgente): a tarefa of servico nao
// monta mais o cartao a cada message.
static void gravar_no_cartao(const espnow_msg_t* m)
{
  static char txt[sizeof(m->text)];   // estatico: so a tarefa of servico chama
  size_t k = 0;
  for (const char* c = m->text; *c && k < sizeof(txt) - 1; ++c) {
    txt[k++] = (*c == '\t' || *c == '\n' || *c == '\r') ? ' ' : *c;
  }
  txt[k] = 0;
  static char line[sizeof(txt) + 64];
  snprintf(line, sizeof(line), "%lu\t%02x%02x%02x%02x%02x%02x\t%c%c%u\t%s\n", (unsigned long)m->when,
           m->mac[0], m->mac[1], m->mac[2], m->mac[3], m->mac[4], m->mac[5],
           m->minha ? 'm' : 'r',
           m->espressif ? (m->difusao ? 'D' : 'U') : (m->difusao ? 'd' : 'u'),
           (unsigned)m->send_state, txt);
  tab5_sd_enqueue_append(ESPNOW_PASTA, ESPNOW_ARQUIVO, line, nullptr, true);
}

// No boot of servico: as ultimas ESPNOW_HIST mensagens of cartao voltam para a
// RAM, e a conversa abre com o que ja foi dito.
//
// SO O FIM DO ARQUIVO, E SEM A TRAVA DURANTE A LEITURA. O
// historico so cresce, e antes era lido INTEIRO com a trava presa - a
// interface, que pergunta pelas mensagens a cada drawing of app, ficava
// esperando o cartao, e cada vez mais conforme o arquivo engordasse. Agora:
// pula direto para os ultimos ~ESPNOW_HIST x 300 bytes (uma line nao passa
// of ~290: epoch, MAC, flags e ate 250 of text), descarta a primeira line
// (provavelmente cortada no middle) e trava so o instante of guardar cada uma.
#define ESPNOW_CAUDA_BYTES ((long)ESPNOW_HIST * 300)
static void carregar_of_cartao(void)
{
  if (!tab5_sd_mount()) { return; }
  s_lendo_cartao = true;
  FILE* f = tab5_sd_open(ESPNOW_PASTA, ESPNOW_ARQUIVO, "r");
  if (f) {
    static char line[400];   // estatico: a stack e of montagem of cartao
    int lidas = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
      const long size = ftell(f);
      if (size > ESPNOW_CAUDA_BYTES && fseek(f, size - ESPNOW_CAUDA_BYTES, SEEK_SET) == 0) {
        fgets(line, sizeof(line), f);   // o pedaco of line cortada
      } else {
        fseek(f, 0, SEEK_SET);
      }
    }
    while (fgets(line, sizeof(line), f)) {
      static espnow_msg_t m;
      memset(&m, 0, sizeof(m));
      unsigned long when = 0;
      unsigned int mac[6] = { };
      char quem = 0, dif = 0;
      unsigned int send_state = 0;
      int pos = 0;
      if (sscanf(line, "%lu\t%2x%2x%2x%2x%2x%2x\t%c%c%u\t%n", &when,
                 &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5],
                 &quem, &dif, &send_state, &pos) < 10 || pos <= 0) { continue; }
      m.when = (uint32_t)when;
      for (int i = 0; i < 6; ++i) { m.mac[i] = (uint8_t)mac[i]; }
      m.minha = (quem == 'm');
      m.difusao = (dif == 'd' || dif == 'D');
      m.espressif = (dif == 'D' || dif == 'U');   // sem isto, depois of boot everything virava "text puro"
      // Um send_state que became pending when o aparelho desligou nao vai ser
      // confirmado nunca mais: mostra como falhou, nao como "enviando" eterno.
      m.send_state = (uint8_t)((send_state == ESPNOW_ENVIO_PENDENTE) ? ESPNOW_ENVIO_FALHOU : send_state);
      snprintf(m.text, sizeof(m.text), "%s", line + pos);
      const size_t n = strlen(m.text);
      if (n && m.text[n - 1] == '\n') { m.text[n - 1] = 0; }
      lock();
      guardar(&m);
      destravar();
      ++lidas;
    }
    fclose(f);
    if (lidas) { ESP_LOGI(TAG, "card: %d message(s) in history", lidas); }
  }
  s_lendo_cartao = false;
  tab5_sd_unmount();
}

static void count_visto(const uint8_t mac[6])
{
  for (int i = 0; i < s_n_vistos; ++i) { if (memcmp(s_vistos[i], mac, 6) == 0) { return; } }
  if (s_n_vistos < VISTOS_MAX) { memcpy(s_vistos[s_n_vistos++], mac, 6); }
}

static uint32_t now_epoch(void) { return tab5_time_epoch(); }

// ---------------------------------------------------------------------------
// A TAREFA
// ---------------------------------------------------------------------------
static void tratar(const evento_t* ev);

// O ESP-NOW caiu (radio parado, C6 sem enlace): one send_state pending nao vai ser
// confirmado nunca - o C6 so responde pelo que ele mesmo tem in queue. Vira
// falha, igual on que o carregar_do_cartao faz no boot.
static void falhar_pendentes(void)
{
  for (;;) {
    bool achou = false;
    lock();
    for (int j = 0; j < s_n && !achou; ++j) {
      const espnow_msg_t* m = &s_hist[(s_ini + j) % ESPNOW_HIST];
      achou = m->minha && m->send_state == ESPNOW_ENVIO_PENDENTE;
    }
    destravar();
    if (!achou) { return; }
    static evento_t falha;   // mesmo caminho of ESTADO of C6: marca e grava no cartao
    memset(&falha, 0, sizeof(falha));
    falha.type = C6_MSG_ESTADO;
    falha.n = 7;
    memset(falha.data, 0xFF, 6);
    tratar(&falha);
  }
}

static void tratar(const evento_t* ev)
{
  switch (ev->type) {
    case C6_MSG_PRONTO: {
      if (ev->n < 7) { return; }
      memcpy(s_mac, ev->data, 6);
      s_canal = ev->data[6];
      // O PRONTO v2 traz versao of contrato e capacidades; o farejo precisa
      // saber se este C6 sabe farejar/CSI/FTM/802.15.4.
      tab5_farejo_anotar_pronto(ev->data, ev->n);
      if (!s_pronto) {
        s_pronto = true;
        ESP_LOGI(TAG, "running: MAC %02X:%02X:%02X:%02X:%02X:%02X, channel %u",
                 s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5], (unsigned)s_canal);
        // O CANAL FIXO SOBREVIVE A VOLTA DO RADIO. Desligar e religar o WiFi
        // (ou a desistencia) para o radio, e o C6 iteration no canal default dele -
        // o app continuava dizendo "canal N fixo" com o radio em outro.
        if (s_canal_fixo) {
          static evento_t canal;
          memset(&canal, 0, sizeof(canal));
          canal.type = EV_CANAL;
          canal.n = 1;
          canal.data[0] = s_canal_fixo;
          tratar(&canal);
        }
      }
      system_notify(AVISO_ESPNOW);
      break;
    }

    case C6_MSG_RECEBIDO: {
      if (ev->n < 8) { return; }
      static espnow_msg_t m;   // estatico: so esta tarefa; a stack stays para o cartao
      memset(&m, 0, sizeof(m));
      memcpy(m.mac, ev->data, 6);
      m.difusao   = (ev->data[6] & 1) != 0;
      m.espressif = (ev->data[6] & 2) != 0;   // bit 1: o C6 tirou o cabecalho Espressif (ver c6/ DOIS DIALETOS)
      m.minha   = false;
      m.send_state   = ESPNOW_ENVIO_NADA;
      m.when  = now_epoch();
      size_t t = ev->n - 7;
      if (t > C6_TEXTO_MAX) { t = C6_TEXTO_MAX; }
      memcpy(m.text, ev->data + 7, t);
      m.text[t] = 0;
      // Texto of outro aparelho pode vir com lixo no end (one '\0' of C, uma
      // quebra of line of keyboard dele): o que nao e imprimivel no final exit.
      size_t n = strlen(m.text);
      while (n && (unsigned char)m.text[n - 1] < 0x20) { m.text[--n] = 0; }
      // No MEIO, controle vira espaco: one formato que nao e text (uma struct
      // com numero depois of text, por exemplo) nao vira caractere quebrado no
      // bubble. O corte no first '\0' ja fez o resto: of "struct {char
      // text[50]; int valor;}" aparece o text.
      for (size_t k = 0; k < n; ++k) {
        if ((unsigned char)m.text[k] < 0x20) { m.text[k] = ' '; }
      }

      lock();
      guardar(&m);
      ++s_nao_lidas;
      count_visto(m.mac);
      s_ultimo_rx_us = esp_timer_get_time();
      publicar_notif();
      destravar();
      gravar_no_cartao(&m);
      ESP_LOGI(TAG, "de %02X%02X: %s", m.mac[4], m.mac[5], m.text);
      system_notify(AVISO_ESPNOW);
      break;
    }

    case C6_MSG_ESTADO: {
      if (ev->n < 7) { return; }
      // Os envios saem em queue, one of cada vez: o estado que chega e o of mais
      // antigo ainda pending.
      lock();
      static espnow_msg_t copia;
      memset(&copia, 0, sizeof(copia));
      bool achou = false;
      for (int j = 0; j < s_n && !achou; ++j) {
        espnow_msg_t* m = &s_hist[(s_ini + j) % ESPNOW_HIST];
        if (m->minha && m->send_state == ESPNOW_ENVIO_PENDENTE) {
          m->send_state = ev->data[6] ? ESPNOW_ENVIO_OK : ESPNOW_ENVIO_FALHOU;
          copia = *m;
          achou = true;
        }
      }
      destravar();
      if (achou) { gravar_no_cartao(&copia); }
      system_notify(AVISO_ESPNOW);
      break;
    }

    case EV_ENVIAR: {
      static espnow_msg_t m;
      memset(&m, 0, sizeof(m));
      memset(m.mac, 0xFF, 6);   // difusao
      m.minha = true;
      m.difusao = true;
      m.when = now_epoch();
      snprintf(m.text, sizeof(m.text), "%.*s", (int)ev->n, (const char*)ev->data);
      m.send_state = s_pronto ? ESPNOW_ENVIO_PENDENTE : ESPNOW_ENVIO_FALHOU;
      s_ultimo_tx_us = esp_timer_get_time();

      lock();
      guardar(&m);
      destravar();
      system_notify(AVISO_ESPNOW);

      if (s_pronto) {
        static uint8_t buf[6 + C6_TEXTO_MAX];
        memset(buf, 0xFF, 6);
        const size_t t = strlen(m.text);
        memcpy(buf + 6, m.text, t);
        if (eh_host_peer_data_send(C6_MSG_HOST_ENVIAR, buf, 6 + t) != ESP_OK) {
          // O C6 nem recebeu o request: nao vai haver ESTADO para este send_state.
          static evento_t falha;
          memset(&falha, 0, sizeof(falha));
          falha.type = C6_MSG_ESTADO;
          falha.n = 7;
          memset(falha.data, 0xFF, 6);
          falha.data[6] = 0;
          tratar(&falha);
          return;
        }
      } else {
        gravar_no_cartao(&m);
      }
      break;
    }

    case EV_CANAL: {
      const uint8_t c = ev->data[0];
      s_canal_fixo = c;
      settings_set_espnow_channel(c);   // sobrevive on reinicio (so grava se changed)
      if (c == 0) {
        // De iteration on roteador: o canal passa a ser o dele (o loop confere).
        tab5_wifi_radio_only_mode(false);
        notification_limpar(s_notif_canal);
        ESP_LOGI(TAG, "channel: router channel");
      } else {
        // O radio so muda of canal SOLTO of roteador: pede e espera release.
        tab5_wifi_radio_only_mode(true);
        for (int k = 0; k < 30 && tab5_wifi_state() == TAB5_WIFI_CONNECTED; ++k) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        // CONFERE NO C6, e repete. Pedido no middle of uma
        // reconexao (touch rapido roteador -> 1), o C6 recusa a troca calado e
        // o radio stays no canal anterior - visto no log: "canal 1 -> 11". A
        // unica verdade e o canal que o C6 diz que esta.
        uint8_t real = 0; wifi_second_chan_t sec;
        for (int tent = 0; tent < 5; ++tent) {
          vTaskDelay(pdMS_TO_TICKS(200));
          eh_host_peer_data_send(C6_MSG_HOST_CANAL, &c, 1);
          vTaskDelay(pdMS_TO_TICKS(100));
          if (esp_wifi_get_channel(&real, &sec) == ESP_OK && real == c) { break; }
        }
        s_canal = real ? real : c;
        if (real == c) { ESP_LOGI(TAG, "fixed channel %u (WiFi released from router)", (unsigned)c); }
        else           { ESP_LOGW(TAG, "fixed channel %u requested, but C6 stayed on %u", (unsigned)c, (unsigned)real); }
        // VISIVEL NO SISTEMA: sem isto, o WiFi released parecia WiFi quebrado.
        char summary[NOTIF_RESUMO];
        snprintf(summary, sizeof(summary), "channel %u - no WiFi", (unsigned)s_canal);
        notification_publicar(s_notif_canal, 1, NOTIF_ESTADO, summary);
      }
      system_notify(AVISO_ESPNOW);
      break;
    }

    default:
      break;
  }
}

// O RECEPTOR LIGADO SEM PARAR E CARO, e so vale when alguem pode estar
// falando com o Tab5. Cochilando entre beacons (modem sleep), o C6 perde o
// frame of ESP-NOW que chega no cochilo. Pede o receptor enabled when:
//   - o canal esta FIXO: o user escolheu o modo ESP-NOW (e sem roteador o
//     modem sleep nem funciona);
//   - o app esta open com a screen acesa: a conversa esta in frente dele;
//   - houve message, recebida ou enviada, nos ultimos 5 min: conversa alive,
//     e a resposta tem que chegar mesmo com o app closed ou a screen apagada.
// Fora disso o radio economiza. Uma message que chegar no cochilo pode se
// perder - e o formato Espressif repete o frame, o que ajuda.
static bool quer_receptor_enabled(void)
{
  if (!s_pronto) { return false; }
  if (s_canal_fixo) { return true; }
  if (s_window && !tab5_power_is_sleeping()) { return true; }
  const int64_t now = esp_timer_get_time();
  const int64_t ult = (s_ultimo_rx_us > s_ultimo_tx_us) ? s_ultimo_rx_us : s_ultimo_tx_us;
  return ult && (now - ult) < CONVERSA_VIVA_US;
}

static void vTaskEspnow(void*)
{
  // Espera o firmware nosso responder: sem ele nao ha ESP-NOW para enable.
  // Enquanto isso, one send_state of interface falha in hora (nao stays pendurado).
  bool registrado = false;
  bool carregado = false;
  int64_t request_us = 0, canal_us = 0;
  static evento_t ev;

  for (;;) {
    const bool enlace = tab5_wifi_c6_is_ours() && tab5_wifi_c6_link();
    // ESP-NOW precisa of RADIO INICIADO, nao so of enlace: disable o WiFi no
    // Ajustes para o radio com o C6 alimentado e o enlace of pe.
    const bool no_ar = enlace && tab5_wifi_radio_active();

    if (enlace && !registrado) {
      eh_host_peer_data_register(C6_MSG_PRONTO,   on_peer_data, nullptr);
      eh_host_peer_data_register(C6_MSG_RECEBIDO, on_peer_data, nullptr);
      eh_host_peer_data_register(C6_MSG_ESTADO,   on_peer_data, nullptr);
      registrado = true;
    }
    if (enlace && !carregado) { carregar_of_cartao(); carregado = true; system_notify(AVISO_ESPNOW); }

    // Radio parado (WiFi disabled em Ajustes, desistencia fora of alcance of roteador): o
    // ESP-NOW caiu com ele. Quando o radio voltar, o request abaixo traz one
    // PRONTO novo - e com ele o canal fixo, se houver.
    if (!no_ar && s_pronto) {
      s_pronto = false;
      falhar_pendentes();
      ESP_LOGI(TAG, "radio stopped - ESP-NOW unavailable");
      system_notify(AVISO_ESPNOW);
    }

    // PEDE PARA LIGAR, e repete ate o PRONTO chegar. O C6 so liga o ESP-NOW
    // when o host manda (ver "LIGAR O ESP-NOW E DECISAO DO HOST" em
    // c6/main/app_main.c) - e o host so manda depois of WiFi of pe.
    const int64_t now = esp_timer_get_time();
    if (no_ar && !s_pronto && (now - request_us) >= (int64_t)REPEDIR_MS * 1000) {
      request_us = now;
      eh_host_peer_data_send(C6_MSG_HOST_ESPNOW, nullptr, 0);
    }

    // O canal segue o roteador (reconexao pode switch): confere of tempos em
    // tempos - acordado. Em sleep ninguem ve o canal, e cada conferencia e
    // uma ida on C6 pelo SDIO; on wake, a next iteration confere.
    if (s_pronto && !tab5_power_is_sleeping() && (now - canal_us) >= (int64_t)CANAL_MS * 1000) {
      canal_us = now;
      uint8_t c = 0; wifi_second_chan_t sec;
      if (esp_wifi_get_channel(&c, &sec) == ESP_OK && c && c != s_canal) {
        ESP_LOGI(TAG, "canal %u -> %u", (unsigned)s_canal, (unsigned)c);
        s_canal = c;
        system_notify(AVISO_ESPNOW);
      }
    }

    // O request so vai on WiFi when MUDA (a API ja ignora repeticao).
    tab5_wifi_keep_awake(TAB5_AWAKE_ESPNOW, quer_receptor_enabled());

    // SEM ENLACE COM O C6 (Wi-Fi nunca subiu, ou o C6 nao tem o firmware
    // nosso) nada aqui muda ate a stack raise - e o Wi-Fi avisa nessa hora
    // (tab5_wifi_on_stack_ready -> espnow_cutucar). Entao dorme 30 s em vez of
    // wake todo segundo para perguntar.
    const uint32_t espera_ms = enlace ? 1000 : 30000;
    if (xQueueReceive(s_fila, &ev, pdMS_TO_TICKS(espera_ms)) == pdTRUE) { tratar(&ev); }
  }
}

// ---------------------------------------------------------------------------
// O RELOGIO SALTOU (ver tab5_time.hpp): as mensagens desta sessao foram
// carimbadas pela hora velha e andam junto. So a RAM - a line que ja foi para
// o cartao stays com a hora of when foi gravada.
static void on_saltar_clock(int32_t delta)
{
  if (!s_trava) { return; }
  int n = 0;
  lock();
  for (int j = 0; j < s_n; ++j) {
    const int k = (s_ini + j) % ESPNOW_HIST;
    if (!s_of_sessao[k] || s_hist[k].when == 0) { continue; }
    s_hist[k].when = (uint32_t)((int64_t)s_hist[k].when + delta);
    ++n;
  }
  if (n) { ++s_total; }   // a screen rele e redesenha com os dias certos
  destravar();
  if (n) { system_notify(AVISO_ESPNOW); }
}

// API
// ---------------------------------------------------------------------------
// O Wi-Fi subiu a stack: acorda o laco para ver o enlace in hora.
static void espnow_cutucar(void)
{
  if (!s_fila) { return; }
  evento_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = EV_NADA;
  xQueueSend(s_fila, &ev, 0);
}

void tab5_espnow_start(void)
{
  if (s_fila) { return; }
  s_trava = xSemaphoreCreateMutex();
  s_fila  = xQueueCreateWithCaps(12, sizeof(evento_t), MALLOC_CAP_SPIRAM);
  s_notif = notification_registrar("ESP-NOW");
  s_notif_canal = notification_registrar("ESP-NOW canal");
  notification_associar_app(s_notif_canal, "ESP-NOW");
  // E one ESTADO (o WiFi released of roteador), nao uma message: mudar of canal
  // nao pode soar como notification nova.
  notification_silenciar(s_notif_canal, true);
  if (!s_trava || !s_fila) { ESP_LOGE(TAG, "no memory - service unavailable"); return; }
  // O canal escolhido of ultima vez. Nao aplica aqui: quem aplica e o PRONTO
  // of C6 (ele reaplica o canal fixo toda vez que o radio iteration), entao basta
  // a vontade estar em s_canal_fixo antes disso.
  s_canal_fixo = settings_espnow_channel();
  tab5_time_on_jump(on_saltar_clock);
  tab5_wifi_on_stack_ready(espnow_cutucar);   // sem enlace a tarefa dorme 30 s; o Wi-Fi acorda ela
  // 6 KB, e nao 4: esta tarefa MONTA O CARTAO (gravar/carregar o historico),
  // e o f_mount of FATFS desce fundo in stack. Com 4 KB o monitor viu 140
  // bytes livres e o send_state seguinte estourou a stack no middle of montagem
  // (Guru Meditation em sdmmc_read_sectors) - pico of ~4 KB. Com 8 KB, dois
  // dias of pilhas-*.csv deram ~2,6 KB. 6 KB cobre o pico of montagem com 2 KB
  // of folga; os buffers grandes daqui sao static (PSRAM), nao stack.
  {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(vTaskEspnow, "espnow", 6144, nullptr, 3, &h, tskNO_AFFINITY);
    tab5_owner_register(h, OWNER_SERVICE, "ESP-NOW");   // ver hal/tab5_owners.hpp
  }
  ESP_LOGI(TAG, "service running (waiting for our C6 firmware)");
}

bool    tab5_espnow_pronto(void)          { return s_pronto; }
uint8_t tab5_espnow_canal(void)           { return s_canal; }
void    tab5_espnow_meu_mac(uint8_t out[6]) { memcpy(out, s_mac, 6); }
bool    tab5_espnow_c6_compativel(void)   { return tab5_wifi_c6_is_ours(); }

int tab5_espnow_n(void)
{
  if (!s_trava) { return 0; }
  lock(); const int n = s_n; destravar();
  return n;
}

bool tab5_espnow_msg(int i, espnow_msg_t* out)
{
  if (!s_trava || !out) { return false; }
  lock();
  const bool ok = (i >= 0 && i < s_n);
  if (ok) { *out = s_hist[(s_ini + i) % ESPNOW_HIST]; }
  destravar();
  return ok;
}

uint32_t tab5_espnow_total(void)   { return s_total; }
int      tab5_espnow_vistos(void)  { return s_n_vistos; }

uint32_t tab5_espnow_ms_since_rx(void)
{
  if (!s_ultimo_rx_us) { return UINT32_MAX; }
  return (uint32_t)((esp_timer_get_time() - s_ultimo_rx_us) / 1000);
}

uint8_t tab5_espnow_canal_fixo(void) { return s_canal_fixo; }

// LOCAL, e nao estatico: o comentario dizia "so a interface
// chama", mas o console (commands espnow canal/send) chama of tarefa DELE -
// duas tarefas no mesmo buffer estatico podiam mandar uma message com o text
// of outra. 266 bytes cabem folgados nas stacks das duas (8 e 12 KB).
bool tab5_espnow_set_canal(uint8_t canal)
{
  if (!s_fila || canal > 13) { return false; }
  evento_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = EV_CANAL;
  ev.n = 1;
  ev.data[0] = canal;
  return xQueueSend(s_fila, &ev, 0) == pdTRUE;
}

bool tab5_espnow_send(const char* text)
{
  if (!s_fila || !text || !text[0]) { return false; }
  evento_t ev;   // local: ver tab5_espnow_definir_canal
  ev.type = EV_ENVIAR;
  size_t n = strlen(text);
  if (n > C6_TEXTO_MAX) { n = C6_TEXTO_MAX; }
  ev.n = (uint16_t)n;
  memcpy(ev.data, text, n);
  return xQueueSend(s_fila, &ev, 0) == pdTRUE;
}

int tab5_espnow_nao_lidas(void) { return s_nao_lidas; }

void tab5_espnow_window(bool open)
{
  s_window = open;
  if (s_fila) {   // reavalia ja, sem wait a iteration of 1 s of loop
    evento_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_NADA;
    xQueueSend(s_fila, &ev, 0);
  }
}

void tab5_espnow_marcar_lidas(void)
{
  if (!s_trava) { return; }
  lock();
  if (s_nao_lidas) { s_nao_lidas = 0; publicar_notif(); }
  destravar();
}
