#include "tab5_monitor.hpp"
#include "tab5_owners.hpp"
#include "tab5_marks.hpp"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - large buffers in PSRAM

#include <string.h>
#include "task_pool.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "tab5_sd.hpp"
#include "tab5_time.hpp"
#include "tab5_frame.hpp"
#include "compositor.hpp"   // counter of underrun of panel
#include "app.h"            // current_app_system

static const char* TAG = "monitor";
static const char* PASTA = "Tasks";

// Photo of composer in the last minute recorded (frames-*.csv).
static comp_estat_t s_qd_ant = {};
static uint32_t     s_qd_amostras = 0;

// -------------------------------------------------------------------------------
// THE TRIGGER - small and cheap, all by BORDA
// -------------------------------------------------------------------------------
// Each one fires ONCE upon crossing the limit and only resets after returning with
// slack (hysteresis) - a memory oscillating in iteration of limit does not become a
// flurry of events.
static const uint32_t RAM_BAIXA      = 48 * 1024;    // free internal RAM below this
static const uint32_t RAM_FOLGA      = 16 * 1024;
static const uint32_t BLOCO_BAIXO    = 16 * 1024;    // largest internal block below this (fragmentation)
static const uint32_t BLOCO_FOLGA    =  8 * 1024;
static const uint32_t PSRAM_BAIXA    = 2 * 1024 * 1024;
static const uint32_t PSRAM_FOLGA    = 512 * 1024;
static const uint32_t NOVO_MIN_PASSO =  4 * 1024;    // new record of low RAM, of 4KB in 4KB
static const uint32_t QUEDA_BRUSCA   = 32 * 1024;    // Internal RAM dropped this in 1 s
static const uint32_t PILHA_BAIXA    = 512;          // bytes left in stack of a task
static const float    CPU_TAREFA_ALTA = 60.0f, CPU_TAREFA_OK = 40.0f;
static const int      CPU_TAREFA_SEG  = 3;           // seconds in a row above
static const float    CPU_TOTAL_ALTA  = 90.0f, CPU_TOTAL_OK = 70.0f;
static const int      CPU_TOTAL_SEG   = 5;
static const uint32_t ACOMODAR_S      = 20;          // boot allocates of everything: remember only after this

// Short-lived tasks of the kernel itself: birth and disappearance and their work,
// no news (one per opening of app).
static bool efemera(const char* n)
{
  return strcmp(n, "sd_monta") == 0 || strcmp(n, "sd_desmonta") == 0;
}

// -------------------------------------------------------------------------------
// STATUS
// -------------------------------------------------------------------------------
static SemaphoreHandle_t s_mutex = nullptr;
EXT_RAM_BSS_ATTR static mon_foto_t s_foto;
static bool              s_tem_foto = false;

// In PSRAM (3.6 KB): read 1x/s, only by the Tasks app - the internal RAM and batteries and DMA.
EXT_RAM_BSS_ATTR static float    s_h_cpu[MON_HIST_N];
EXT_RAM_BSS_ATTR static uint32_t s_h_ram[MON_HIST_N], s_h_psram[MON_HIST_N];
static int      s_h_n = 0, s_h_i = 0;

EXT_RAM_BSS_ATTR static mon_evento_t s_ev[MON_EVENTOS_N];
static int          s_ev_n = 0, s_ev_i = 0;
static uint32_t     s_ev_total = 0;

// Lines waiting for the card (written once per iteration, not every event).
#define PENDENTES_N 16
EXT_RAM_BSS_ATTR static char s_pend[PENDENTES_N][112];   // ditto (1.8 KB)
static int  s_n_pend = 0;

// Memory of sampling task (only it rings).
typedef struct { UBaseType_t numero; uint64_t time; int seg_alta; bool alerta_cpu; bool alerta_stack; } anterior_t;
static anterior_t    s_ant[MON_MAX_TAREFAS];
static int           s_n_ant = 0;
static TaskStatus_t* s_status = nullptr;

static bool     s_ram_alerta = false, s_bloco_alerta = false, s_psram_alerta = false, s_cpu_alerta = false;
static int      s_cpu_seg_alta = 0;
static uint32_t s_ram_recorde_avisado = UINT32_MAX;
static uint32_t s_ram_anterior = 0;
static uint32_t s_underruns_ant = 0;
static uint32_t s_atrasos_ant = 0;
static int64_t  s_foto_pior_us = 0;   // the worst duration of uxTaskGetSystemState
static uint32_t s_dsi_erros_ant = 0;

// MINIMUM STACK BY TASK NAME, to size the stacks with real data.
// The high water mark of a task disappears when it dies (sd_monta, grove_sonda,
// tasks of app), and the Tasks app only shows who is alive NOW. This table
// stores the lowest value ever seen by name and goes on the card every 10 min.
#define PILHAS_N        48
#define PILHAS_PERIODO  600
EXT_RAM_BSS_ATTR static char s_stack_name[PILHAS_N][MON_NOME];
static uint32_t s_stack_min[PILHAS_N];
static int      s_stack_n = 0;
static uint32_t s_stack_gravada_em = 0;
EXT_RAM_BSS_ATTR static char s_stack_line[PILHAS_N * (MON_NOME + 10) + 32];

