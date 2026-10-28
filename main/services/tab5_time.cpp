#include "tab5_time.hpp"
#include "tab5_rtc.hpp"
#include "tab5_gps.hpp"
#include "tab5_mesh.hpp"
#include "tab5_wifi.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include <ctime>
#include <cstring>
#include <cstdio>
#include <sys/time.h>

static const char* TAG = "hora";

// Carimbo mais antigo que isto e "essa fonte ainda nao sincronizou de
// verdade" (2025-01-01T00:00:00Z).
#define EPOCH_MINIMO 1735689600u

// NAO ESCREVE O RTC TODA VOLTA - o chip tem vida finita de escrita, e o
// relogio do sistema anda sozinho entre acertos. 10 minutos corrige a deriva
// sem martelar o barramento I2C.
#define RESSINCRONIZAR_MS (10u * 60u * 1000u)

// Uma fonte pior so assume depois de a melhor ficar este tempo sem acertar.
#define VALIDADE_MS (6u * 60u * 60u * 1000u)

// Abaixo disto a diferenca e deriva, nao relogio errado: ninguem precisa
// corrigir carimbo por isso.
#define SALTO_S 60

// Uma resposta do NTP vale como "agora" por este tempo (o lwIP pergunta de
// novo a cada CONFIG_LWIP_SNTP_UPDATE_DELAY, 1 h).
#define NTP_VALIDO_MS (2u * 60u * 60u * 1000u)

typedef enum { NIVEL_NENHUM = 0, NIVEL_MESH, NIVEL_GPS, NIVEL_NTP } nivel_t;
static const char* const NOME_NIVEL[] = { "relogio interno", "Mesh", "GPS", "internet" };
static const char* const PELO_NIVEL[] = { "pelo relogio interno", "pelo Mesh", "pelo GPS", "pela internet" };

static nivel_t  s_nivel          = NIVEL_NENHUM;
static uint32_t s_ultima_sync_ms = 0;

#define MAX_ASSINANTES 4
static tab5_hora_salto_cb s_assinantes[MAX_ASSINANTES];


// ---------------------------------------------------------------------------
// NTP
// ---------------------------------------------------------------------------
// O lwIP chama sntp_sync_time() com a hora recebida, e a versao padrao (weak)
// faz settimeofday na hora. Esta a substitui: a resposta so e ANOTADA, e o
// acerto passa pela mesma porta das outras fontes - com a prioridade, o RTC
// e o aviso de salto. Roda na tarefa do lwIP; os dois campos sao lidos na de
// interface, e um par rasgado custaria no maximo um acerto adiado.
static volatile uint32_t s_ntp_epoch = 0;
static volatile uint32_t s_ntp_ms    = 0;
static bool              s_sntp_de_pe = false;

extern "C" void sntp_sync_time(struct timeval* tv)
{
  s_ntp_ms    = tab5_agora_ms();
  s_ntp_epoch = (uint32_t)tv->tv_sec;
}

static void garantir_sntp(void)
{
  if (s_sntp_de_pe || tab5_wifi_state() != TAB5_WIFI_CONNECTED) { return; }
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  if (esp_netif_sntp_init(&cfg) == ESP_OK) {
    s_sntp_de_pe = true;
    ESP_LOGI(TAG, "NTP ligado (pool.ntp.org)");
  }
}

static uint32_t hora_ntp(void)
{
  const uint32_t ms = s_ntp_ms, ep = s_ntp_epoch;
  if (ms == 0 || ep < EPOCH_MINIMO || (tab5_agora_ms() - ms) > NTP_VALIDO_MS) { return 0; }
  return ep + (tab5_agora_ms() - ms) / 1000;
}

