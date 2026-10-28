#include "tab5_espnow.hpp"
#include "tab5_sniffer.hpp"   // repassa o PRONTO (versao e capacidades do C6)
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
#define REPEDIR_MS       5000     // sem PRONTO, pede de novo
#define CANAL_MS         10000    // o canal segue o roteador: confere de tempos em tempos

// ---------------------------------------------------------------------------
// ESTADO - escrito pela tarefa do servico, lido pela interface (copia, trava)
// ---------------------------------------------------------------------------
static EXT_RAM_BSS_ATTR espnow_msg_t s_hist[ESPNOW_HIST];   // anel: s_ini e a mais antiga
static int      s_n = 0, s_ini = 0;
static uint32_t s_total = 0;
// Carimbada pelo relogio DESTA sessao (as do cartao nao): so essas sao
// corrigidas quando o relogio salta - ver ao_saltar_relogio().
static bool     s_da_sessao[ESPNOW_HIST];
static bool     s_lendo_cartao = false;
static int      s_nao_lidas = 0;
static SemaphoreHandle_t s_trava = nullptr;

static volatile bool    s_pronto = false;
static volatile uint8_t s_canal  = 0;
static uint8_t          s_mac[6] = { };
static int              s_notif  = -1;

// Aparelhos ouvidos (so para contar "quantos"), e o ultimo frame.
#define VISTOS_MAX 16
static uint8_t  s_vistos[VISTOS_MAX][6];
static int      s_n_vistos = 0;
static int64_t  s_ultimo_rx_us = 0;

// ENERGIA. O receptor do C6 so fica ligado o tempo todo quando o
// ESP-NOW esta de fato em uso - ver quer_receptor_ligado(). Fora disso, modem
// sleep, como qualquer WiFi.
#define CONVERSA_VIVA_US (5LL * 60 * 1000000)   // 5 min desde a ultima mensagem
static int64_t       s_ultimo_tx_us = 0;
static volatile bool s_janela = false;          // o app ESP-NOW esta aberto

static inline void travar(void)    { xSemaphoreTake(s_trava, portMAX_DELAY); }
static inline void destravar(void) { xSemaphoreGive(s_trava); }

// ---------------------------------------------------------------------------
// A QUEUE DO SERVICO - uma so, para o que chega do C6 e o que a interface pede
// ---------------------------------------------------------------------------
// Os avisos do peer_data chegam na tarefa de recepcao do esp_hosted, que nao
// pode esperar por nada (e ela que entrega TODO o trafego do WiFi). O callback
// so copia para a queue; quem pensa e a tarefa do servico. O envio vem da
// interface pela MESMA queue, porque eh_host_peer_data_send() BLOQUEIA ate o C6
// responder (ate 5 s) - na interface isso seria tela congelada.
typedef struct {
  uint32_t type;                         // C6_MSG_* recebido, ou EV_ENVIAR
  uint16_t n;
  uint8_t  dados[7 + C6_TEXTO_MAX + 1];
} evento_t;

#define EV_ENVIAR 0x1000
#define EV_CANAL  0x1001   // dados[0] = canal fixo (0 = o do roteador)
#define EV_NADA   0x1002   // so acorda o loop (reavaliar o receptor)

static volatile uint8_t s_canal_fixo = 0;
static int s_notif_canal = -1;   // pastilha cinza enquanto o WiFi esta solto do roteador

static QueueHandle_t s_fila = nullptr;

static void ao_peer_data(uint32_t msg_id, const uint8_t* dados, size_t n, void*)
{
  if (!s_fila) { return; }
  // ESTATICO: este callback roda na tarefa de recepcao do esp_hosted, cuja
  // stack nao e nossa - 270 bytes a mais nela e aposta. So essa tarefa chama
  // aqui (uma por vez), e o xQueueSend copia antes de voltar.
  static evento_t ev;
  ev.type = msg_id;
  ev.n = (uint16_t)(n > sizeof(ev.dados) ? sizeof(ev.dados) : n);
  if (dados && ev.n) { memcpy(ev.dados, dados, ev.n); }
  xQueueSend(s_fila, &ev, 0);   // nunca espera: perder um frame e melhor que travar o WiFi
}