// Memory of minutes (fragmentation of internal RAM).
static uint32_t s_mm_ram_min = UINT32_MAX, s_mm_bloco_min = UINT32_MAX, s_mm_psram_min = UINT32_MAX;
static uint32_t s_mm_blocos_max = 0, s_mm_ocupadas_max = 0;
static float    s_mm_frag_max = 0;
static int      s_mm_amostras = 0;

// Summary of minute.
static float    s_min_cpu_max = 0;
static uint32_t s_min_ram = UINT32_MAX, s_min_psram = UINT32_MAX, s_min_bloco = UINT32_MAX;
static int      s_min_amostras = 0;

// ---- extract of internal RAM (see tab5_monitor.hpp) ----
// No lock: the index is reserved with one atomic increment, then two
// simultaneous milestones fall in different slots. Full, new landmarks are
// discarded - what matters is the boot and the first rise of each radio.
static memory_marco_t s_marcos[MEM_MARCOS_N];
static uint32_t    s_marcos_n = 0;
static uint32_t    s_marco_livre_ant = 0;

void tab5_memory_marco(const char* etapa)
{
  multi_heap_info_t hi;
  heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
  const uint32_t livre = (uint32_t)hi.total_free_bytes;
  const uint32_t ant   = __atomic_exchange_n(&s_marco_livre_ant, livre, __ATOMIC_RELAXED);
  const int32_t  delta = ant ? (int32_t)livre - (int32_t)ant : 0;

  ESP_LOGI("mem", "%-26s RAM livre %6lu B (%+7ld)  maior bloco %6lu B  minimo %6lu B",
           etapa, (unsigned long)livre, (long)delta, (unsigned long)hi.largest_free_block,
           (unsigned long)hi.minimum_free_bytes);

  const uint32_t i = __atomic_fetch_add(&s_marcos_n, 1, __ATOMIC_RELAXED);
  if (i >= MEM_MARCOS_N) { return; }
  memory_marco_t& m = s_marcos[i];
  m.ms          = tab5_now_ms();
  m.livre       = livre;
  m.maior_bloco = (uint32_t)hi.largest_free_block;
  m.minimum      = (uint32_t)hi.minimum_free_bytes;
  m.delta       = delta;
  snprintf(m.etapa, sizeof(m.etapa), "%s", etapa);
}

int tab5_memory_extrato(memory_marco_t* out, int max)
{
  uint32_t n = __atomic_load_n(&s_marcos_n, __ATOMIC_RELAXED);
  if (n > MEM_MARCOS_N) { n = MEM_MARCOS_N; }
  if ((int)n > max)     { n = (uint32_t)max; }
  memcpy(out, s_marcos, n * sizeof(memory_marco_t));
  return (int)n;
}

static inline uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000LL); }

static uint32_t epoch_valido(void) { return tab5_time_epoch(); }

static void name_of_dia(const char* prefixo, char* out, size_t size)
{
  char pre[24];
  snprintf(pre, sizeof(pre), "%s-", prefixo);
  tab5_day_filename(epoch_valido(), pre, ".csv", out, size);
}

