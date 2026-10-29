#include "tab5_log.hpp"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - large buffers in PSRAM
#include "tab5_sd.hpp"
#include "tab5_time.hpp"   // tab5_day_filename

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"   // xStreamBufferCreateWithCaps
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"   // esp_reset_reason
#include <time.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static const char* TAG = "log";

// 12KB - a few dozen lines of log on a system with several services
// speaking on the same team (GPS, Mesh, camera, interface) fits together calmly
// between two flushes of FLUSH_MS. A single fixed block, created once in the
// boot and never relocated again - does not fragment. Stays in PSRAM: 12KB fixed in
// Internal SRAM comes out of the stacks and DMA buffers of everything else. It can, because
// no one writes here with the cache disabled - the hook does not run in ISR, and the
// code of writing in flash logs via ROM (ESP_EARLY_LOG), not via this
// vprintf.
#define LOG_BUF_TAM    (12 * 1024)

// How often does the log (in task "card") empty the buffer on the card,
// for INFORMATIONAL lines. Warning and error do not wait (see log_na_carona). It was 15s
// and the lines "sd: mounted/unmounted" of each download
// fed back into the buffer, mounting the card every 15s forever.
#define LOG_FLUSH_MS   60000   // was 15000 - see "EM BATCH" in log_na_carona

// Each call on vprintf of ESP_LOG is ONE entire line already formatted (tag,
// level, message, counter of milliseconds, break of line - itself
// esp_log_writev assembles this string and only then calls this function). 200 bytes
// covers any normal line of this system; a longer and cut line,
// does not pop - see vsnprintf below.
#define LOG_LINE_SIZE  200

static StreamBufferHandle_t s_buf      = nullptr;
static volatile bool        s_urgente  = false;   // warning/error arrived: download now
static vprintf_like_t       s_original = nullptr;

// ONE WRITER AT A TIME. The stream buffer of FreeRTOS is made to
// ONE writer and ONE reader - the documentation requires serialization when there is more than
// one. And here there are many: every task that calls ESP_LOG writes (interface,
// WiFi, ESP-NOW, Mesh, the reception of esp_hosted...), and the esp_log of IDF (version
// 1, ours) DOES NOT lock anything before calling this function. Two lines on the same
// time could shuffle the index of buffer. Short wait, never infinite: if
// the lock doesn't come within 5 ms, the line exit on the console and just doesn't go on the card
// (counts as lost) - log can never lock anyone who logs in.
static SemaphoreHandle_t    s_write_lock = nullptr;

static uint32_t s_lines_pendentes = 0;   // queued, not yet recorded
static uint32_t s_lines_gravadas  = 0;   // confirmed on the card
static uint32_t s_lines_perdidas  = 0;   // buffer full, line discarded
static char     s_current_filename[24] = "";
static bool     s_boot_marcado     = false;

void tab5_log_estado(size_t* bytes_pendentes, uint32_t* lines_gravadas,
                      uint32_t* lines_perdidas, char* arquivo, size_t arquivo_size)
{
  if (bytes_pendentes) { *bytes_pendentes = s_buf ? xStreamBufferBytesAvailable(s_buf) : 0; }
  if (lines_gravadas) { *lines_gravadas = s_lines_gravadas; }
  if (lines_perdidas) { *lines_perdidas = s_lines_perdidas; }
  if (arquivo)          { snprintf(arquivo, arquivo_size, "%s", s_current_filename); }
}

// THE FUNCTION THAT REPLACES ESP_LOG'S vprintf - see the big warning in the .hpp
// about why it only FORMATS AND LINES, never writes on the card here.
//
// va_copy BEFORE touch in 'args': the original list is consumed by
// FIRST formatting to use (here, the original vprintf call) -
// reuse it after that and undefined behavior in several libc. By
// so we make a copy right away and format OUR copy from
// of it, never of original 'args' after it has already been used.
// COLOR MATRIX REFUSED BY ISP - known noise, filtered.
//
// In P4 v1.0 each coefficient of CCM fits in 13 bits with 2 of integer part
// (isp_ll.h: ISP_LL_CCM_MATRIX_INT_BITS) - maximum +-4, fixed on silicon. In a
// sudden turn of light, white balance demands more than that, the driver
// refuses and the ISP KEEP the last good matrix - which is precisely what leaves the
// right color (limiting the matrix to fit left the image yellowish, tested
// and undone). The effect is only in the log: five red lines for refusal.
//
// The string is recognized by the TEXT, not by the tag: 'isp_video',
// 'esp_video' and 'ISP' also carry true errors, which continue
// appearing. Just add what comes right after (10 ms) of one "out of range" of
// CCM. Each refusal becomes a counter; the of log task prints one summary, in
// maximum one line per minute.
static volatile int64_t  s_ccm_cadeia_ate = 0;
static volatile uint32_t s_ccm_recusas    = 0;