// ---------------------------------------------------------------------------
// HISTORICO
// ---------------------------------------------------------------------------
static void publicar_notif(void)
{
  if (s_notif < 0) { return; }
  char resumo[NOTIF_RESUMO];
  if (s_nao_lidas > 0) {
    // Quem mandou a ultima: os dois ultimos bytes do MAC bastam para distinguir
    // os aparelhos de uma mesa.
    const espnow_msg_t* m = &s_hist[(s_ini + s_n - 1) % ESPNOW_HIST];
    snprintf(resumo, sizeof(resumo), "%d de %02X%02X", s_nao_lidas, m->mac[4], m->mac[5]);
  } else {
    resumo[0] = 0;
  }
  notif_publicar(s_notif, s_nao_lidas, NOTIF_INFO, resumo);
}

// Com a trava tomada. Devolve o indice (no anel) da mensagem guardada.
static int guardar(const espnow_msg_t* m)
{
  int k;
  if (s_n < ESPNOW_HIST) { k = (s_ini + s_n) % ESPNOW_HIST; ++s_n; }
  else                   { k = s_ini; s_ini = (s_ini + 1) % ESPNOW_HIST; }
  s_hist[k] = *m;
  s_da_sessao[k] = !s_lendo_cartao;
  ++s_total;
  return k;
}

// Uma linha por mensagem: epoch, MAC, flags e o texto - sem TAB nem quebra de
// linha dentro dele. Flags: m/r (minha/recebida), d/u (difusao/direta) - EM
// MAIUSCULA (D/U) quando veio no formato Espressif, o que mantem legivel o
// historico gravado antes de existir o dialeto -, e o estado do envio.
// Pelo gravador do sistema (tab5_sd_enqueue_append, urgente): a tarefa do servico nao
// monta mais o cartao a cada mensagem.
static void gravar_no_cartao(const espnow_msg_t* m)
{
  static char txt[sizeof(m->texto)];   // estatico: so a tarefa do servico chama
  size_t k = 0;
  for (const char* c = m->texto; *c && k < sizeof(txt) - 1; ++c) {
    txt[k++] = (*c == '\t' || *c == '\n' || *c == '\r') ? ' ' : *c;
  }
  txt[k] = 0;
  static char linha[sizeof(txt) + 64];
  snprintf(linha, sizeof(linha), "%lu\t%02x%02x%02x%02x%02x%02x\t%c%c%u\t%s\n", (unsigned long)m->quando,
           m->mac[0], m->mac[1], m->mac[2], m->mac[3], m->mac[4], m->mac[5],
           m->minha ? 'm' : 'r',
           m->espressif ? (m->difusao ? 'D' : 'U') : (m->difusao ? 'd' : 'u'),
           (unsigned)m->envio, txt);
  tab5_sd_enqueue_append(ESPNOW_PASTA, ESPNOW_ARQUIVO, linha, nullptr, true);
}

