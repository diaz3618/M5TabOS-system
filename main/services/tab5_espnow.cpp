#include "tab5_espnow.hpp"
#include "tab5_sniffer.hpp"   // reviews PRONTO (version and capabilities of C6)
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
#define REPEDIR_MS       5000     // without READY, requests new
#define CHANNEL_CHECK_MS         10000    // the channel follows the router: check from time to time

// -------------------------------------------------------------------------------
// STATE - written by the service task, read by the interface (copy, lock)
// -------------------------------------------------------------------------------
static EXT_RAM_BSS_ATTR espnow_msg_t s_hist[ESPNOW_HIST];   // ring: s_ini is the oldest
static int      s_n = 0, s_ini = 0;
static uint32_t s_total = 0;
// Stamped by the clock of THIS session (as of card not): only these are
// fixed when the clock jumps - see ao_saltar_relogio().
static bool     s_of_sessao[ESPNOW_HIST];
static bool     s_lendo_cartao = false;
static int      s_nao_lidas = 0;
static SemaphoreHandle_t s_trava = nullptr;

static volatile bool    s_pronto = false;
static volatile uint8_t s_canal  = 0;
static uint8_t          s_mac[6] = { };
static int              s_notif  = -1;

// Listening devices (only for count "how many"), and the last frame.
#define VISTOS_MAX 16
static uint8_t  s_vistos[VISTOS_MAX][6];
static int      s_n_vistos = 0;
static int64_t  s_ultimo_rx_us = 0;

// ENERGY. The receiver of C6 so stays enabled all the time when the
// ESP-NOW is indeed in use - see quer_receiver_ligado(). Other than that, modem
// sleep, like any WiFi.
#define ACTIVE_CONVERSATION_US (5LL * 60 * 1000000)   // 5 min since the last message
static int64_t       s_ultimo_tx_us = 0;
static volatile bool s_window = false;          // the ESP-NOW app is open

static inline void lock(void)    { xSemaphoreTake(s_trava, portMAX_DELAY); }
static inline void destravar(void) { xSemaphoreGive(s_trava); }

// -------------------------------------------------------------------------------
// THE SERVICE QUEUE - just one, for what arrives from C6 and what the interface requests
// -------------------------------------------------------------------------------
// Notices of peer_data arrive in task of reception of esp_hosted, which does not
// can wait for nothing (and it delivers ALL WiFi traffic). The callback
// only copies to the queue; who thinks is the task of service. The send_state comes from
// interface through the SAME queue, because eh_host_peer_data_send() BLOCKS even C6
// respond (up to 5 s) - in interface this would be screen frozen.
typedef struct {
  uint32_t type;                         // C6_MSG_* received, or EV_SEND
  uint16_t n;
  uint8_t  data[7 + C6_TEXTO_MAX + 1];
} evento_t;

#define EV_SEND 0x1000
#define EV_CHANNEL  0x1001   // data[0] = fixed channel (0 = the of router)
#define EV_NADA   0x1002   // just wakes up the loop (reevaluates the receiver)

static volatile uint8_t s_canal_fixo = 0;
static int s_notif_canal = -1;   // gray tablet while WiFi is released from router

static QueueHandle_t s_fila = nullptr;

static void on_peer_data(uint32_t msg_id, const uint8_t* data, size_t n, void*)
{
  if (!s_fila) { return; }
  // STATIC: this callback runs in task of reception of esp_hosted, whose
  // stack is not ours - 270 bytes more on it and bet. So this task calls
  // here (one at a time), and xQueueSend copies it before returning.
  static evento_t ev;
  ev.type = msg_id;
  ev.n = (uint16_t)(n > sizeof(ev.data) ? sizeof(ev.data) : n);
  if (data && ev.n) { memcpy(ev.data, data, ev.n); }
  xQueueSend(s_fila, &ev, 0);   // never wait: losing a frame is better than blocking WiFi
}