static bool ruido_of_ccm(const char* line)
{
  const int64_t now = esp_timer_get_time();
  if (strstr(line, "ISP_CCM") && strstr(line, "out of range")) {
    s_ccm_cadeia_ate = now + 10000;
    s_ccm_recusas = s_ccm_recusas + 1;
    return true;
  }
  if (now > s_ccm_cadeia_ate) { return false; }
  return strstr(line, "failed to configure CCM")   || strstr(line, "failed to reconfigure CCM")
      || strstr(line, "set_ext_ctrl=")             || strstr(line, "ISP: failed to set CCM")
      || strstr(line, "ISP: failed to set blue balance") || strstr(line, "ISP: failed to set red balance");
}

static int log_vprintf(const char* fmt, va_list args)
{
  char line[LOG_LINE_SIZE];
  va_list copia;
  va_copy(copia, args);
  int n = vsnprintf(line, sizeof(line), fmt, copia);
  va_end(copia);

  // Known noise of CCM: neither in serial nor on card (see above).
  if (n > 0 && ruido_of_ccm(line)) { return n; }

  // ORIGINAL with ORIGINAL arguments - console/serial continues
  // identical to who he always was, no one who already depends on him (himself
  // flash.sh, whoever follows it live) will notice any difference.
  const int ret = s_original ? s_original(fmt, args) : vprintf(fmt, args);

  if (n > 0 && s_buf && s_write_lock && !xPortInIsrContext()) {
    if ((size_t)n >= sizeof(line)) { n = sizeof(line) - 1; }   // truncated, not burst
    // WARNING AND ERROR DO NOT WAIT FOR THE BATCH: the line starts with "W (" / "E (" -
    // after the code of color, if any ("\033[0;33mW (").
    const char* c = line;
    if (c[0] == '\033') { while (*c && *c != 'm') { ++c; } if (*c) { ++c; } }
    if ((c[0] == 'W' || c[0] == 'E') && c[1] == ' ' && c[2] == '(') { s_urgente = true; }
    // With the scheduler stopped, you cannot wait (assert of FreeRTOS).
    const TickType_t wait_ticks = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? pdMS_TO_TICKS(5) : 0;
    if (xSemaphoreTake(s_write_lock, wait_ticks) == pdTRUE) {
      const size_t enviado = xStreamBufferSend(s_buf, line, (size_t)n, 0);   // 0 = never wait
      if (enviado < (size_t)n) { s_lines_perdidas++; }
      else                     { s_lines_pendentes++; }
      xSemaphoreGive(s_write_lock);
    } else {
      s_lines_perdidas++;
    }
  }
  return ret;
}

// LOCAL day in name, such as power and monitor (tab5_day_filename); the lines
// and the of boot tag remain in UTC.
static void today_log_filename(char* out, size_t size)
{
  tab5_day_filename((uint32_t)time(nullptr), "log_", ".txt", out, size);
}

// Empties whatever is in the buffer to the TODAY file. Call only by
// "card" task (log_na_carona), never by anyone is logging in.
//
// Through port tab5_sd_append (hal/tab5_sd.hpp): with the borrowed card
// on the computer, the batch goes to the diary in flash and arrives at the file in
// iteration - the log of a pendrive session is not lost.
static void descarregar(void)
{
  if (!s_buf || xStreamBufferIsEmpty(s_buf)) { return; }

  // IS THERE ANYWHERE TO RECORD? The card (insured until the end of batch, so as not to
  // disappear in the middle) or, with it borrowed, the diary. Without either NO
  // DISCARD: the lines remain in the buffer, waiting for the card to return (or
  // giving way to the younger ones if he fills first, the same policy that
  // xStreamBufferSend already applies).
  const bool montou = tab5_sd_mount();
  if (!montou && !tab5_sd_lent()) { return; }

  char name[24];
  today_log_filename(name, sizeof(name));

  // BOOT MARK, once per session - same reasoning as "BOOT" in
  // energy_log.cpp/energia.csv: how several boots of the same day fail on the SAME
  // file (see the big warning in the .hpp about "per day, not per boot"), without
  // this would make it impossible to know where a restart separates two sessions. O
  // "(N)" of each line of ESP_LOG and only milliseconds since THIS boot - a
  // tag ties this local clock to a true date/time.
  if (!s_boot_marcado) {
    const time_t now = time(nullptr);
    struct tm t = { };
    gmtime_r(&now, &t);
    char marca[80];
    const int n = snprintf(marca, sizeof(marca), "=== boot %04d-%02d-%02dT%02d:%02d:%02dZ, uptime %llums ===\n",
                           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                           (unsigned long long)(esp_timer_get_time() / 1000));
    if (tab5_sd_append(TAB5_SD_LOGS, name, marca, (size_t)n)) { s_boot_marcado = true; }
  }

  // Drains in blocks, without allocating by line, in a static block of size
  // entire buffer (12KB, in PSRAM - never in stack of anyone downloading).
  EXT_RAM_BSS_ATTR static uint8_t bloco[LOG_BUF_TAM];
  size_t total = 0;
  size_t n;
  while ((n = xStreamBufferReceive(s_buf, bloco, sizeof(bloco), 0)) > 0) {
    if (!tab5_sd_append(TAB5_SD_LOGS, name, bloco, n)) { s_lines_perdidas++; break; }
    total += n;
  }
  if (montou) { tab5_sd_unmount(); }

  if (total > 0) {
    snprintf(s_current_filename, sizeof(s_current_filename), "%s", name);
    s_lines_gravadas  += s_lines_pendentes;
    s_lines_pendentes  = 0;
  }
}