// No boot do servico: as ultimas ESPNOW_HIST mensagens do cartao voltam para a
// RAM, e a conversa abre com o que ja foi dito.
//
// SO O FIM DO ARQUIVO, E SEM A TRAVA DURANTE A LEITURA. O
// historico so cresce, e antes era lido INTEIRO com a trava presa - a
// interface, que pergunta pelas mensagens a cada desenho do app, ficava
// esperando o cartao, e cada vez mais conforme o arquivo engordasse. Agora:
// pula direto para os ultimos ~ESPNOW_HIST x 300 bytes (uma linha nao passa
// de ~290: epoch, MAC, flags e ate 250 de texto), descarta a primeira linha
// (provavelmente cortada no meio) e trava so o instante de guardar cada uma.
#define ESPNOW_CAUDA_BYTES ((long)ESPNOW_HIST * 300)
static void carregar_do_cartao(void)
{
  if (!tab5_sd_mount()) { return; }
  s_lendo_cartao = true;
  FILE* f = tab5_sd_open(ESPNOW_PASTA, ESPNOW_ARQUIVO, "r");
  if (f) {
    static char linha[400];   // estatico: a stack e da montagem do cartao
    int lidas = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
      const long tam = ftell(f);
      if (tam > ESPNOW_CAUDA_BYTES && fseek(f, tam - ESPNOW_CAUDA_BYTES, SEEK_SET) == 0) {
        fgets(linha, sizeof(linha), f);   // o pedaco da linha cortada
      } else {
        fseek(f, 0, SEEK_SET);
      }
    }
    while (fgets(linha, sizeof(linha), f)) {
      static espnow_msg_t m;
      memset(&m, 0, sizeof(m));
      unsigned long quando = 0;
      unsigned int mac[6] = { };
      char quem = 0, dif = 0;
      unsigned int envio = 0;
      int pos = 0;
      if (sscanf(linha, "%lu\t%2x%2x%2x%2x%2x%2x\t%c%c%u\t%n", &quando,
                 &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5],
                 &quem, &dif, &envio, &pos) < 10 || pos <= 0) { continue; }
      m.quando = (uint32_t)quando;
      for (int i = 0; i < 6; ++i) { m.mac[i] = (uint8_t)mac[i]; }
      m.minha = (quem == 'm');
      m.difusao = (dif == 'd' || dif == 'D');
      m.espressif = (dif == 'D' || dif == 'U');   // sem isto, depois do boot tudo virava "texto puro"
      // Um envio que ficou pendente quando o aparelho desligou nao vai ser
      // confirmado nunca mais: mostra como falhou, nao como "enviando" eterno.
      m.envio = (uint8_t)((envio == ESPNOW_ENVIO_PENDENTE) ? ESPNOW_ENVIO_FALHOU : envio);
      snprintf(m.texto, sizeof(m.texto), "%s", linha + pos);
      const size_t n = strlen(m.texto);
      if (n && m.texto[n - 1] == '\n') { m.texto[n - 1] = 0; }
      travar();
      guardar(&m);
      destravar();
      ++lidas;
    }
    fclose(f);
    if (lidas) { ESP_LOGI(TAG, "cartao: %d mensagem(ns) no historico", lidas); }
  }
  s_lendo_cartao = false;
  tab5_sd_unmount();
}

static void contar_visto(const uint8_t mac[6])
{
  for (int i = 0; i < s_n_vistos; ++i) { if (memcmp(s_vistos[i], mac, 6) == 0) { return; } }
  if (s_n_vistos < VISTOS_MAX) { memcpy(s_vistos[s_n_vistos++], mac, 6); }
}

static uint32_t agora_epoch(void) { return tab5_time_epoch(); }

// ---------------------------------------------------------------------------
// A TAREFA
// ---------------------------------------------------------------------------
static void tratar(const evento_t* ev);

// O ESP-NOW caiu (radio parado, C6 sem enlace): um envio pendente nao vai ser
// confirmado nunca - o C6 so responde pelo que ele mesmo tem na queue. Vira
// falha, igual ao que o carregar_do_cartao faz no boot.
static void falhar_pendentes(void)
{
  for (;;) {
    bool achou = false;
    travar();
    for (int j = 0; j < s_n && !achou; ++j) {
      const espnow_msg_t* m = &s_hist[(s_ini + j) % ESPNOW_HIST];
      achou = m->minha && m->envio == ESPNOW_ENVIO_PENDENTE;
    }
    destravar();
    if (!achou) { return; }
    static evento_t falha;   // mesmo caminho do ESTADO do C6: marca e grava no cartao
    memset(&falha, 0, sizeof(falha));
    falha.type = C6_MSG_ESTADO;
    falha.n = 7;
    memset(falha.dados, 0xFF, 6);
    tratar(&falha);
  }
}