static void hora_text(char* out, size_t size)
{
  const uint32_t e = epoch_valido();
  if (!e) { snprintf(out, size, "--:--:--"); return; }
  struct tm lt;
  tab5_local_time(e, &lt);
  snprintf(out, size, "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
}

// -------------------------------------------------------------------------------
// EVENT: memory + log + queue of card
// -------------------------------------------------------------------------------
static void evento(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void evento(const char* fmt, ...)
{
  mon_evento_t e;
  e.uptime_s = uptime_s();
  e.epoch    = epoch_valido();
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.text, sizeof(e.text), fmt, ap);
  va_end(ap);

  ESP_LOGW(TAG, "%s", e.text);

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_ev[s_ev_i] = e;
  s_ev_i = (s_ev_i + 1) % MON_EVENTOS_N;
  if (s_ev_n < MON_EVENTOS_N) { ++s_ev_n; }
  ++s_ev_total;
  xSemaphoreGive(s_mutex);

  if (s_n_pend < PENDENTES_N) {
    char h[12];
    hora_text(h, sizeof(h));
    snprintf(s_pend[s_n_pend++], sizeof(s_pend[0]), "%s,%lu,%s\n",
             h, (unsigned long)e.uptime_s, e.text);
  }
}

// By system recorder (hal/tab5_sd.hpp) - deliver and continue.
static void anexar(const char* prefixo, const char* cabecalho, const char* lines, int n, size_t passo)
{
  if (n <= 0) { return; }
  char name[40];
  name_of_dia(prefixo, name, sizeof(name));
  for (int i = 0; i < n; ++i) { tab5_sd_enqueue_append(PASTA, name, lines + i * passo, cabecalho); }
}

// -------------------------------------------------------------------------------
// THE ONE-SECOND SAMPLE
// -------------------------------------------------------------------------------
static anterior_t* anterior_of(UBaseType_t numero)
{
  for (int i = 0; i < s_n_ant; ++i) { if (s_ant[i].numero == numero) { return &s_ant[i]; } }
  return nullptr;
}

static void read_memory(mon_memory_t* m, const mon_memory_t* velho, uint32_t caps, uint32_t now)
{
  m->livre       = (uint32_t)heap_caps_get_free_size(caps);
  m->total       = (uint32_t)heap_caps_get_total_size(caps);
  m->maior_bloco = (uint32_t)heap_caps_get_largest_free_block(caps);
  m->min_livre    = velho->min_livre;    m->min_livre_em = velho->min_livre_em;
  m->min_bloco    = velho->min_bloco;    m->min_bloco_em = velho->min_bloco_em;
  if (m->livre < m->min_livre)       { m->min_livre = m->livre;       m->min_livre_em = now; }
  if (m->maior_bloco < m->min_bloco) { m->min_bloco = m->maior_bloco; m->min_bloco_em = now; }
}

static void anotar_stack(const char* name, uint32_t livre)
{
  int k = 0;
  while (k < s_stack_n && strncmp(s_stack_name[k], name, MON_NOME) != 0) { ++k; }
  if (k == s_stack_n) {
    if (s_stack_n >= PILHAS_N) { return; }
    snprintf(s_stack_name[k], MON_NOME, "%s", name);
    s_stack_min[k] = livre;
    ++s_stack_n;
  } else if (livre < s_stack_min[k]) {
    s_stack_min[k] = livre;
  }
}

// Vacancy of pool with an execution inside: the util name is the one of DONO ("Mesh"), no
// o of vacancy ("pool2").
static const char* name_efetivo(const TaskStatus_t& st)
{
  const char* n = st.pcTaskName ? st.pcTaskName : "?";
  if (strncmp(n, "pool", 4) == 0) {
    const char* dono = nullptr;
    tab5_owner_of(st.xHandle, &dono);
    if (dono && dono[0]) { return dono; }
  }
  return n;
}

static void amostrar(bool primeira)
{
  const uint32_t now = uptime_s();
  // uxTaskGetSystemState runs ENTIRELY in a critical section (in the SMP of IDF:
  // interrupts turned off on this core), measuring the free stack of each
  // task inside - ~0.8 ms with the CPU at 360 MHz. And more than the slack of
  // ~0.5 ms that the DMA of panel has between frames: falling into it, you lose one
  // entire frame. IT WAS THE CAUSE of "FRAME DELAYED" with the two colors in
  // IDLE - this loop runs in fixed phase of 1 s, and all
  // delays had the same fraction of seconds. That's why the photo is only taken
  // soon AFTER one end of frame (tab5_frame_wait_for_end).
  // AT REST THE PANEL IS DISASSEMBLED: there is no end of frame to wait
  // (no frame to lose). The wait was only due for the deadline - 40 awakenings
  // of 1 tick, every second, with the screen off.
  if (!tab5_power_is_sleeping()) { tab5_frame_wait_for_end(40); }
  const int64_t t_foto = esp_timer_get_time();
  const UBaseType_t n = uxTaskGetSystemState(s_status, MON_MAX_TAREFAS, nullptr);
  const int64_t us_foto = esp_timer_get_time() - t_foto;
  tab5_mark("monitor: foto das tarefas us", (int32_t)us_foto);
  if (us_foto > s_foto_pior_us) {
    s_foto_pior_us = us_foto;
    // Above 5 ms not even the window between frames covers (~14 ms, but the limit
    // of truth depends on where the wait woke up) - worth knowing.
    if (us_foto > 5000 && !tab5_power_is_sleeping()) {
      evento("foto das tarefas segurou o nucleo %d por %lld us", (int)xPortGetCoreID(), (long long)us_foto);
    }
  }
  if (n == 0) {
    // More tasks than vacancies: FreeRTOS doesn't fill anything. Notify me once
    // Instead of stopping the photo in silence.
    static bool avisou = false;
    if (!avisou) {
      avisou = true;
      evento("monitor cego: %u tarefas, so cabem %d", (unsigned)uxTaskGetNumberOfTasks(), MON_MAX_TAREFAS);
    }
    return;
  }

  uint64_t deltas[MON_MAX_TAREFAS];
  uint64_t total = 0;
  for (UBaseType_t i = 0; i < n; ++i) {
    const anterior_t* a = anterior_of(s_status[i].xTaskNumber);
    const uint64_t current = (uint64_t)s_status[i].ulRunTimeCounter;
    const uint64_t antes = a ? a->time : current;   // new task: starts from scratch
    deltas[i] = (current >= antes) ? (current - antes) : current;
    total += deltas[i];
  }

  // Stats: ~1.7KB each, and only this task uses them - in stack, along with
  // fprintf of card, they would squeeze its 6KB.
  EXT_RAM_BSS_ATTR static mon_foto_t nova, velha;
  memset(&nova, 0, sizeof(nova));
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  velha = s_foto;
  xSemaphoreGive(s_mutex);

  nova.uptime_s = now;
  nova.n = (int)n;
  nova.foto_us      = (uint32_t)us_foto;
  nova.foto_pior_us = (uint32_t)s_foto_pior_us;
  float idle = 0;
  for (UBaseType_t i = 0; i < n; ++i) {
    mon_tarefa_t& t = nova.t[i];
    snprintf(t.name, sizeof(t.name), "%s", name_efetivo(s_status[i]));
    t.numero      = s_status[i].xTaskNumber;
    t.priority  = s_status[i].uxCurrentPriority;
    t.stack_livre = (uint32_t)s_status[i].usStackHighWaterMark * sizeof(StackType_t);
    const char* dono = nullptr;
    t.type = (uint8_t)tab5_owner_of(s_status[i].xHandle, &dono);
    snprintf(t.dono, sizeof(t.dono), "%s", dono ? dono : "");
#if ( configTASKLIST_INCLUDE_COREID == 1 )
    t.core = s_status[i].xCoreID;
#else
    t.core = tskNO_AFFINITY;
#endif
    t.pct = total ? (100.0f * (float)deltas[i] / (float)total) : 0.0f;
    if (strncmp(t.name, "IDLE", 4) == 0) { idle += t.pct; }

    // The peak of this task comes from the previous photo (if it already existed).
    t.pct_pico = t.pct; t.pct_pico_em = now;
    for (int k = 0; k < velha.n; ++k) {
      if (velha.t[k].numero == t.numero && velha.t[k].pct_pico > t.pct) {
        t.pct_pico = velha.t[k].pct_pico; t.pct_pico_em = velha.t[k].pct_pico_em;
      }
    }
  }
  // Stable order on screen: by xTaskNumber (order of creation).
  for (int i = 1; i < nova.n; ++i) {
    mon_tarefa_t c = nova.t[i]; int j = i - 1;
    while (j >= 0 && nova.t[j].numero > c.numero) { nova.t[j + 1] = nova.t[j]; --j; }
    nova.t[j + 1] = c;
  }

  // Minimum stack by name (only this task touches the table). Pool vacancies
  // enter the name of EXECUCAO (more below); Here are just the common tasks
  // and vacancies with a known owner.
  for (int i = 0; i < nova.n; ++i) {
    const mon_tarefa_t& t = nova.t[i];
    if (strncmp(t.name, "IDLE", 4) == 0 || strncmp(t.name, "pool", 4) == 0) { continue; }
    anotar_stack(t.name, t.stack_livre);
  }
  {
    pool_stack_t pp[24];
    const int np = pool_pilhas(pp, 24);
    for (int i = 0; i < np; ++i) { anotar_stack(pp[i].name, pp[i].livre_min); }
  }

  nova.cpu = 100.0f - idle;
  if (nova.cpu < 0) { nova.cpu = 0; }
  if (nova.cpu > 100) { nova.cpu = 100; }
  nova.cpu_pico = velha.cpu_pico; nova.cpu_pico_em = velha.cpu_pico_em;
  if (primeira || nova.cpu > nova.cpu_pico) { nova.cpu_pico = nova.cpu; nova.cpu_pico_em = now; }

  if (primeira) {
    nova.ram.min_livre = nova.ram.min_bloco = UINT32_MAX;
    nova.psram.min_livre = nova.psram.min_bloco = UINT32_MAX;
    read_memory(&nova.ram,   &nova.ram,   MALLOC_CAP_INTERNAL, now);
    read_memory(&nova.psram, &nova.psram, MALLOC_CAP_SPIRAM,   now);
  } else {
    read_memory(&nova.ram,   &velha.ram,   MALLOC_CAP_INTERNAL, now);
    read_memory(&nova.psram, &velha.psram, MALLOC_CAP_SPIRAM,   now);
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_foto = nova;
  s_tem_foto = true;
  s_h_cpu[s_h_i] = nova.cpu; s_h_ram[s_h_i] = nova.ram.livre; s_h_psram[s_h_i] = nova.psram.livre;
  s_h_i = (s_h_i + 1) % MON_HIST_N;
  if (s_h_n < MON_HIST_N) { ++s_h_n; }
  xSemaphoreGive(s_mutex);

  // ---- triggers -----------------------------------------------------------
  if (!primeira) {
    const mon_memory_t& r = nova.ram;
    if (!s_ram_alerta && r.livre < RAM_BAIXA) {
      s_ram_alerta = true;
      evento("RAM baixa: %luK livres (maior bloco %luK)", (unsigned long)(r.livre / 1024), (unsigned long)(r.maior_bloco / 1024));
    } else if (s_ram_alerta && r.livre > RAM_BAIXA + RAM_FOLGA) {
      s_ram_alerta = false;
      evento("RAM normalizada: %luK livres", (unsigned long)(r.livre / 1024));
    }
    if (!s_bloco_alerta && r.maior_bloco < BLOCO_BAIXO) {
      s_bloco_alerta = true;
      evento("RAM fragmentada: maior bloco %luK (livre %luK)", (unsigned long)(r.maior_bloco / 1024), (unsigned long)(r.livre / 1024));
    } else if (s_bloco_alerta && r.maior_bloco > BLOCO_BAIXO + BLOCO_FOLGA) {
      s_bloco_alerta = false;
    }
    if (!s_psram_alerta && nova.psram.livre < PSRAM_BAIXA) {
      s_psram_alerta = true;
      evento("PSRAM baixa: %luK livres", (unsigned long)(nova.psram.livre / 1024));
    } else if (s_psram_alerta && nova.psram.livre > PSRAM_BAIXA + PSRAM_FOLGA) {
      s_psram_alerta = false;
    }
    if (now >= ACOMODAR_S && r.livre + NOVO_MIN_PASSO <= s_ram_recorde_avisado) {
      if (s_ram_recorde_avisado != UINT32_MAX) {
        evento("novo minimo de RAM: %luK livres", (unsigned long)(r.livre / 1024));
      }
      s_ram_recorde_avisado = r.livre;
    }
    if (s_ram_anterior > r.livre + QUEDA_BRUSCA) {
      evento("RAM caiu %luK em 1s (%luK -> %luK)", (unsigned long)((s_ram_anterior - r.livre) / 1024),
             (unsigned long)(s_ram_anterior / 1024), (unsigned long)(r.livre / 1024));
    }
    if (nova.cpu > CPU_TOTAL_ALTA) {
      if (++s_cpu_seg_alta >= CPU_TOTAL_SEG && !s_cpu_alerta) {
        s_cpu_alerta = true;
        evento("CPU acima de %.0f%% ha %ds", (double)CPU_TOTAL_ALTA, s_cpu_seg_alta);
      }
    } else {
      s_cpu_seg_alta = 0;
      if (s_cpu_alerta && nova.cpu < CPU_TOTAL_OK) { s_cpu_alerta = false; }
    }
  }
  s_ram_anterior = nova.ram.livre;

  // THE BLUE FRAME BECOMES AN EVENT. The lookout of underrun
  // (hal/tab5_frame) counts each time the panel became undated; here every
  // increase becomes an event with the APP THAT WAS OPEN - the blue blinking stops
  // be a report and now have a time and place.
  const uint32_t und = tab5_frame_underruns();
  if (!primeira && und > s_underruns_ant) {
    const char* app = system_app_current();
    evento("UNDERRUN do painel (+%lu) com %s aberto", (unsigned long)(und - s_underruns_ant),
           app ? app : "a home");
  }
  s_underruns_ant = und;

  // INTERFACE HAS STOPPED? (screen frozen, no touch, nothing in the log). Its loop counts turns and leaves the name of the step in which it is
  // (kernel/core.cpp, STEP). Stopped for more than 3 s: event with step - one
  // once, and another time when you return. The watchdog of tasks catches the loop
  // BLOCKED; This also catches the loop that stopped doing iteration for another reason.
  {
    static bool     s_presa = false;
    static uint32_t s_presa_since = 0;
    system_diag_t d;
    system_diag_interface(&d);
    if (!s_presa && d.voltas > 0 && d.ms_since_iteration > 3000) {
      s_presa = true;
      s_presa_since = d.voltas;
      evento("INTERFACE PRESA ha %lus em: %s", (unsigned long)(d.ms_since_iteration / 1000), d.etapa);
      evento("  com %s%s; dedo lido ha %lds", d.app ? d.app : "a home",
             d.sleep ? " (repouso)" : "",
             d.ms_since_finger == UINT32_MAX ? -1L : (long)(d.ms_since_finger / 1000));
    } else if (s_presa && d.voltas != s_presa_since) {
      s_presa = false;
      evento("interface voltou (parada em: %s)", d.etapa);
    }
  }

  // Detectors that do not depend on log of driver (see hal/tab5_frame):
  // frame delayed (DMA reconnected late) and error in the host DSI.
  {
    uint32_t pior_us = 0, periodo_us = 0;
    const uint32_t atr = tab5_frame_delays(&pior_us, &periodo_us);
    if (!primeira && atr > s_atrasos_ant) {
      const char* app = system_app_current();
      evento("QUADRO ATRASADO (+%lu) pior %lu.%lums (normal %lu.%lums) com %s",
             (unsigned long)(atr - s_atrasos_ant),
             (unsigned long)(pior_us / 1000), (unsigned long)(pior_us % 1000 / 100),
             (unsigned long)(periodo_us / 1000), (unsigned long)(periodo_us % 1000 / 100),
             app ? app : "a home");
      int nucleo = -1; char t0[16], t1[16];
      tab5_frame_delay_source(&nucleo, t0, t1);
      evento("  atraso: ISR no nucleo %d; rodando c0=%s c1=%s", nucleo, t0, t1);
      // WHAT HAPPENED CLOSELY: how much time before the delay did the card set up/
      // dismounted the truth and exited the last frame (and of what size).
      const int64_t t = tab5_frame_delay_time();
      int64_t mont = 0, desm = 0, qd = 0; uint32_t px = 0;
      tab5_sd_last_transitions(&mont, &desm);
      comp_ultimo_quadro(&qd, &px);
      auto ms = [&](int64_t x) { return x ? (long)((t - x) / 1000) : -1L; };
      evento("  em %ld.%03lds: sd +%ldms -%ldms, quadro %ldms antes (%luKpx)",
             (long)(t / 1000000), (long)(t / 1000 % 1000), ms(mont), ms(desm), ms(qd),
             (unsigned long)(px / 1000));
      // THE MILESTONES OF THE PREVIOUS 300 ms (hal/tab5_marks): changes of clock of
      // CPU (and with them the readjustment of bus of PSRAM), sleep, polling
      // of Grove... Directly in the log, and not in event(), because the line is long.
      tab5_mark_t m[12];
      const int nm = tab5_marks_between(t - 300000, t + 2000, m, 12);
      if (nm == 0) {
        ESP_LOGW(TAG, "  marcos: nenhum nos 300 ms antes");
      } else {
        char line[400];
        int k = snprintf(line, sizeof(line), "  marcos:");
        for (int i = 0; i < nm && k < (int)sizeof(line) - 48; ++i) {
          k += snprintf(line + k, sizeof(line) - k, " [%+ldms %s %ld]",
                        (long)((m[i].us - t) / 1000), m[i].what, (long)m[i].value);
        }
        ESP_LOGW(TAG, "%s", line);
      }
    }
    s_atrasos_ant = atr;

    uint32_t st0 = 0, st1 = 0;
    const uint32_t err = tab5_frame_dsi_errors(&st0, &st1);
    if (!primeira && err > s_dsi_erros_ant) {
      const char* app = system_app_current();
      evento("DSI host: erro st0=%08lx st1=%08lx (+%lu quadros) com %s",
             (unsigned long)st0, (unsigned long)st1, (unsigned long)(err - s_dsi_erros_ant),
             app ? app : "a home");
    }
    s_dsi_erros_ant = err;
  }

  // By task: CPU stuck high, stack at end, birth. The previous state
  // of each one is reconstructed here - the ones that disappeared are left out.
  static anterior_t novo_ant[MON_MAX_TAREFAS];
  for (UBaseType_t i = 0; i < n; ++i) {
    const char* name = name_efetivo(s_status[i]);
    const anterior_t* a = anterior_of(s_status[i].xTaskNumber);
    anterior_t& b = novo_ant[i];
    b.numero       = s_status[i].xTaskNumber;
    b.time        = (uint64_t)s_status[i].ulRunTimeCounter;
    b.seg_alta     = a ? a->seg_alta : 0;
    b.alerta_cpu   = a ? a->alerta_cpu : false;
    b.alerta_stack = a ? a->alerta_stack : false;

    if (!a && !primeira && !efemera(name)) { evento("tarefa nova: %s", name); }

    const float pct = total ? (100.0f * (float)deltas[i] / (float)total) : 0.0f;
    if (strncmp(name, "IDLE", 4) != 0) {
      if (pct > CPU_TAREFA_ALTA) {
        if (++b.seg_alta >= CPU_TAREFA_SEG && !b.alerta_cpu) {
          b.alerta_cpu = true;
          evento("%s em %.0f%% de CPU ha %ds", name, (double)pct, b.seg_alta);
        }
      } else {
        b.seg_alta = 0;
        if (pct < CPU_TAREFA_OK) { b.alerta_cpu = false; }
      }
    }
    const uint32_t stack = (uint32_t)s_status[i].usStackHighWaterMark * sizeof(StackType_t);
    if (!b.alerta_stack && stack < PILHA_BAIXA) {
      b.alerta_stack = true;
      evento("pilha de %s quase no fim: %luB livres", name, (unsigned long)stack);
    }
  }
  if (!primeira) {
    for (int k = 0; k < s_n_ant; ++k) {
      bool alive = false;
      for (UBaseType_t i = 0; i < n; ++i) { if (novo_ant[i].numero == s_ant[k].numero) { alive = true; break; } }
      if (!alive) {
        const char* name = "?";
        for (int j = 0; j < velha.n; ++j) { if (velha.t[j].numero == s_ant[k].numero) { name = velha.t[j].name; } }
        if (!efemera(name)) { evento("tarefa encerrada: %s", name); }
      }
    }
  }
  memcpy(s_ant, novo_ant, sizeof(anterior_t) * n);
  s_n_ant = (int)n;

  // ---- memory of minutes: fragmentation becomes a number ----------------------
  // frag % = 1 - (largest block / free): 0 = everything in one piece; the biggest,
  // more chopped up is the internal heap. 'blocks' = separate free pieces.
  {
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    pool_estado_t pe;
    pool_estado(&pe);
    const uint32_t livre = (uint32_t)hi.total_free_bytes;
    const float frag = livre ? 100.0f * (1.0f - (float)hi.largest_free_block / (float)livre) : 0.0f;
    if (livre < s_mm_ram_min)                      { s_mm_ram_min = livre; }
    if (hi.largest_free_block < s_mm_bloco_min)    { s_mm_bloco_min = (uint32_t)hi.largest_free_block; }
    if (nova.psram.livre < s_mm_psram_min)         { s_mm_psram_min = nova.psram.livre; }
    if (hi.free_blocks > s_mm_blocos_max)          { s_mm_blocos_max = (uint32_t)hi.free_blocks; }
    if (frag > s_mm_frag_max)                      { s_mm_frag_max = frag; }
    if ((uint32_t)pe.ocupadas > s_mm_ocupadas_max) { s_mm_ocupadas_max = (uint32_t)pe.ocupadas; }
    if (++s_mm_amostras >= 60) {
      char h[12], line[112];
      hora_text(h, sizeof(h));
      snprintf(line, sizeof(line), "%s,%lu,%lu,%lu,%lu,%.0f,%lu,%lu,%lu\n", h, (unsigned long)now,
               (unsigned long)(s_mm_ram_min / 1024), (unsigned long)(s_mm_bloco_min / 1024),
               (unsigned long)s_mm_blocos_max, (double)s_mm_frag_max,
               (unsigned long)(s_mm_psram_min / 1024), (unsigned long)s_mm_ocupadas_max,
               (unsigned long)pe.dinamicas);
      anexar("memoria",
             "hora,uptime_s,ram_livre_min_kb,maior_bloco_min_kb,blocos_livres_max,frag_max_pct,psram_livre_min_kb,vagas_ocupadas_max,vagas_dinamicas\n",
             line, 1, sizeof(line));
      s_mm_ram_min = s_mm_bloco_min = s_mm_psram_min = UINT32_MAX;
      s_mm_blocos_max = s_mm_ocupadas_max = 0; s_mm_frag_max = 0; s_mm_amostras = 0;
    }
  }

  // ---- frames of minute: the cost of interface ----------------------------
  // Only records minutes that HAD a frame: screen stopped, not spent writing on the card.
  // MB of copy = reading + writing in PSRAM (x4 per pixel); drawn = so
  // writing (x2); ms = average per frame. p1..p4mais = turns of interface that
  // ended in frame, however many periods of panel took - above
  // 1 the frame lost its turn.
  if (++s_qd_amostras >= 60) {
    s_qd_amostras = 0;
    comp_estat_t a;
    comp_estat(&a);
    const comp_estat_t& b = s_qd_ant;
    const uint32_t q = a.frames - b.frames;
    if (q > 0) {
      auto ms = [&](uint64_t us) { return (double)us / 1000.0 / q; };
      char h[12], line[200];
      hora_text(h, sizeof(h));
      snprintf(line, sizeof(line),
               "%s,%lu,%lu,%lu,%lu,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%lu,%lu,%lu,%lu\n",
               h, (unsigned long)now, (unsigned long)q,
               (unsigned long)(a.cheios - b.cheios), (unsigned long)(a.estouros - b.estouros),
               (double)(a.px_frame - b.px_frame) * 2.0 / 1e6,
               (double)(a.px_sinc - b.px_sinc) * 4.0 / 1e6,
               (double)(a.px_rolar_dma - b.px_rolar_dma) * 4.0 / 1e6,
               ms(a.us_drawing - b.us_drawing), ms(a.us_copia - b.us_copia),
               ms(a.us_espera - b.us_espera), ms(a.us_rolar - b.us_rolar),
               ms(a.us_iteration - b.us_iteration),
               (unsigned long)(a.voltas[0] - b.voltas[0]), (unsigned long)(a.voltas[1] - b.voltas[1]),
               (unsigned long)(a.voltas[2] - b.voltas[2]), (unsigned long)(a.voltas[3] - b.voltas[3]));
      // The format changed with direct mode: new file ("frames2"), to
      // do not mix columns with of dia.
      anexar("quadros2",
             "hora,uptime_s,quadros,cheios,estouros,mb_desenhado,mb_sinc,mb_rolagem_dma,"
             "ms_desenho,ms_copia,ms_espera,ms_rolagem,ms_volta,p1,p2,p3,p4mais\n",
             line, 1, sizeof(line));
    }
    s_qd_ant = a;
  }

  // ---- summary of minutes ---------------------------------------------------
  if (nova.cpu > s_min_cpu_max)          { s_min_cpu_max = nova.cpu; }
  if (nova.ram.livre < s_min_ram)        { s_min_ram = nova.ram.livre; }
  if (nova.psram.livre < s_min_psram)    { s_min_psram = nova.psram.livre; }
  if (nova.ram.maior_bloco < s_min_bloco){ s_min_bloco = nova.ram.maior_bloco; }
  if (++s_min_amostras >= 60) {
    char h[12], line[112];
    hora_text(h, sizeof(h));
    snprintf(line, sizeof(line), "%s,%lu,%.0f,%lu,%lu,%lu,%d\n", h, (unsigned long)now,
             (double)s_min_cpu_max, (unsigned long)(s_min_ram / 1024),
             (unsigned long)(s_min_psram / 1024), (unsigned long)(s_min_bloco / 1024), nova.n);
    anexar("resumo", "hora,uptime_s,cpu_max_pct,ram_min_kb,psram_min_kb,bloco_min_kb,tarefas\n",
           line, 1, sizeof(line));
    s_min_cpu_max = 0; s_min_ram = s_min_psram = s_min_bloco = UINT32_MAX; s_min_amostras = 0;
  }

  // Minimum stack per task: one line every 10 min, "name=livreB" separated
  // by ';'. The file is short (144 lines/day) and of to compare with the
  // declared size of each task.
  if (now - s_stack_gravada_em >= PILHAS_PERIODO && s_stack_n > 0) {
    s_stack_gravada_em = now;
    char h[12];
    hora_text(h, sizeof(h));
    int o = snprintf(s_stack_line, sizeof(s_stack_line), "%s,%lu,", h, (unsigned long)now);
    for (int k = 0; k < s_stack_n && o < (int)sizeof(s_stack_line) - (MON_NOME + 12); ++k) {
      o += snprintf(s_stack_line + o, sizeof(s_stack_line) - o, "%s%s=%lu",
                    k ? ";" : "", s_stack_name[k], (unsigned long)s_stack_min[k]);
    }
    snprintf(s_stack_line + o, sizeof(s_stack_line) - o, "\n");
    anexar("pilhas", "hora,uptime_s,tarefa=pilha_livre_min_bytes\n", s_stack_line, 1, sizeof(s_stack_line));
  }

  // Pending events are displayed once.
  if (s_n_pend > 0) {
    anexar("eventos", "hora,uptime_s,evento\n", &s_pend[0][0], s_n_pend, sizeof(s_pend[0]));
    s_n_pend = 0;
  }
}

static volatile bool s_reset_request = false;
static void reset_now(void);

static void vTaskMonitor(void*)
{
  amostrar(true);
  TickType_t marca = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&marca, pdMS_TO_TICKS(1000));
    if (s_reset_request) { s_reset_request = false; reset_now(); }
    // SCREEN OFF: one sample every 10 s. The photo of the tasks is a section
    // criticizes (~1 ms, a stack of each task read byte by byte) and no one looks
    // the Tasks in the Dark app; RAM/CPU alarms only arrive up to 10 s later.
    // Agreed, iteration at 1 s in hour (the CPU of each window comes from deltas, worth
    // for any range).
    static uint32_t s_pulou = 0;
    if (tab5_power_is_sleeping() && ++s_pulou < 10) { continue; }
    s_pulou = 0;
    amostrar(false);
  }
}