// IN BATCH, IN THE CARRONA OF THE TASK "card" (tab5_sd_carona) - the log does not have
// own task: the "card" already wakes up every 1 s and it's already who writes to the card.
// - informative wait for batch of LOG_FLUSH_MS (60s);
// - WARNING or ERROR goes on the card within 1s (and what you want to read after one
// crash);
// - buffer passing 3/4 unloads instantly, so as not to lose line.
// Added to the postponed unmounting of tab5_sd, upcoming recordings (log, monitor,
// energy) share a single assembly.
static void log_in_carona(void)
{
  static int64_t  since_us      = esp_timer_get_time();
  static int64_t  ccm_summary_us = since_us;
  static uint32_t ccm_contadas  = 0;
  const int64_t now = esp_timer_get_time();

  // The summary of filtered CCM refusals (see ccm_noise): one line, in
  // maximum per minute, only when there is a new refusal.
  const uint32_t recusas = s_ccm_recusas;
  if (recusas != ccm_contadas && now - ccm_summary_us >= 60000000LL) {
    ESP_LOGI(TAG, "camera: %lu color adjustment(s) outside the chip limit were ignored (expected, not an error)",
             (unsigned long)(recusas - ccm_contadas));
    ccm_contadas = recusas;
    ccm_summary_us = now;
  }
  const bool cheio = s_buf && xStreamBufferBytesAvailable(s_buf) > (LOG_BUF_TAM * 3) / 4;
  if (s_urgente || cheio || now - since_us >= (int64_t)LOG_FLUSH_MS * 1000) {
    s_urgente = false;
    since_us = now;
    descarregar();
  }
}

void tab5_log_descarregar_logo(void) { s_urgente = true; }

void tab5_log_start(void)
{
  // 1 byte of "trigger" - we do not use blocking xStreamBufferReceive
  s_buf = xStreamBufferCreateWithCaps(LOG_BUF_TAM, 1, MALLOC_CAP_SPIRAM);
  s_write_lock = xSemaphoreCreateMutex();              // BEFORE hook: it only writes with the lock
  if (!s_buf || !s_write_lock) {
    ESP_LOGE(TAG, "no RAM for the log buffer - card capture disabled");
    return;
  }

  // Store the CURRENT function before of switch - normally the default vprintf()
  // of IDF, but we do not assume: whatever is installed continues to receive
  // ALL log, only through our now function.
  s_original = esp_log_set_vprintf(log_vprintf);

  // Without its own task: unload() runs in the "card" task (see
  // log_na_carona). The block of drain of 12KB is 'static' in PSRAM, off of stack.
  tab5_sd_piggyback(log_in_carona);

  ESP_LOGI(TAG, "capture running: %dKB buffer, flush every %lus, /sdcard/apps/Sistema/logs/log_AAAA-MM-DD.txt",
           LOG_BUF_TAM / 1024, (unsigned long)(LOG_FLUSH_MS / 1000));

  // WHY THE DEVICE RESTART. A crash loses the lines that are still
  // were in the buffer (up to 60 s of informative log) - without this line, one
  // restart in the middle of something just seems like "the log ended there".
  const esp_reset_reason_t r = esp_reset_reason();
  const char* reason = "outro";
  switch (r) {
    case ESP_RST_POWERON:   reason = "powered on"; break;
    case ESP_RST_SW:        reason = "requested restart (esp_restart)"; break;
    case ESP_RST_PANIC:     reason = "TRAVOU (panic/excecao)"; break;
    case ESP_RST_INT_WDT:   reason = "TRAVOU (watchdog de interrupcao)"; break;
    case ESP_RST_TASK_WDT:  reason = "TASK WATCHDOG RESET"; break;
    case ESP_RST_WDT:       reason = "TRAVOU (outro watchdog)"; break;
    case ESP_RST_BROWNOUT:  reason = "queda de tensao"; break;
    case ESP_RST_DEEPSLEEP: reason = "acordou do sono profundo"; break;
    default: break;
  }
  if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
    ESP_LOGW(TAG, "boot: last reset was %s (code %d)", reason, (int)r);
  } else {
    ESP_LOGI(TAG, "boot: last reset was %s (code %d)", reason, (int)r);
  }
}