static void tratar(const evento_t* ev)
{
  switch (ev->type) {
    case C6_MSG_PRONTO: {
      if (ev->n < 7) { return; }
      memcpy(s_mac, ev->dados, 6);
      s_canal = ev->dados[6];
      // O PRONTO v2 traz versao do contrato e capacidades; o farejo precisa
      // saber se este C6 sabe farejar/CSI/FTM/802.15.4.
      tab5_farejo_anotar_pronto(ev->dados, ev->n);
      if (!s_pronto) {
        s_pronto = true;
        ESP_LOGI(TAG, "no ar: MAC %02X:%02X:%02X:%02X:%02X:%02X, canal %u",
                 s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5], (unsigned)s_canal);
        // O CANAL FIXO SOBREVIVE A VOLTA DO RADIO. Desligar e religar o WiFi
        // (ou a desistencia) para o radio, e o C6 volta no canal padrao dele -
        // o app continuava dizendo "canal N fixo" com o radio em outro.
        if (s_canal_fixo) {
          static evento_t canal;
          memset(&canal, 0, sizeof(canal));
          canal.type = EV_CANAL;
          canal.n = 1;
          canal.dados[0] = s_canal_fixo;
          tratar(&canal);
        }
      }
      sistema_avisar(AVISO_ESPNOW);
      break;
    }

    case C6_MSG_RECEBIDO: {
      if (ev->n < 8) { return; }
      static espnow_msg_t m;   // estatico: so esta tarefa; a stack fica para o cartao
      memset(&m, 0, sizeof(m));
      memcpy(m.mac, ev->dados, 6);
      m.difusao   = (ev->dados[6] & 1) != 0;
      m.espressif = (ev->dados[6] & 2) != 0;   // bit 1: o C6 tirou o cabecalho Espressif (ver c6/ DOIS DIALETOS)
      m.minha   = false;
      m.envio   = ESPNOW_ENVIO_NADA;
      m.quando  = agora_epoch();
      size_t t = ev->n - 7;
      if (t > C6_TEXTO_MAX) { t = C6_TEXTO_MAX; }
      memcpy(m.texto, ev->dados + 7, t);
      m.texto[t] = 0;
      // Texto de outro aparelho pode vir com lixo no fim (um '\0' do C, uma
      // quebra de linha do teclado dele): o que nao e imprimivel no final sai.
      size_t n = strlen(m.texto);
      while (n && (unsigned char)m.texto[n - 1] < 0x20) { m.texto[--n] = 0; }
      // No MEIO, controle vira espaco: um formato que nao e texto (uma struct
      // com numero depois do texto, por exemplo) nao vira caractere quebrado no
      // balao. O corte no primeiro '\0' ja fez o resto: de "struct {char
      // texto[50]; int valor;}" aparece o texto.
      for (size_t k = 0; k < n; ++k) {
        if ((unsigned char)m.texto[k] < 0x20) { m.texto[k] = ' '; }
      }

      travar();
      guardar(&m);
      ++s_nao_lidas;
      contar_visto(m.mac);
      s_ultimo_rx_us = esp_timer_get_time();
      publicar_notif();
      destravar();
      gravar_no_cartao(&m);
      ESP_LOGI(TAG, "de %02X%02X: %s", m.mac[4], m.mac[5], m.texto);
      sistema_avisar(AVISO_ESPNOW);
      break;
    }

    case C6_MSG_ESTADO: {
      if (ev->n < 7) { return; }
      // Os envios saem em queue, um de cada vez: o estado que chega e o do mais
      // antigo ainda pendente.
      travar();
      static espnow_msg_t copia;
      memset(&copia, 0, sizeof(copia));
      bool achou = false;
      for (int j = 0; j < s_n && !achou; ++j) {
        espnow_msg_t* m = &s_hist[(s_ini + j) % ESPNOW_HIST];
        if (m->minha && m->envio == ESPNOW_ENVIO_PENDENTE) {
          m->envio = ev->dados[6] ? ESPNOW_ENVIO_OK : ESPNOW_ENVIO_FALHOU;
          copia = *m;
          achou = true;
        }
      }
      destravar();
      if (achou) { gravar_no_cartao(&copia); }
      sistema_avisar(AVISO_ESPNOW);
      break;
    }

    case EV_ENVIAR: {
      static espnow_msg_t m;
      memset(&m, 0, sizeof(m));
      memset(m.mac, 0xFF, 6);   // difusao
      m.minha = true;
      m.difusao = true;
      m.quando = agora_epoch();
      snprintf(m.texto, sizeof(m.texto), "%.*s", (int)ev->n, (const char*)ev->dados);
      m.envio = s_pronto ? ESPNOW_ENVIO_PENDENTE : ESPNOW_ENVIO_FALHOU;
      s_ultimo_tx_us = esp_timer_get_time();

      travar();
      guardar(&m);
      destravar();
      sistema_avisar(AVISO_ESPNOW);

      if (s_pronto) {
        static uint8_t buf[6 + C6_TEXTO_MAX];
        memset(buf, 0xFF, 6);
        const size_t t = strlen(m.texto);
        memcpy(buf + 6, m.texto, t);
        if (eh_host_peer_data_send(C6_MSG_HOST_ENVIAR, buf, 6 + t) != ESP_OK) {
          // O C6 nem recebeu o pedido: nao vai haver ESTADO para este envio.
          static evento_t falha;
          memset(&falha, 0, sizeof(falha));
          falha.type = C6_MSG_ESTADO;
          falha.n = 7;
          memset(falha.dados, 0xFF, 6);
          falha.dados[6] = 0;
          tratar(&falha);
          return;
        }
      } else {
        gravar_no_cartao(&m);
      }
      break;
    }

    case EV_CANAL: {
      const uint8_t c = ev->dados[0];
      s_canal_fixo = c;
      settings_set_espnow_channel(c);   // sobrevive ao reinicio (so grava se mudou)
      if (c == 0) {
        // De volta ao roteador: o canal passa a ser o dele (o loop confere).
        tab5_wifi_radio_only_mode(false);
        notif_limpar(s_notif_canal);
        ESP_LOGI(TAG, "canal: o do roteador");
      } else {
        // O radio so muda de canal SOLTO do roteador: pede e espera soltar.
        tab5_wifi_radio_only_mode(true);
        for (int k = 0; k < 30 && tab5_wifi_state() == TAB5_WIFI_CONNECTED; ++k) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        // CONFERE NO C6, e repete. Pedido no meio de uma
        // reconexao (toque rapido roteador -> 1), o C6 recusa a troca calado e
        // o radio fica no canal anterior - visto no log: "canal 1 -> 11". A
        // unica verdade e o canal que o C6 diz que esta.
        uint8_t real = 0; wifi_second_chan_t sec;
        for (int tent = 0; tent < 5; ++tent) {
          vTaskDelay(pdMS_TO_TICKS(200));
          eh_host_peer_data_send(C6_MSG_HOST_CANAL, &c, 1);
          vTaskDelay(pdMS_TO_TICKS(100));
          if (esp_wifi_get_channel(&real, &sec) == ESP_OK && real == c) { break; }
        }
        s_canal = real ? real : c;
        if (real == c) { ESP_LOGI(TAG, "canal fixo %u (WiFi solto do roteador)", (unsigned)c); }
        else           { ESP_LOGW(TAG, "canal fixo %u pedido, mas o C6 ficou no %u", (unsigned)c, (unsigned)real); }
        // VISIVEL NO SISTEMA: sem isto, o WiFi solto parecia WiFi quebrado.
        char resumo[NOTIF_RESUMO];
        snprintf(resumo, sizeof(resumo), "canal %u - sem WiFi", (unsigned)s_canal);
        notif_publicar(s_notif_canal, 1, NOTIF_ESTADO, resumo);
      }
      sistema_avisar(AVISO_ESPNOW);
      break;
    }

    default:
      break;
  }
}