// -------------------------------------------------------------------------------
// API
// -------------------------------------------------------------------------------
void tab5_monitor_start(void)
{
  if (s_mutex) { return; }
#if ( configGENERATE_RUN_TIME_STATS != 1 )
  ESP_LOGE(TAG, "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS disabled - no per-task CPU data");
#endif
  s_mutex  = xSemaphoreCreateMutex();
  // In PSRAM: only the of monitor task reads this, and uxTaskGetSystemState
  // suspends the scheduler, does not turn off the cache.
  s_status = (TaskStatus_t*)heap_caps_malloc(MON_MAX_TAREFAS * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
  if (!s_mutex || !s_status) { ESP_LOGE(TAG, "no memory"); return; }
  {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(vTaskMonitor, "monitor", 6144, nullptr, 1, &h, tskNO_AFFINITY);
    tab5_owner_register(h, OWNER_SERVICE, "Monitor");   // see hal/tab5_owners.hpp
  }
}

bool tab5_monitor_foto(mon_foto_t* out)
{
  if (!s_mutex || !out) { return false; }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const bool ok = s_tem_foto;
  if (ok) { *out = s_foto; }
  xSemaphoreGive(s_mutex);
  return ok;
}

int tab5_monitor_historico(float* cpu, uint32_t* ram, uint32_t* psram, int max)
{
  if (!s_mutex) { return 0; }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const int n = (s_h_n < max) ? s_h_n : max;
  const int start = (s_h_i - n + MON_HIST_N) % MON_HIST_N;
  for (int k = 0; k < n; ++k) {
    const int i = (start + k) % MON_HIST_N;
    if (cpu)   { cpu[k]   = s_h_cpu[i]; }
    if (ram)   { ram[k]   = s_h_ram[i]; }
    if (psram) { psram[k] = s_h_psram[i]; }
  }
  xSemaphoreGive(s_mutex);
  return n;
}

int tab5_monitor_eventos(mon_evento_t* out, int max)
{
  if (!s_mutex) { return 0; }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const int n = (s_ev_n < max) ? s_ev_n : max;
  for (int k = 0; k < n; ++k) {
    out[k] = s_ev[(s_ev_i - 1 - k + MON_EVENTOS_N) % MON_EVENTOS_N];
  }
  xSemaphoreGive(s_mutex);
  return n;
}

uint32_t tab5_monitor_total_eventos(void) { return s_ev_total; }

// Call via INTERFACE: just ask. Who resets it is the task of monitor, in
// start of next sample - the state of the triggers and the queue of cards are
// of it, and messing with them would be another task.
void tab5_monitor_reset_picos(void) { s_reset_request = true; }

static void reset_now(void)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const uint32_t now = uptime_s();
  s_foto.cpu_pico = s_foto.cpu; s_foto.cpu_pico_em = now;
  for (int i = 0; i < s_foto.n; ++i) { s_foto.t[i].pct_pico = s_foto.t[i].pct; s_foto.t[i].pct_pico_em = now; }
  s_foto.ram.min_livre   = s_foto.ram.livre;         s_foto.ram.min_livre_em = now;
  s_foto.ram.min_bloco   = s_foto.ram.maior_bloco;   s_foto.ram.min_bloco_em = now;
  s_foto.psram.min_livre = s_foto.psram.livre;       s_foto.psram.min_livre_em = now;
  s_foto.psram.min_bloco = s_foto.psram.maior_bloco; s_foto.psram.min_bloco_em = now;
  xSemaphoreGive(s_mutex);
  s_stack_n = 0;
  pool_pilhas_reset();
  s_ram_recorde_avisado = UINT32_MAX;
  evento("picos zerados pelo usuario");
}
