#include "tab5_time.hpp"
#include "tab5_rtc.hpp"
#include "tab5_gps.hpp"
#include "tab5_mesh.hpp"
#include "tab5_wifi.hpp"
#include "settings.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include <ctime>
#include <cstring>
#include <cstdio>
#include <sys/time.h>

static const char* TAG = "hora";

// Stamp older than this and "this font has not yet synchronized"
// true" (2025-01-01T00:00:00Z).
#define EPOCH_MINIMO 1735689600u

// DOES NOT WRITE THE RTC ALL THE WAY - the chip has a finite writing life, and the
// system clock runs alone between adjustments. 10 minutes corrects the drift
// without hammering the I2C bus.
#define RESSINCRONIZAR_MS (10u * 60u * 1000u)

// A worse source only takes over after the best this team is left without a hit.
#define VALIDADE_MS (6u * 60u * 60u * 1000u)

// Below this the difference is drift, not the wrong clock: no one needs
// fix stamp for this.
#define SALTO_S 60

// An answer of NTP counts as "now" for this team (lwIP asks of
// new every CONFIG_LWIP_SNTP_UPDATE_DELAY, 1 h).
#define NTP_VALIDO_MS (2u * 60u * 60u * 1000u)

typedef enum { NIVEL_NENHUM = 0, NIVEL_MESH, NIVEL_GPS, NIVEL_NTP } level_t;
static const char* const NOME_NIVEL[] = { "relogio interno", "Mesh", "GPS", "internet" };
static const char* const PELO_NIVEL[] = { "pelo relogio interno", "pelo Mesh", "pelo GPS", "pela internet" };

static level_t  s_level          = NIVEL_NENHUM;
static uint32_t s_ultima_sync_ms = 0;

#define MAX_ASSINANTES 4
static tab5_time_jump_cb s_assinantes[MAX_ASSINANTES];


// -------------------------------------------------------------------------------
// NTP
// -------------------------------------------------------------------------------
// lwIP calls sntp_sync_time() with the received time, and the default version (weak)
// makes settimeofday in hour. This replaces it: the answer is only WRITE DOWN, and the
// hit goes through the same port as the other sources - with priority, the RTC
// and the jump warning. Run in task of lwIP; both fields are read in of
// interface, and one torn pair would cost at most one delayed hit.
static volatile uint32_t s_ntp_epoch = 0;
static volatile uint32_t s_ntp_ms    = 0;
static bool              s_sntp_of_pe = false;

extern "C" void sntp_sync_time(struct timeval* tv)
{
  s_ntp_ms    = tab5_now_ms();
  s_ntp_epoch = (uint32_t)tv->tv_sec;
}

static void garantir_sntp(void)
{
  if (s_sntp_of_pe || tab5_wifi_state() != TAB5_WIFI_CONNECTED) { return; }
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  if (esp_netif_sntp_init(&cfg) == ESP_OK) {
    s_sntp_of_pe = true;
    ESP_LOGI(TAG, "NTP ligado (pool.ntp.org)");
  }
}

static uint32_t hora_ntp(void)
{
  const uint32_t ms = s_ntp_ms, ep = s_ntp_epoch;
  if (ms == 0 || ep < EPOCH_MINIMO || (tab5_now_ms() - ms) > NTP_VALIDO_MS) { return 0; }
  return ep + (tab5_now_ms() - ms) / 1000;
}

// -------------------------------------------------------------------------------
// GPS - only with fix and with the receiver speaking now
// -------------------------------------------------------------------------------
// REPLACES timegm() - the libc of this IDF does not declare it in <ctime> without a
// macro of extension. Formula of Howard Hinnant (days_from_civil, domain
// public): UTC days since 1970-01-01, correct in any year.
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
  if (!tab5_gps_on_air()) { return 0; }   // RMC saved from one receiver that shut down
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