// O RECEPTOR LIGADO SEM PARAR E CARO, e so vale quando alguem pode estar
// falando com o Tab5. Cochilando entre beacons (modem sleep), o C6 perde o
// frame de ESP-NOW que chega no cochilo. Pede o receptor ligado quando:
//   - o canal esta FIXO: o usuario escolheu o modo ESP-NOW (e sem roteador o
//     modem sleep nem funciona);
//   - o app esta aberto com a tela acesa: a conversa esta na frente dele;
//   - houve mensagem, recebida ou enviada, nos ultimos 5 min: conversa viva,
//     e a resposta tem que chegar mesmo com o app fechado ou a tela apagada.
// Fora disso o radio economiza. Uma mensagem que chegar no cochilo pode se
// perder - e o formato Espressif repete o frame, o que ajuda.
static bool quer_receptor_ligado(void)
{
  if (!s_pronto) { return false; }
  if (s_canal_fixo) { return true; }
  if (s_janela && !tab5_power_is_sleeping()) { return true; }
  const int64_t agora = esp_timer_get_time();
  const int64_t ult = (s_ultimo_rx_us > s_ultimo_tx_us) ? s_ultimo_rx_us : s_ultimo_tx_us;
  return ult && (agora - ult) < CONVERSA_VIVA_US;
}