// -------------------------------------------------------------------------------
// HISTORY
// -------------------------------------------------------------------------------
static void publicar_notif(void)
{
  if (s_notif < 0) { return; }
  char summary[NOTIF_RESUMO];
  if (s_nao_lidas > 0) {
    // Who sent the last one: the last two bytes of MAC are enough to distinguish
    // the appliances of a table.
    const espnow_msg_t* m = &s_hist[(s_ini + s_n - 1) % ESPNOW_HIST];
    snprintf(summary, sizeof(summary), "%d de %02X%02X", s_nao_lidas, m->mac[4], m->mac[5]);
  } else {
    summary[0] = 0;
  }
  notification_publicar(s_notif, s_nao_lidas, NOTIF_INFO, summary);
}

// With the lock taken. Returns the index (in the ring) of stored message.
static int store_message(const espnow_msg_t* m)
{
  int k;
  if (s_n < ESPNOW_HIST) { k = (s_ini + s_n) % ESPNOW_HIST; ++s_n; }
  else                   { k = s_ini; s_ini = (s_ini + 1) % ESPNOW_HIST; }
  s_hist[k] = *m;
  s_of_sessao[k] = !s_lendo_cartao;
  ++s_total;
  return k;
}

// One line per message: epoch, MAC, flags and text - without TAB or break of
// line inside it. Flags: m/r (mine/received), d/u (broadcast/direct) - EM
// UPPERCASE (D/U) when it comes in Espressif format, which keeps the text legible
// history recorded before the dialect existed -, and the state of send_state.
// By system recorder (tab5_sd_enqueue_append, urgent): service task not
// assembles the card more with each message.
static void write_to_sd_card(const espnow_msg_t* m)
{
  static char txt[sizeof(m->text)];   // static: only the service task calls
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

// No boot of service: the latest ESPNOW_HIST messages of card return to the
// RAM, and the conversation opens with what has already been said.
//
// ONLY THE END OF THE FILE, AND WITHOUT LOCKING DURING READING. O
// history only grows, and before it was read ENTIRELY with the lock stuck - the
// interface, which asks for messages with each drawing of app, was
// waiting for the card, and more and more as the file got fatter. Now:
// skips straight to the last ~ESPNOW_HIST x 300 bytes (a line does not pass
// of ~290: epoch, MAC, flags and up to 250 of text), discards the first line
// (probably cut in the middle) and locks just the moment of saving each one.
#define ESPNOW_CAUDA_BYTES ((long)ESPNOW_HIST * 300)
static void load_from_sd_card(void)
{
  if (!tab5_sd_mount()) { return; }
  s_lendo_cartao = true;
  FILE* f = tab5_sd_open(ESPNOW_PASTA, ESPNOW_ARQUIVO, "r");
  if (f) {
    static char line[400];   // static: a stack and assembly of cards
    int lidas = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
      const long size = ftell(f);
      if (size > ESPNOW_CAUDA_BYTES && fseek(f, size - ESPNOW_CAUDA_BYTES, SEEK_SET) == 0) {
        fgets(line, sizeof(line), f);   // the piece of line cut
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
      m.espressif = (dif == 'D' || dif == 'U');   // without this, after boot everything would become "pure text"
      // A send_state that became pending when the device turned off will not be
      // confirmed never again: shows as failed, not as "sending" eternal.
      m.send_state = (uint8_t)((send_state == ESPNOW_ENVIO_PENDENTE) ? ESPNOW_ENVIO_FALHOU : send_state);
      snprintf(m.text, sizeof(m.text), "%s", line + pos);
      const size_t n = strlen(m.text);
      if (n && m.text[n - 1] == '\n') { m.text[n - 1] = 0; }
      lock();
      store_message(&m);
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

// -------------------------------------------------------------------------------
// THE TASK
// -------------------------------------------------------------------------------
static void tratar(const evento_t* ev);

// ESP-NOW went down (radio stopped, C6 without link): one send_state pending will not be
// never confirmed - C6 only responds for what it itself has in queue. Turn
// fails, just like the loading_card does at boot.
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
    static evento_t falha;   // same path of STATE of C6: marks and writes to the card
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
      // PRONTO v2 brings version of contract and capabilities; the sniff needs
      // know if this C6 knows how to sniff/CSI/FTM/802.15.4.
      tab5_farejo_anotar_pronto(ev->data, ev->n);
      if (!s_pronto) {
        s_pronto = true;
        ESP_LOGI(TAG, "running: MAC %02X:%02X:%02X:%02X:%02X:%02X, channel %u",
                 s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5], (unsigned)s_canal);
        // THE FIXED CHANNEL SURVIVES THE RETURN OF THE RADIO. Turn WiFi off and back on
        // (or giving up) to the radio, and the C6 iteration on its default channel -
        // the app kept saying "fixed channel N" with the radio on another.
        if (s_canal_fixo) {
          static evento_t canal;
          memset(&canal, 0, sizeof(canal));
          canal.type = EV_CHANNEL;
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
      static espnow_msg_t m;   // static: only this task; the stack stays for the card
      memset(&m, 0, sizeof(m));
      memcpy(m.mac, ev->data, 6);
      m.difusao   = (ev->data[6] & 1) != 0;
      m.espressif = (ev->data[6] & 2) != 0;   // bit 1: C6 removed the Espressif header (see c6/ TWO DIALECTS)
      m.minha   = false;
      m.send_state   = ESPNOW_ENVIO_NADA;
      m.when  = now_epoch();
      size_t t = ev->n - 7;
      if (t > C6_TEXTO_MAX) { t = C6_TEXTO_MAX; }
      memcpy(m.text, ev->data + 7, t);
      m.text[t] = 0;
      // Text from another device may come with garbage at the end (one '\0' of C, a
      // break of line of keyboard): what is not printable at the end exit.
      size_t n = strlen(m.text);
      while (n && (unsigned char)m.text[n - 1] < 0x20) { m.text[--n] = 0; }
      // In the MIDDLE, control becomes space: a format that is not text (a struct
      // with a number after text, for example) does not become a broken character in
      // bubble. The cut in the first '\0' already did the rest: of "struct {char
      // text[50]; int value;}" the text appears.
      for (size_t k = 0; k < n; ++k) {
        if ((unsigned char)m.text[k] < 0x20) { m.text[k] = ' '; }
      }

      lock();
      store_message(&m);
      ++s_nao_lidas;
      count_visto(m.mac);
      s_ultimo_rx_us = esp_timer_get_time();
      publicar_notif();
      destravar();
      write_to_sd_card(&m);
      ESP_LOGI(TAG, "de %02X%02X: %s", m.mac[4], m.mac[5], m.text);
      system_notify(AVISO_ESPNOW);
      break;
    }

    case C6_MSG_ESTADO: {
      if (ev->n < 7) { return; }
      // Sends are queued, one at a time: the state that arrives and the most
      // old still pending.
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
      if (achou) { write_to_sd_card(&copia); }
      system_notify(AVISO_ESPNOW);
      break;
    }

    case EV_SEND: {
      static espnow_msg_t m;
      memset(&m, 0, sizeof(m));
      memset(m.mac, 0xFF, 6);   // diffusion
      m.minha = true;
      m.difusao = true;
      m.when = now_epoch();
      snprintf(m.text, sizeof(m.text), "%.*s", (int)ev->n, (const char*)ev->data);
      m.send_state = s_pronto ? ESPNOW_ENVIO_PENDENTE : ESPNOW_ENVIO_FALHOU;
      s_ultimo_tx_us = esp_timer_get_time();

      lock();
      store_message(&m);
      destravar();
      system_notify(AVISO_ESPNOW);

      if (s_pronto) {
        static uint8_t buf[6 + C6_TEXTO_MAX];
        memset(buf, 0xFF, 6);
        const size_t t = strlen(m.text);
        memcpy(buf + 6, m.text, t);
        if (eh_host_peer_data_send(C6_MSG_HOST_ENVIAR, buf, 6 + t) != ESP_OK) {
          // C6 didn't even receive the request: there will be no STATE for this send_state.
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
        write_to_sd_card(&m);
      }
      break;
    }

    case EV_CHANNEL: {
      const uint8_t c = ev->data[0];
      s_canal_fixo = c;
      settings_set_espnow_channel(c);   // survives on restart (only writes if changed)
      if (c == 0) {
        // From iteration on router: the channel becomes his (the loop checks).
        tab5_wifi_radio_only_mode(false);
        notification_limpar(s_notif_canal);
        ESP_LOGI(TAG, "channel: router channel");
      } else {
        // The radio only changes the LOOSE channel of the router: asks and waits for release.
        tab5_wifi_radio_only_mode(true);
        for (int k = 0; k < 30 && tab5_wifi_state() == TAB5_WIFI_CONNECTED; ++k) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        // CHECK C6, and repeat. Request in the middle of a
        // reconnection (rapid touch router -> 1), the C6 refuses the silent change and
        // the radio stays on the previous channel - seen in the log: "channel 1 -> 11". A
        // the only truth is the channel that C6 says it is on.
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
        // VISIBLE ON THE SYSTEM: without this, the WiFi released looked like broken WiFi.
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

// THE RECEIVER CONNECTED NON-STOP IS EXPENSIVE, and only applies when someone may be
// talking to Tab5. Dozing between beacons (modem sleep), the C6 loses its
// frame of ESP-NOW that arrives in the nap. Requests receiver enabled when:
// - the channel is FIXED: the user chose ESP-NOW mode (and without a router the
// modem sleep doesn't even work);
// - the app is open with the screen on: the conversation is in front of it;
// - there was a message, received or sent, in the last 5 min: conversation alive,
// and the answer must arrive even with the app closed or the screen turned off.
// Other than that, the radio saves money. A message that arrives in the nap can be
// lose - and the Espressif format repeats the frame, which helps.
static bool quer_receptor_enabled(void)
{
  if (!s_pronto) { return false; }
  if (s_canal_fixo) { return true; }
  if (s_window && !tab5_power_is_sleeping()) { return true; }
  const int64_t now = esp_timer_get_time();
  const int64_t ult = (s_ultimo_rx_us > s_ultimo_tx_us) ? s_ultimo_rx_us : s_ultimo_tx_us;
  return ult && (now - ult) < ACTIVE_CONVERSATION_US;
}

static void vTaskEspnow(void*)
{
  // Wait for our firmware to respond: without it there is no ESP-NOW to enable.
  // Meanwhile, one send_state of interface fails in time (does not stay hanging).
  bool registrado = false;
  bool carregado = false;
  int64_t request_us = 0, canal_us = 0;
  static evento_t ev;

  for (;;) {
    const bool enlace = tab5_wifi_c6_is_ours() && tab5_wifi_c6_link();
    // ESP-NOW needs RADIO INITIATED, not just link: disable WiFi on
    // Adjustments for the radio with the C6 powered and the pe link.
    const bool no_ar = enlace && tab5_wifi_radio_active();

    if (enlace && !registrado) {
      eh_host_peer_data_register(C6_MSG_PRONTO,   on_peer_data, nullptr);
      eh_host_peer_data_register(C6_MSG_RECEBIDO, on_peer_data, nullptr);
      eh_host_peer_data_register(C6_MSG_ESTADO,   on_peer_data, nullptr);
      registrado = true;
    }
    if (enlace && !carregado) { load_from_sd_card(); carregado = true; system_notify(AVISO_ESPNOW); }

    // Radio stopped (WiFi disabled in Settings, drops out of range of router): o
    // ESP-NOW fell with him. When the radio returns, the request below brings one
    // READY new - and with it the fixed channel, if any.
    if (!no_ar && s_pronto) {
      s_pronto = false;
      falhar_pendentes();
      ESP_LOGI(TAG, "radio stopped - ESP-NOW unavailable");
      system_notify(AVISO_ESPNOW);
    }

    // ASK TO CALL, and repeat until READY arrives. The C6 only turns on ESP-NOW
    // when the host commands (see "TURN ON ESP-NOW AND HOST DECISION" in
    // c6/main/app_main.c) - and the host only sends it after WiFi of pe.
    const int64_t now = esp_timer_get_time();
    if (no_ar && !s_pronto && (now - request_us) >= (int64_t)REPEDIR_MS * 1000) {
      request_us = now;
      eh_host_peer_data_send(C6_MSG_HOST_ESPNOW, nullptr, 0);
    }

    // The channel follows the router (reconnection can switch): checks from time to time
    // times - awake. In sleep nobody sees the channel, and every conference and
    // a trip to C6 via SDIO; on wake, the next iteration checks.
    if (s_pronto && !tab5_power_is_sleeping() && (now - canal_us) >= (int64_t)CHANNEL_CHECK_MS * 1000) {
      canal_us = now;
      uint8_t c = 0; wifi_second_chan_t sec;
      if (esp_wifi_get_channel(&c, &sec) == ESP_OK && c && c != s_canal) {
        ESP_LOGI(TAG, "canal %u -> %u", (unsigned)s_canal, (unsigned)c);
        s_canal = c;
        system_notify(AVISO_ESPNOW);
      }
    }

    // The request only goes on WiFi when it CHANGES (the API already ignores repetition).
    tab5_wifi_keep_awake(TAB5_AWAKE_ESPNOW, quer_receptor_enabled());

    // NO LINK WITH THE C6 (Wi-Fi never went up, or the C6 doesn't have the firmware
    // wow) nothing here changes until the stack raise - and the Wi-Fi warns you at that time
    // (tab5_wifi_on_stack_ready -> espnow_poke). So sleep 30 seconds instead of
    // wake up every second to ask.
    const uint32_t espera_ms = enlace ? 1000 : 30000;
    if (xQueueReceive(s_fila, &ev, pdMS_TO_TICKS(espera_ms)) == pdTRUE) { tratar(&ev); }
  }
}

// -------------------------------------------------------------------------------
// THE CLOCK SKIPPED (see tab5_time.hpp): messages from this session were
// stamped by the old time and go together. Only RAM - the line that has already gone to
// the card remains with the time of when it was recorded.
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
  if (n) { ++s_total; }   // the screen rereads and redraws with the right days
  destravar();
  if (n) { system_notify(AVISO_ESPNOW); }
}

// API
// -------------------------------------------------------------------------------
// Wi-Fi has increased the stack: wake up the link to see the link instantly.
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
  // It is one STATUS (the WiFi released from router), not a message: change of channel
  // cannot sound like a new notification.
  notification_silenciar(s_notif_canal, true);
  if (!s_trava || !s_fila) { ESP_LOGE(TAG, "no memory - service unavailable"); return; }
  // The channel chosen last time. Does not apply here: PRONTO is the one who applies it
  // of C6 (it reapplies the fixed channel every time the radio iteration), so just
  // will be on s_canal_fixo before that.
  s_canal_fixo = settings_espnow_channel();
  tab5_time_on_jump(on_saltar_clock);
  tab5_wifi_on_stack_ready(espnow_cutucar);   // without link the task sleeps 30 s; the Wi-Fi wakes her up
  // 6 KB, not 4: this task MOUNT THE CARD (write/load history),
  // and the f_mount of FATFS drops deep in stack. With 4 KB the monitor saw 140
  // free bytes and the next send_state overflowed the stack in the middle of assembly
  // (Guru Meditation in sdmmc_read_sectors) - peak of ~4 KB. With 8 KB, two
  // days of stacks-*.csv gave ~2.6 KB. 6 KB covers the peak of assembly with 2 KB
  // of slack; the large buffers here are static (PSRAM), not stack.
  {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(vTaskEspnow, "espnow", 6144, nullptr, 3, &h, tskNO_AFFINITY);
    tab5_owner_register(h, OWNER_SERVICE, "ESP-NOW");   // see hal/tab5_owners.hpp
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

// LOCAL, not static: the comment said "only the interface
// calls", but the console (commands espnow canal/send) calls of HIS task -
// two tasks in the same static buffer could send a message with the text
// of another. 266 bytes fit comfortably in both stacks (8 and 12 KB).
bool tab5_espnow_set_canal(uint8_t canal)
{
  if (!s_fila || canal > 13) { return false; }
  evento_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = EV_CHANNEL;
  ev.n = 1;
  ev.data[0] = canal;
  return xQueueSend(s_fila, &ev, 0) == pdTRUE;
}

bool tab5_espnow_send(const char* text)
{
  if (!s_fila || !text || !text[0]) { return false; }
  evento_t ev;   // location: see tab5_espnow_define_canal
  ev.type = EV_SEND;
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
  if (s_fila) {   // reevaluates ja, without waiting an iteration of 1 s of loop
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