// -------------------------------------------------------------------------------
// THE AGREEMENT
// -------------------------------------------------------------------------------
static bool pode_usar(level_t level)
{
  if (level > s_level) { return true; }   // better source: in hora
  const uint32_t since = tab5_now_ms() - s_ultima_sync_ms;
  if (level == s_level) { return since >= RESSINCRONIZAR_MS; }
  return since >= VALIDADE_MS;            // the best disappeared from the team
}

// How much and which way the clock was off, in words. The log said
// "clock changed +1s", and it was impossible to know if the +1 was the clock moving
// forward, the correction or difference of spindle.
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

static bool acertar(uint32_t epoch, level_t level)
{
  const int64_t antes = (int64_t)time(nullptr);
  if (!tab5_rtc_set((time_t)epoch)) { return false; }
  const int64_t delta = (int64_t)epoch - antes;

  if (level != s_level || delta > SALTO_S || delta < -SALTO_S) {
    // LOCAL TIME in the log, the same as the bar shows. In UTC ("18:19Z") it
    // seemed wrong to anyone reading it, who is 3 hours ago.
    struct tm t = { };
    tab5_local_time(epoch, &t);
    char ajuste[48];
    descrever_ajuste(antes, delta, ajuste, sizeof(ajuste));
    ESP_LOGI(TAG, "hora acertada %s: %02d/%02d/%04d %02d:%02d:%02d (UTC%+d) - %s",
             PELO_NIVEL[level], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
             t.tm_hour, t.tm_min, t.tm_sec, (int)(tab5_utc_offset_s() / 3600), ajuste);
  }
  s_level = level;
  s_ultima_sync_ms = tab5_now_ms();

  // The old clock is only comparable if it was a moment of truth: leaving
  // 1970 (RTC never agreed) nothing was stamped - stamp below minimum
  // becomes "no date" everywhere.
  if ((delta > SALTO_S || delta < -SALTO_S) && antes > (int64_t)EPOCH_MINIMO) {
    for (int i = 0; i < MAX_ASSINANTES; ++i) {
      if (s_assinantes[i]) { s_assinantes[i]((int32_t)delta); }
    }
  }
  return true;
}

void tab5_time_sync(void)
{
  garantir_sntp();

  const uint32_t ntp = hora_ntp();
  if (ntp && pode_usar(NIVEL_NTP)) { acertar(ntp, NIVEL_NTP); return; }

  const uint32_t gps = hora_gps();
  if (gps && pode_usar(NIVEL_GPS)) { acertar(gps, NIVEL_GPS); return; }

  const uint32_t mesh = tab5_mesh_hora_alive();
  if (mesh > EPOCH_MINIMO && pode_usar(NIVEL_MESH)) { acertar(mesh, NIVEL_MESH); }
}

const char* tab5_time_source(void) { return NOME_NIVEL[s_level]; }

void tab5_time_on_jump(tab5_time_jump_cb cb)
{
  for (int i = 0; i < MAX_ASSINANTES; ++i) {
    if (s_assinantes[i] == cb) { return; }
    if (!s_assinantes[i]) { s_assinantes[i] = cb; return; }
  }
  ESP_LOGW(TAG, "assinantes de salto esgotados");
}

int32_t tab5_utc_offset_s(void) { return (int32_t)settings_utc_offset_hours() * 3600; }

void tab5_local_time(uint32_t epoch_utc, struct tm* out)
{
  const time_t deslocado = (time_t)epoch_utc + tab5_utc_offset_s();
  gmtime_r(&deslocado, out);
}

void tab5_day_filename(uint32_t epoch_utc, const char* prefixo, const char* sufixo,
                           char* out, size_t size)
{
  if (epoch_utc < 1577836800u) {   // 2020-01-01: before this the RTC was never agreed
    snprintf(out, size, "%ssem-data%s", prefixo, sufixo);
    return;
  }
  struct tm lt;
  tab5_local_time(epoch_utc, &lt);
  snprintf(out, size, "%s%04d-%02d-%02d%s", prefixo, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, sufixo);
}