static void vTaskEspnow(void*)
{
  // Espera o firmware nosso responder: sem ele nao ha ESP-NOW para ligar.
  // Enquanto isso, um envio da interface falha na hora (nao fica pendurado).
  bool registrado = false;
  bool carregado = false;
  int64_t pedido_us = 0, canal_us = 0;
  static evento_t ev;

  for (;;) {
    const bool enlace = tab5_wifi_c6_is_ours() && tab5_wifi_c6_link();
    // ESP-NOW precisa do RADIO INICIADO, nao so do enlace: desligar o WiFi no
    // Ajustes para o radio com o C6 alimentado e o enlace de pe.
    const bool no_ar = enlace && tab5_wifi_radio_active();

    if (enlace && !registrado) {
      eh_host_peer_data_register(C6_MSG_PRONTO,   ao_peer_data, nullptr);
      eh_host_peer_data_register(C6_MSG_RECEBIDO, ao_peer_data, nullptr);
      eh_host_peer_data_register(C6_MSG_ESTADO,   ao_peer_data, nullptr);
      registrado = true;
    }
    if (enlace && !carregado) { carregar_do_cartao(); carregado = true; sistema_avisar(AVISO_ESPNOW); }

    // Radio parado (WiFi desligado em Ajustes, desistencia fora do alcance do roteador): o
    // ESP-NOW caiu com ele. Quando o radio voltar, o pedido abaixo traz um
    // PRONTO novo - e com ele o canal fixo, se houver.
    if (!no_ar && s_pronto) {
      s_pronto = false;
      falhar_pendentes();
      ESP_LOGI(TAG, "radio parado - ESP-NOW fora do ar");
      sistema_avisar(AVISO_ESPNOW);
    }

    // PEDE PARA LIGAR, e repete ate o PRONTO chegar. O C6 so liga o ESP-NOW
    // quando o host manda (ver "LIGAR O ESP-NOW E DECISAO DO HOST" em
    // c6/main/app_main.c) - e o host so manda depois do WiFi de pe.
    const int64_t agora = esp_timer_get_time();
    if (no_ar && !s_pronto && (agora - pedido_us) >= (int64_t)REPEDIR_MS * 1000) {
      pedido_us = agora;
      eh_host_peer_data_send(C6_MSG_HOST_ESPNOW, nullptr, 0);
    }

    // O canal segue o roteador (reconexao pode trocar): confere de tempos em
    // tempos - acordado. Em repouso ninguem ve o canal, e cada conferencia e
    // uma ida ao C6 pelo SDIO; ao acordar, a proxima volta confere.
    if (s_pronto && !tab5_power_is_sleeping() && (agora - canal_us) >= (int64_t)CANAL_MS * 1000) {
      canal_us = agora;
      uint8_t c = 0; wifi_second_chan_t sec;
      if (esp_wifi_get_channel(&c, &sec) == ESP_OK && c && c != s_canal) {
        ESP_LOGI(TAG, "canal %u -> %u", (unsigned)s_canal, (unsigned)c);
        s_canal = c;
        sistema_avisar(AVISO_ESPNOW);
      }
    }

    // O pedido so vai ao WiFi quando MUDA (a API ja ignora repeticao).
    tab5_wifi_keep_awake(TAB5_AWAKE_ESPNOW, quer_receptor_ligado());

    // SEM ENLACE COM O C6 (Wi-Fi nunca subiu, ou o C6 nao tem o firmware
    // nosso) nada aqui muda ate a pilha subir - e o Wi-Fi avisa nessa hora
    // (tab5_wifi_on_stack_ready -> espnow_cutucar). Entao dorme 30 s em vez de
    // acordar todo segundo para perguntar.
    const uint32_t espera_ms = enlace ? 1000 : 30000;
    if (xQueueReceive(s_fila, &ev, pdMS_TO_TICKS(espera_ms)) == pdTRUE) { tratar(&ev); }
  }
}