// ---------------------------------------------------------------------------
// GPS - so com fix e com o receptor falando agora
// ---------------------------------------------------------------------------
// SUBSTITUI timegm() - a libc deste IDF nao a declara em <ctime> sem uma
// macro de extensao. Formula de Howard Hinnant (days_from_civil, dominio
// publico): dias UTC desde 1970-01-01, correta em qualquer ano.
static time_t utc_para_epoch(const struct tm& t)
{
  int y = t.tm_year + 1900;
  const int m = t.tm_mon + 1;
  const int d = t.tm_mday;
  y -= (m <= 2);
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long dias = era * 146097 + (long)doe - 719468;
  return (time_t)dias * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

static uint32_t hora_gps(void)
{
  if (!tab5_gps_on_air()) { return 0; }   // RMC guardado de um receptor que calou
  const nmea_time_t g = tab5_gps_time();
  if (!g.valid || !g.fix) { return 0; }
  struct tm t = { };
  t.tm_year = g.ano - 1900;
  t.tm_mon  = g.mes - 1;
  t.tm_mday = g.dia;
  t.tm_hour = g.hora;
  t.tm_min  = g.min;
  t.tm_sec  = g.seg;
  const time_t e = utc_para_epoch(t);
  return (e > (time_t)EPOCH_MINIMO) ? (uint32_t)e : 0;
}

// ---------------------------------------------------------------------------
// O ACERTO
// ---------------------------------------------------------------------------
static bool pode_usar(nivel_t nivel)
{
  if (nivel > s_nivel) { return true; }   // fonte melhor: na hora
  const uint32_t desde = tab5_agora_ms() - s_ultima_sync_ms;
  if (nivel == s_nivel) { return desde >= RESSINCRONIZAR_MS; }
  return desde >= VALIDADE_MS;            // a melhor sumiu faz tempo
}

// Quanto e para que lado o relogio estava errado, em palavras. O log dizia
// "relogio mudou +1s", e nao dava para saber se o +1 era o relogio andando
// para frente, a correcao ou a diferenca de fuso.
static void descrever_ajuste(int64_t antes, int64_t delta, char* out, size_t n)
{
  if (antes < (int64_t)EPOCH_MINIMO) { snprintf(out, n, "o relogio ainda nao tinha hora"); return; }
  if (delta == 0)                     { snprintf(out, n, "o relogio ja estava certo"); return; }
  const long long a = (long long)(delta < 0 ? -delta : delta);
  const char* lado  = (delta > 0) ? "atrasado" : "adiantado";
  if      (a < 120)        { snprintf(out, n, "o relogio estava %lld s %s", a, lado); }
  else if (a < 2 * 3600)   { snprintf(out, n, "o relogio estava %lld min %s", a / 60, lado); }
  else if (a < 2 * 86400)  { snprintf(out, n, "o relogio estava %lld h %s", a / 3600, lado); }
  else                     { snprintf(out, n, "o relogio estava %lld dias %s", a / 86400, lado); }
}

static bool acertar(uint32_t epoch, nivel_t nivel)
{
  const int64_t antes = (int64_t)time(nullptr);
  if (!tab5_rtc_set((time_t)epoch)) { return false; }
  const int64_t delta = (int64_t)epoch - antes;

  if (nivel != s_nivel || delta > SALTO_S || delta < -SALTO_S) {
    // HORA LOCAL no log, a mesma que a barra mostra. Em UTC ("18:19Z") ela
    // parecia errada para quem le, que esta 3 h atras.
    struct tm t = { };
    tab5_hora_local(epoch, &t);
    char ajuste[48];
    descrever_ajuste(antes, delta, ajuste, sizeof(ajuste));
    ESP_LOGI(TAG, "hora acertada %s: %02d/%02d/%04d %02d:%02d:%02d (UTC%+d) - %s",
             PELO_NIVEL[nivel], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
             t.tm_hour, t.tm_min, t.tm_sec, TAB5_FUSO_S / 3600, ajuste);
  }
  s_nivel = nivel;
  s_ultima_sync_ms = tab5_agora_ms();

  // O relogio velho so e comparavel se era uma hora de verdade: saindo de
  // 1970 (RTC nunca acertado) nada foi carimbado - carimbo abaixo do minimo
  // vira "sem data" em todo lugar.
  if ((delta > SALTO_S || delta < -SALTO_S) && antes > (int64_t)EPOCH_MINIMO) {
    for (int i = 0; i < MAX_ASSINANTES; ++i) {
      if (s_assinantes[i]) { s_assinantes[i]((int32_t)delta); }
    }
  }
  return true;
}

void tab5_hora_sincronizar(void)
{
  garantir_sntp();

  const uint32_t ntp = hora_ntp();
  if (ntp && pode_usar(NIVEL_NTP)) { acertar(ntp, NIVEL_NTP); return; }

  const uint32_t gps = hora_gps();
  if (gps && pode_usar(NIVEL_GPS)) { acertar(gps, NIVEL_GPS); return; }

  const uint32_t mesh = tab5_mesh_hora_viva();
  if (mesh > EPOCH_MINIMO && pode_usar(NIVEL_MESH)) { acertar(mesh, NIVEL_MESH); }
}

const char* tab5_hora_fonte(void) { return NOME_NIVEL[s_nivel]; }

void tab5_hora_ao_saltar(tab5_hora_salto_cb cb)
{
  for (int i = 0; i < MAX_ASSINANTES; ++i) {
    if (s_assinantes[i] == cb) { return; }
    if (!s_assinantes[i]) { s_assinantes[i] = cb; return; }
  }
  ESP_LOGW(TAG, "assinantes de salto esgotados");
}

void tab5_hora_local(uint32_t epoch_utc, struct tm* out)
{
  const time_t deslocado = (time_t)epoch_utc + TAB5_FUSO_S;
  gmtime_r(&deslocado, out);
}

void tab5_hora_nome_do_dia(uint32_t epoch_utc, const char* prefixo, const char* sufixo,
                           char* out, size_t tam)
{
  if (epoch_utc < 1577836800u) {   // 2020-01-01: antes disso o RTC nunca foi acertado
    snprintf(out, tam, "%ssem-data%s", prefixo, sufixo);
    return;
  }
  struct tm lt;
  tab5_hora_local(epoch_utc, &lt);
  snprintf(out, tam, "%s%04d-%02d-%02d%s", prefixo, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, sufixo);
}