// ---------------------------------------------------------------------------
// O RELOGIO SALTOU (ver tab5_time.hpp): as mensagens desta sessao foram
// carimbadas pela hora velha e andam junto. So a RAM - a linha que ja foi para
// o cartao fica com a hora de quando foi gravada.
static void ao_saltar_relogio(int32_t delta)
{
  if (!s_trava) { return; }
  int n = 0;
  travar();
  for (int j = 0; j < s_n; ++j) {
    const int k = (s_ini + j) % ESPNOW_HIST;
    if (!s_da_sessao[k] || s_hist[k].quando == 0) { continue; }
    s_hist[k].quando = (uint32_t)((int64_t)s_hist[k].quando + delta);
    ++n;
  }
  if (n) { ++s_total; }   // a tela rele e redesenha com os dias certos
  destravar();
  if (n) { sistema_avisar(AVISO_ESPNOW); }
}

// API
// ---------------------------------------------------------------------------
// O Wi-Fi subiu a pilha: acorda o laco para ver o enlace na hora.
static void espnow_cutucar(void)
{
  if (!s_fila) { return; }
  evento_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = EV_NADA;
  xQueueSend(s_fila, &ev, 0);
}

void tab5_espnow_iniciar(void)
{
  if (s_fila) { return; }
  s_trava = xSemaphoreCreateMutex();
  s_fila  = xQueueCreateWithCaps(12, sizeof(evento_t), MALLOC_CAP_SPIRAM);
  s_notif = notif_registrar("ESP-NOW");
  s_notif_canal = notif_registrar("ESP-NOW canal");
  notif_associar_app(s_notif_canal, "ESP-NOW");
  // E um ESTADO (o WiFi solto do roteador), nao uma mensagem: mudar de canal
  // nao pode soar como notificacao nova.
  notif_silenciar(s_notif_canal, true);
  if (!s_trava || !s_fila) { ESP_LOGE(TAG, "sem memoria - servico fora do ar"); return; }
  // O canal escolhido da ultima vez. Nao aplica aqui: quem aplica e o PRONTO
  // do C6 (ele reaplica o canal fixo toda vez que o radio volta), entao basta
  // a vontade estar em s_canal_fixo antes disso.
  s_canal_fixo = settings_espnow_channel();
  tab5_time_on_jump(ao_saltar_relogio);
  tab5_wifi_on_stack_ready(espnow_cutucar);   // sem enlace a tarefa dorme 30 s; o Wi-Fi acorda ela
  // 6 KB, e nao 4: esta tarefa MONTA O CARTAO (gravar/carregar o historico),
  // e o f_mount do FATFS desce fundo na stack. Com 4 KB o monitor viu 140
  // bytes livres e o envio seguinte estourou a stack no meio da montagem
  // (Guru Meditation em sdmmc_read_sectors) - pico de ~4 KB. Com 8 KB, dois
  // dias de pilhas-*.csv deram ~2,6 KB. 6 KB cobre o pico da montagem com 2 KB
  // de folga; os buffers grandes daqui sao static (PSRAM), nao pilha.
  {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(vTaskEspnow, "espnow", 6144, nullptr, 3, &h, tskNO_AFFINITY);
    tab5_owner_register(h, OWNER_SERVICE, "ESP-NOW");   // ver hal/tab5_owners.hpp
  }
  ESP_LOGI(TAG, "servico no ar (espera o firmware nosso no C6)");
}

bool    tab5_espnow_pronto(void)          { return s_pronto; }
uint8_t tab5_espnow_canal(void)           { return s_canal; }
void    tab5_espnow_meu_mac(uint8_t out[6]) { memcpy(out, s_mac, 6); }
bool    tab5_espnow_c6_compativel(void)   { return tab5_wifi_c6_is_ours(); }

int tab5_espnow_n(void)
{
  if (!s_trava) { return 0; }
  travar(); const int n = s_n; destravar();
  return n;
}

bool tab5_espnow_msg(int i, espnow_msg_t* out)
{
  if (!s_trava || !out) { return false; }
  travar();
  const bool ok = (i >= 0 && i < s_n);
  if (ok) { *out = s_hist[(s_ini + i) % ESPNOW_HIST]; }
  destravar();
  return ok;
}

uint32_t tab5_espnow_total(void)   { return s_total; }
int      tab5_espnow_vistos(void)  { return s_n_vistos; }

uint32_t tab5_espnow_ms_desde_rx(void)
{
  if (!s_ultimo_rx_us) { return UINT32_MAX; }
  return (uint32_t)((esp_timer_get_time() - s_ultimo_rx_us) / 1000);
}

uint8_t tab5_espnow_canal_fixo(void) { return s_canal_fixo; }

// LOCAL, e nao estatico: o comentario dizia "so a interface
// chama", mas o console (comandos espnow canal/enviar) chama da tarefa DELE -
// duas tarefas no mesmo buffer estatico podiam mandar uma mensagem com o texto
// da outra. 266 bytes cabem folgados nas stacks das duas (8 e 12 KB).
bool tab5_espnow_definir_canal(uint8_t canal)
{
  if (!s_fila || canal > 13) { return false; }
  evento_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = EV_CANAL;
  ev.n = 1;
  ev.dados[0] = canal;
  return xQueueSend(s_fila, &ev, 0) == pdTRUE;
}

bool tab5_espnow_enviar(const char* texto)
{
  if (!s_fila || !texto || !texto[0]) { return false; }
  evento_t ev;   // local: ver tab5_espnow_definir_canal
  ev.type = EV_ENVIAR;
  size_t n = strlen(texto);
  if (n > C6_TEXTO_MAX) { n = C6_TEXTO_MAX; }
  ev.n = (uint16_t)n;
  memcpy(ev.dados, texto, n);
  return xQueueSend(s_fila, &ev, 0) == pdTRUE;
}

int tab5_espnow_nao_lidas(void) { return s_nao_lidas; }

void tab5_espnow_janela(bool aberta)
{
  s_janela = aberta;
  if (s_fila) {   // reavalia ja, sem esperar a volta de 1 s do loop
    evento_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_NADA;
    xQueueSend(s_fila, &ev, 0);
  }
}

void tab5_espnow_marcar_lidas(void)
{
  if (!s_trava) { return; }
  travar();
  if (s_nao_lidas) { s_nao_lidas = 0; publicar_notif(); }
  destravar();
}
