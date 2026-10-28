#include "tab5_monitor.hpp"
#include "tab5_owners.hpp"
#include "tab5_marks.hpp"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes na PSRAM

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
#include "compositor.hpp"   // contador de underrun do painel
#include "app.h"            // sistema_app_atual

static const char* TAG = "monitor";
static const char* PASTA = "Tarefas";

// Foto do compositor no ultimo minuto gravado (quadros-*.csv).
static comp_estat_t s_qd_ant = {};
static uint32_t     s_qd_amostras = 0;

// ---------------------------------------------------------------------------
// OS GATILHOS - pequenos e baratos, todos por BORDA
// ---------------------------------------------------------------------------
// Cada um dispara UMA vez ao cruzar o limite e so rearma depois de voltar com
// folga (histerese) - uma memoria oscilando em volta do limite nao vira uma
// enxurrada de eventos.
static const uint32_t RAM_BAIXA      = 48 * 1024;    // RAM interna livre abaixo disto
static const uint32_t RAM_FOLGA      = 16 * 1024;
static const uint32_t BLOCO_BAIXO    = 16 * 1024;    // maior bloco interno abaixo disto (fragmentacao)
static const uint32_t BLOCO_FOLGA    =  8 * 1024;
static const uint32_t PSRAM_BAIXA    = 2 * 1024 * 1024;
static const uint32_t PSRAM_FOLGA    = 512 * 1024;
static const uint32_t NOVO_MIN_PASSO =  4 * 1024;    // novo recorde de RAM baixa, de 4KB em 4KB
static const uint32_t QUEDA_BRUSCA   = 32 * 1024;    // RAM interna caiu isto em 1 s
static const uint32_t PILHA_BAIXA    = 512;          // bytes que sobraram na stack de uma tarefa
static const float    CPU_TAREFA_ALTA = 60.0f, CPU_TAREFA_OK = 40.0f;
static const int      CPU_TAREFA_SEG  = 3;           // segundos seguidos acima
static const float    CPU_TOTAL_ALTA  = 90.0f, CPU_TOTAL_OK = 70.0f;
static const int      CPU_TOTAL_SEG   = 5;
static const uint32_t ACOMODAR_S      = 20;          // o boot aloca de tudo: recorde so depois disto

// Tarefas de vida curta do proprio kernel: nascer e sumir e o trabalho delas,
// nao noticia (uma por abertura de app).
static bool efemera(const char* n)
{
  return strcmp(n, "sd_monta") == 0 || strcmp(n, "sd_desmonta") == 0;
}

// ---------------------------------------------------------------------------
// ESTADO
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_mutex = nullptr;
EXT_RAM_BSS_ATTR static mon_foto_t s_foto;
static bool              s_tem_foto = false;

// Na PSRAM (3,6 KB): lido 1x/s, so pelo app Tarefas - a RAM interna e das pilhas e do DMA.
EXT_RAM_BSS_ATTR static float    s_h_cpu[MON_HIST_N];
EXT_RAM_BSS_ATTR static uint32_t s_h_ram[MON_HIST_N], s_h_psram[MON_HIST_N];
static int      s_h_n = 0, s_h_i = 0;

EXT_RAM_BSS_ATTR static mon_evento_t s_ev[MON_EVENTOS_N];
static int          s_ev_n = 0, s_ev_i = 0;
static uint32_t     s_ev_total = 0;

// Linhas esperando o cartao (gravadas uma vez por volta, nao a cada evento).
#define PENDENTES_N 16
EXT_RAM_BSS_ATTR static char s_pend[PENDENTES_N][112];   // idem (1,8 KB)
static int  s_n_pend = 0;

// Memoria da tarefa de amostragem (so ela toca).
typedef struct { UBaseType_t numero; uint64_t tempo; int seg_alta; bool alerta_cpu; bool alerta_pilha; } anterior_t;
static anterior_t    s_ant[MON_MAX_TAREFAS];
static int           s_n_ant = 0;
static TaskStatus_t* s_status = nullptr;

static bool     s_ram_alerta = false, s_bloco_alerta = false, s_psram_alerta = false, s_cpu_alerta = false;
static int      s_cpu_seg_alta = 0;
static uint32_t s_ram_recorde_avisado = UINT32_MAX;
static uint32_t s_ram_anterior = 0;
static uint32_t s_underruns_ant = 0;
static uint32_t s_atrasos_ant = 0;
static int64_t  s_foto_pior_us = 0;   // a pior duracao de uxTaskGetSystemState
static uint32_t s_dsi_erros_ant = 0;

// PILHA MINIMA POR NOME DE TAREFA, para dimensionar as pilhas com dado real.
// O high water mark de uma tarefa some quando ela morre (sd_monta, grove_sonda,
// tarefas de app), e o app Tarefas so mostra quem esta vivo AGORA. Esta tabela
// guarda o menor valor ja visto por nome e vai ao cartao a cada 10 min.
#define PILHAS_N        48
#define PILHAS_PERIODO  600
EXT_RAM_BSS_ATTR static char s_pilha_nome[PILHAS_N][MON_NOME];
static uint32_t s_pilha_min[PILHAS_N];
static int      s_pilha_n = 0;
static uint32_t s_pilha_gravada_em = 0;
EXT_RAM_BSS_ATTR static char s_pilha_linha[PILHAS_N * (MON_NOME + 10) + 32];

// Memoria do minuto (fragmentacao da RAM interna).
static uint32_t s_mm_ram_min = UINT32_MAX, s_mm_bloco_min = UINT32_MAX, s_mm_psram_min = UINT32_MAX;
static uint32_t s_mm_blocos_max = 0, s_mm_ocupadas_max = 0;
static float    s_mm_frag_max = 0;
static int      s_mm_amostras = 0;

// Resumo do minuto.
static float    s_min_cpu_max = 0;
static uint32_t s_min_ram = UINT32_MAX, s_min_psram = UINT32_MAX, s_min_bloco = UINT32_MAX;
static int      s_min_amostras = 0;

// ---- extrato da RAM interna (ver tab5_monitor.hpp) ----
// Sem trava: o indice e reservado com um incremento atomico, entao dois
// marcos simultaneos caem em vagas diferentes. Cheio, os marcos novos sao
// descartados - o que interessa e o boot e a primeira subida de cada radio.
static mem_marco_t s_marcos[MEM_MARCOS_N];
static uint32_t    s_marcos_n = 0;
static uint32_t    s_marco_livre_ant = 0;

void tab5_mem_marco(const char* etapa)
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
  mem_marco_t& m = s_marcos[i];
  m.ms          = tab5_agora_ms();
  m.livre       = livre;
  m.maior_bloco = (uint32_t)hi.largest_free_block;
  m.minimo      = (uint32_t)hi.minimum_free_bytes;
  m.delta       = delta;
  snprintf(m.etapa, sizeof(m.etapa), "%s", etapa);
}

int tab5_mem_extrato(mem_marco_t* out, int max)
{
  uint32_t n = __atomic_load_n(&s_marcos_n, __ATOMIC_RELAXED);
  if (n > MEM_MARCOS_N) { n = MEM_MARCOS_N; }
  if ((int)n > max)     { n = (uint32_t)max; }
  memcpy(out, s_marcos, n * sizeof(mem_marco_t));
  return (int)n;
}

static inline uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000LL); }

static uint32_t epoch_valido(void) { return tab5_hora_epoch(); }

static void nome_do_dia(const char* prefixo, char* out, size_t tam)
{
  char pre[24];
  snprintf(pre, sizeof(pre), "%s-", prefixo);
  tab5_hora_nome_do_dia(epoch_valido(), pre, ".csv", out, tam);
}

static void hora_texto(char* out, size_t tam)
{
  const uint32_t e = epoch_valido();
  if (!e) { snprintf(out, tam, "--:--:--"); return; }
  struct tm lt;
  tab5_hora_local(e, &lt);
  snprintf(out, tam, "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
}

// ---------------------------------------------------------------------------
// EVENTO: memoria + log + queue do cartao
// ---------------------------------------------------------------------------
static void evento(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void evento(const char* fmt, ...)
{
  mon_evento_t e;
  e.uptime_s = uptime_s();
  e.epoch    = epoch_valido();
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.texto, sizeof(e.texto), fmt, ap);
  va_end(ap);

  ESP_LOGW(TAG, "%s", e.texto);

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_ev[s_ev_i] = e;
  s_ev_i = (s_ev_i + 1) % MON_EVENTOS_N;
  if (s_ev_n < MON_EVENTOS_N) { ++s_ev_n; }
  ++s_ev_total;
  xSemaphoreGive(s_mutex);

  if (s_n_pend < PENDENTES_N) {
    char h[12];
    hora_texto(h, sizeof(h));
    snprintf(s_pend[s_n_pend++], sizeof(s_pend[0]), "%s,%lu,%s\n",
             h, (unsigned long)e.uptime_s, e.texto);
  }
}

// Pelo gravador do sistema (hal/tab5_sd.hpp) - entrega e segue.
static void anexar(const char* prefixo, const char* cabecalho, const char* linhas, int n, size_t passo)
{
  if (n <= 0) { return; }
  char nome[40];
  nome_do_dia(prefixo, nome, sizeof(nome));
  for (int i = 0; i < n; ++i) { tab5_sd_enqueue_append(PASTA, nome, linhas + i * passo, cabecalho); }
}

// ---------------------------------------------------------------------------
// A AMOSTRA DE UM SEGUNDO
// ---------------------------------------------------------------------------
static anterior_t* anterior_de(UBaseType_t numero)
{
  for (int i = 0; i < s_n_ant; ++i) { if (s_ant[i].numero == numero) { return &s_ant[i]; } }
  return nullptr;
}

static void ler_mem(mon_mem_t* m, const mon_mem_t* velho, uint32_t caps, uint32_t agora)
{
  m->livre       = (uint32_t)heap_caps_get_free_size(caps);
  m->total       = (uint32_t)heap_caps_get_total_size(caps);
  m->maior_bloco = (uint32_t)heap_caps_get_largest_free_block(caps);
  m->min_livre    = velho->min_livre;    m->min_livre_em = velho->min_livre_em;
  m->min_bloco    = velho->min_bloco;    m->min_bloco_em = velho->min_bloco_em;
  if (m->livre < m->min_livre)       { m->min_livre = m->livre;       m->min_livre_em = agora; }
  if (m->maior_bloco < m->min_bloco) { m->min_bloco = m->maior_bloco; m->min_bloco_em = agora; }
}

static void anotar_pilha(const char* nome, uint32_t livre)
{
  int k = 0;
  while (k < s_pilha_n && strncmp(s_pilha_nome[k], nome, MON_NOME) != 0) { ++k; }
  if (k == s_pilha_n) {
    if (s_pilha_n >= PILHAS_N) { return; }
    snprintf(s_pilha_nome[k], MON_NOME, "%s", nome);
    s_pilha_min[k] = livre;
    ++s_pilha_n;
  } else if (livre < s_pilha_min[k]) {
    s_pilha_min[k] = livre;
  }
}

// Vaga do pool com uma execucao dentro: o nome util e o do DONO ("Mesh"), nao
// o da vaga ("pool2").
static const char* nome_efetivo(const TaskStatus_t& st)
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
  const uint32_t agora = uptime_s();
  // uxTaskGetSystemState roda INTEIRA numa secao critica (no SMP do IDF:
  // interrupcoes desligadas neste core), medindo a stack livre de cada
  // tarefa la dentro - ~0,8 ms com a CPU a 360 MHz. E mais que a folga de
  // ~0,5 ms que o DMA do painel tem entre frames: caindo nela, perde um
  // frame inteiro. ERA A CAUSA do "FRAME ATRASADO" com os dois cores em
  // IDLE - este loop roda em fase fixa de 1 s, e todos os
  // atrasos tinham a mesma fracao de segundo. Por isso a foto so e tirada
  // logo DEPOIS de um fim de frame (tab5_frame_wait_for_end).
  // EM REPOUSO O PAINEL ESTA DESMONTADO: nao ha fim de quadro para esperar
  // (nem quadro para perder). A espera so vencia pelo prazo - 40 despertares
  // de 1 tick, todo segundo, com a tela apagada.
  if (!tab5_power_is_sleeping()) { tab5_frame_wait_for_end(40); }
  const int64_t t_foto = esp_timer_get_time();
  const UBaseType_t n = uxTaskGetSystemState(s_status, MON_MAX_TAREFAS, nullptr);
  const int64_t us_foto = esp_timer_get_time() - t_foto;
  tab5_mark("monitor: foto das tarefas us", (int32_t)us_foto);
  if (us_foto > s_foto_pior_us) {
    s_foto_pior_us = us_foto;
    // Acima de 5 ms nem a janela entre frames cobre (~14 ms, mas o limite
    // de verdade depende de onde a espera acordou) - vale saber.
    if (us_foto > 5000 && !tab5_power_is_sleeping()) {
      evento("foto das tarefas segurou o nucleo %d por %lld us", (int)xPortGetCoreID(), (long long)us_foto);
    }
  }
  if (n == 0) {
    // Mais tarefas que vagas: o FreeRTOS nao preenche nada. Avisa uma vez, em
    // vez de a foto parar em silencio.
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
    const anterior_t* a = anterior_de(s_status[i].xTaskNumber);
    const uint64_t atual = (uint64_t)s_status[i].ulRunTimeCounter;
    const uint64_t antes = a ? a->tempo : atual;   // tarefa nova: comeca do zero
    deltas[i] = (atual >= antes) ? (atual - antes) : atual;
    total += deltas[i];
  }

  // Estaticas: ~1,7KB cada, e so esta tarefa as usa - na stack, junto do
  // fprintf do cartao, apertariam os 6KB dela.
  EXT_RAM_BSS_ATTR static mon_foto_t nova, velha;
  memset(&nova, 0, sizeof(nova));
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  velha = s_foto;
  xSemaphoreGive(s_mutex);

  nova.uptime_s = agora;
  nova.n = (int)n;
  nova.foto_us      = (uint32_t)us_foto;
  nova.foto_pior_us = (uint32_t)s_foto_pior_us;
  float idle = 0;
  for (UBaseType_t i = 0; i < n; ++i) {
    mon_tarefa_t& t = nova.t[i];
    snprintf(t.name, sizeof(t.name), "%s", nome_efetivo(s_status[i]));
    t.numero      = s_status[i].xTaskNumber;
    t.prioridade  = s_status[i].uxCurrentPriority;
    t.pilha_livre = (uint32_t)s_status[i].usStackHighWaterMark * sizeof(StackType_t);
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

    // O pico desta tarefa vem da foto anterior (se ela ja existia).
    t.pct_pico = t.pct; t.pct_pico_em = agora;
    for (int k = 0; k < velha.n; ++k) {
      if (velha.t[k].numero == t.numero && velha.t[k].pct_pico > t.pct) {
        t.pct_pico = velha.t[k].pct_pico; t.pct_pico_em = velha.t[k].pct_pico_em;
      }
    }
  }
  // Ordem estavel na tela: por xTaskNumber (ordem de criacao).
  for (int i = 1; i < nova.n; ++i) {
    mon_tarefa_t c = nova.t[i]; int j = i - 1;
    while (j >= 0 && nova.t[j].numero > c.numero) { nova.t[j + 1] = nova.t[j]; --j; }
    nova.t[j + 1] = c;
  }

  // Pilha minima por nome (so esta tarefa toca na tabela). Vagas do pool
  // entram pelo nome da EXECUCAO (mais abaixo); aqui ficam so as tarefas comuns
  // e as vagas com dono conhecido.
  for (int i = 0; i < nova.n; ++i) {
    const mon_tarefa_t& t = nova.t[i];
    if (strncmp(t.name, "IDLE", 4) == 0 || strncmp(t.name, "pool", 4) == 0) { continue; }
    anotar_pilha(t.name, t.pilha_livre);
  }
  {
    pool_pilha_t pp[24];
    const int np = pool_pilhas(pp, 24);
    for (int i = 0; i < np; ++i) { anotar_pilha(pp[i].name, pp[i].livre_min); }
  }

  nova.cpu = 100.0f - idle;
  if (nova.cpu < 0) { nova.cpu = 0; }
  if (nova.cpu > 100) { nova.cpu = 100; }
  nova.cpu_pico = velha.cpu_pico; nova.cpu_pico_em = velha.cpu_pico_em;
  if (primeira || nova.cpu > nova.cpu_pico) { nova.cpu_pico = nova.cpu; nova.cpu_pico_em = agora; }

  if (primeira) {
    nova.ram.min_livre = nova.ram.min_bloco = UINT32_MAX;
    nova.psram.min_livre = nova.psram.min_bloco = UINT32_MAX;
    ler_mem(&nova.ram,   &nova.ram,   MALLOC_CAP_INTERNAL, agora);
    ler_mem(&nova.psram, &nova.psram, MALLOC_CAP_SPIRAM,   agora);
  } else {
    ler_mem(&nova.ram,   &velha.ram,   MALLOC_CAP_INTERNAL, agora);
    ler_mem(&nova.psram, &velha.psram, MALLOC_CAP_SPIRAM,   agora);
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_foto = nova;
  s_tem_foto = true;
  s_h_cpu[s_h_i] = nova.cpu; s_h_ram[s_h_i] = nova.ram.livre; s_h_psram[s_h_i] = nova.psram.livre;
  s_h_i = (s_h_i + 1) % MON_HIST_N;
  if (s_h_n < MON_HIST_N) { ++s_h_n; }
  xSemaphoreGive(s_mutex);

  // ---- gatilhos -----------------------------------------------------------
  if (!primeira) {
    const mon_mem_t& r = nova.ram;
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
    if (agora >= ACOMODAR_S && r.livre + NOVO_MIN_PASSO <= s_ram_recorde_avisado) {
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

  // O FRAME AZUL VIRA EVENTO. O vigia de underrun
  // (hal/tab5_frame) conta cada vez que o painel ficou sem dados; aqui cada
  // aumento vira evento com o APP QUE ESTAVA ABERTO - a piscada azul deixa de
  // ser relato e passa a ter hora e lugar.
  const uint32_t und = tab5_frame_underruns();
  if (!primeira && und > s_underruns_ant) {
    const char* app = sistema_app_atual();
    evento("UNDERRUN do painel (+%lu) com %s aberto", (unsigned long)(und - s_underruns_ant),
           app ? app : "a home");
  }
  s_underruns_ant = und;

  // A INTERFACE PAROU? (tela travada, sem toque, sem nada no log). O loop dela conta voltas e deixa o nome do passo em que esta
  // (kernel/core.cpp, ETAPA). Parado ha mais de 3 s: evento com o passo - uma
  // vez so, e outro quando voltar. O watchdog de tarefas pega o loop
  // BLOQUEADO; isto pega tambem o loop que parou de dar volta por outro motivo.
  {
    static bool     s_presa = false;
    static uint32_t s_presa_desde = 0;
    sistema_diag_t d;
    sistema_diag_interface(&d);
    if (!s_presa && d.voltas > 0 && d.ms_desde_volta > 3000) {
      s_presa = true;
      s_presa_desde = d.voltas;
      evento("INTERFACE PRESA ha %lus em: %s", (unsigned long)(d.ms_desde_volta / 1000), d.etapa);
      evento("  com %s%s; dedo lido ha %lds", d.app ? d.app : "a home",
             d.repouso ? " (repouso)" : "",
             d.ms_desde_dedo == UINT32_MAX ? -1L : (long)(d.ms_desde_dedo / 1000));
    } else if (s_presa && d.voltas != s_presa_desde) {
      s_presa = false;
      evento("interface voltou (parada em: %s)", d.etapa);
    }
  }

  // Os detectores que nao dependem do log do driver (ver hal/tab5_frame):
  // frame que atrasou (DMA religado tarde) e erro no DSI host.
  {
    uint32_t pior_us = 0, periodo_us = 0;
    const uint32_t atr = tab5_frame_delays(&pior_us, &periodo_us);
    if (!primeira && atr > s_atrasos_ant) {
      const char* app = sistema_app_atual();
      evento("QUADRO ATRASADO (+%lu) pior %lu.%lums (normal %lu.%lums) com %s",
             (unsigned long)(atr - s_atrasos_ant),
             (unsigned long)(pior_us / 1000), (unsigned long)(pior_us % 1000 / 100),
             (unsigned long)(periodo_us / 1000), (unsigned long)(periodo_us % 1000 / 100),
             app ? app : "a home");
      int nucleo = -1; char t0[16], t1[16];
      tab5_frame_delay_source(&nucleo, t0, t1);
      evento("  atraso: ISR no nucleo %d; rodando c0=%s c1=%s", nucleo, t0, t1);
      // O QUE ACONTECEU PERTO: quanto tempo antes do atraso o cartao montou/
      // desmontou de verdade e saiu o ultimo frame (e de que tamanho).
      const int64_t t = tab5_frame_delay_time();
      int64_t mont = 0, desm = 0, qd = 0; uint32_t px = 0;
      tab5_sd_last_transitions(&mont, &desm);
      comp_ultimo_quadro(&qd, &px);
      auto ms = [&](int64_t x) { return x ? (long)((t - x) / 1000) : -1L; };
      evento("  em %ld.%03lds: sd +%ldms -%ldms, quadro %ldms antes (%luKpx)",
             (long)(t / 1000000), (long)(t / 1000 % 1000), ms(mont), ms(desm), ms(qd),
             (unsigned long)(px / 1000));
      // OS MARCOS DOS 300 ms ANTERIORES (hal/tab5_marks): trocas de clock da
      // CPU (e com elas o reajuste do barramento da PSRAM), repouso, sondagem
      // do Grove... Direto no log, e nao em evento(), porque a linha e longa.
      tab5_mark_t m[12];
      const int nm = tab5_marks_between(t - 300000, t + 2000, m, 12);
      if (nm == 0) {
        ESP_LOGW(TAG, "  marcos: nenhum nos 300 ms antes");
      } else {
        char linha[400];
        int k = snprintf(linha, sizeof(linha), "  marcos:");
        for (int i = 0; i < nm && k < (int)sizeof(linha) - 48; ++i) {
          k += snprintf(linha + k, sizeof(linha) - k, " [%+ldms %s %ld]",
                        (long)((m[i].us - t) / 1000), m[i].what, (long)m[i].value);
        }
        ESP_LOGW(TAG, "%s", linha);
      }
    }
    s_atrasos_ant = atr;

    uint32_t st0 = 0, st1 = 0;
    const uint32_t err = tab5_frame_dsi_errors(&st0, &st1);
    if (!primeira && err > s_dsi_erros_ant) {
      const char* app = sistema_app_atual();
      evento("DSI host: erro st0=%08lx st1=%08lx (+%lu quadros) com %s",
             (unsigned long)st0, (unsigned long)st1, (unsigned long)(err - s_dsi_erros_ant),
             app ? app : "a home");
    }
    s_dsi_erros_ant = err;
  }

  // Por tarefa: CPU presa em alta, stack no fim, nascimento. O estado anterior
  // de cada uma e reconstruido aqui - as que sumiram ficam de fora.
  static anterior_t novo_ant[MON_MAX_TAREFAS];
  for (UBaseType_t i = 0; i < n; ++i) {
    const char* nome = nome_efetivo(s_status[i]);
    const anterior_t* a = anterior_de(s_status[i].xTaskNumber);
    anterior_t& b = novo_ant[i];
    b.numero       = s_status[i].xTaskNumber;
    b.tempo        = (uint64_t)s_status[i].ulRunTimeCounter;
    b.seg_alta     = a ? a->seg_alta : 0;
    b.alerta_cpu   = a ? a->alerta_cpu : false;
    b.alerta_pilha = a ? a->alerta_pilha : false;

    if (!a && !primeira && !efemera(nome)) { evento("tarefa nova: %s", nome); }

    const float pct = total ? (100.0f * (float)deltas[i] / (float)total) : 0.0f;
    if (strncmp(nome, "IDLE", 4) != 0) {
      if (pct > CPU_TAREFA_ALTA) {
        if (++b.seg_alta >= CPU_TAREFA_SEG && !b.alerta_cpu) {
          b.alerta_cpu = true;
          evento("%s em %.0f%% de CPU ha %ds", nome, (double)pct, b.seg_alta);
        }
      } else {
        b.seg_alta = 0;
        if (pct < CPU_TAREFA_OK) { b.alerta_cpu = false; }
      }
    }
    const uint32_t pilha = (uint32_t)s_status[i].usStackHighWaterMark * sizeof(StackType_t);
    if (!b.alerta_pilha && pilha < PILHA_BAIXA) {
      b.alerta_pilha = true;
      evento("pilha de %s quase no fim: %luB livres", nome, (unsigned long)pilha);
    }
  }
  if (!primeira) {
    for (int k = 0; k < s_n_ant; ++k) {
      bool viva = false;
      for (UBaseType_t i = 0; i < n; ++i) { if (novo_ant[i].numero == s_ant[k].numero) { viva = true; break; } }
      if (!viva) {
        const char* nome = "?";
        for (int j = 0; j < velha.n; ++j) { if (velha.t[j].numero == s_ant[k].numero) { nome = velha.t[j].name; } }
        if (!efemera(nome)) { evento("tarefa encerrada: %s", nome); }
      }
    }
  }
  memcpy(s_ant, novo_ant, sizeof(anterior_t) * n);
  s_n_ant = (int)n;

  // ---- memoria do minuto: a fragmentacao vira numero ----------------------
  // frag % = 1 - (maior bloco / livre): 0 = tudo num pedaco so; quanto maior,
  // mais picado esta o heap interno. 'blocos' = pedacos livres separados.
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
      char h[12], linha[112];
      hora_texto(h, sizeof(h));
      snprintf(linha, sizeof(linha), "%s,%lu,%lu,%lu,%lu,%.0f,%lu,%lu,%lu\n", h, (unsigned long)agora,
               (unsigned long)(s_mm_ram_min / 1024), (unsigned long)(s_mm_bloco_min / 1024),
               (unsigned long)s_mm_blocos_max, (double)s_mm_frag_max,
               (unsigned long)(s_mm_psram_min / 1024), (unsigned long)s_mm_ocupadas_max,
               (unsigned long)pe.dinamicas);
      anexar("memoria",
             "hora,uptime_s,ram_livre_min_kb,maior_bloco_min_kb,blocos_livres_max,frag_max_pct,psram_livre_min_kb,vagas_ocupadas_max,vagas_dinamicas\n",
             linha, 1, sizeof(linha));
      s_mm_ram_min = s_mm_bloco_min = s_mm_psram_min = UINT32_MAX;
      s_mm_blocos_max = s_mm_ocupadas_max = 0; s_mm_frag_max = 0; s_mm_amostras = 0;
    }
  }

  // ---- quadros do minuto: o custo da interface ----------------------------
  // So grava minuto que TEVE quadro: tela parada nao gasta escrita no cartao.
  // MB de copia = leitura + escrita na PSRAM (x4 por pixel); desenhado = so
  // escrita (x2); ms = media por quadro. p1..p4mais = voltas da interface que
  // terminaram em quadro, por quantos periodos do painel levaram - acima de
  // 1 o quadro perdeu a vez.
  if (++s_qd_amostras >= 60) {
    s_qd_amostras = 0;
    comp_estat_t a;
    comp_estat(&a);
    const comp_estat_t& b = s_qd_ant;
    const uint32_t q = a.frames - b.frames;
    if (q > 0) {
      auto ms = [&](uint64_t us) { return (double)us / 1000.0 / q; };
      char h[12], linha[200];
      hora_texto(h, sizeof(h));
      snprintf(linha, sizeof(linha),
               "%s,%lu,%lu,%lu,%lu,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%lu,%lu,%lu,%lu\n",
               h, (unsigned long)agora, (unsigned long)q,
               (unsigned long)(a.cheios - b.cheios), (unsigned long)(a.estouros - b.estouros),
               (double)(a.px_quadro - b.px_quadro) * 2.0 / 1e6,
               (double)(a.px_sinc - b.px_sinc) * 4.0 / 1e6,
               (double)(a.px_rolar_dma - b.px_rolar_dma) * 4.0 / 1e6,
               ms(a.us_desenho - b.us_desenho), ms(a.us_copia - b.us_copia),
               ms(a.us_espera - b.us_espera), ms(a.us_rolar - b.us_rolar),
               ms(a.us_volta - b.us_volta),
               (unsigned long)(a.voltas[0] - b.voltas[0]), (unsigned long)(a.voltas[1] - b.voltas[1]),
               (unsigned long)(a.voltas[2] - b.voltas[2]), (unsigned long)(a.voltas[3] - b.voltas[3]));
      // O formato mudou com o modo direto: arquivo novo ("quadros2"), para
      // nao misturar colunas com o do dia.
      anexar("quadros2",
             "hora,uptime_s,quadros,cheios,estouros,mb_desenhado,mb_sinc,mb_rolagem_dma,"
             "ms_desenho,ms_copia,ms_espera,ms_rolagem,ms_volta,p1,p2,p3,p4mais\n",
             linha, 1, sizeof(linha));
    }
    s_qd_ant = a;
  }

  // ---- resumo do minuto ---------------------------------------------------
  if (nova.cpu > s_min_cpu_max)          { s_min_cpu_max = nova.cpu; }
  if (nova.ram.livre < s_min_ram)        { s_min_ram = nova.ram.livre; }
  if (nova.psram.livre < s_min_psram)    { s_min_psram = nova.psram.livre; }
  if (nova.ram.maior_bloco < s_min_bloco){ s_min_bloco = nova.ram.maior_bloco; }
  if (++s_min_amostras >= 60) {
    char h[12], linha[112];
    hora_texto(h, sizeof(h));
    snprintf(linha, sizeof(linha), "%s,%lu,%.0f,%lu,%lu,%lu,%d\n", h, (unsigned long)agora,
             (double)s_min_cpu_max, (unsigned long)(s_min_ram / 1024),
             (unsigned long)(s_min_psram / 1024), (unsigned long)(s_min_bloco / 1024), nova.n);
    anexar("resumo", "hora,uptime_s,cpu_max_pct,ram_min_kb,psram_min_kb,bloco_min_kb,tarefas\n",
           linha, 1, sizeof(linha));
    s_min_cpu_max = 0; s_min_ram = s_min_psram = s_min_bloco = UINT32_MAX; s_min_amostras = 0;
  }

  // Pilha minima por tarefa: uma linha a cada 10 min, "nome=livreB" separados
  // por ';'. O arquivo e curto (144 linhas/dia) e da para comparar com o
  // tamanho declarado de cada tarefa.
  if (agora - s_pilha_gravada_em >= PILHAS_PERIODO && s_pilha_n > 0) {
    s_pilha_gravada_em = agora;
    char h[12];
    hora_texto(h, sizeof(h));
    int o = snprintf(s_pilha_linha, sizeof(s_pilha_linha), "%s,%lu,", h, (unsigned long)agora);
    for (int k = 0; k < s_pilha_n && o < (int)sizeof(s_pilha_linha) - (MON_NOME + 12); ++k) {
      o += snprintf(s_pilha_linha + o, sizeof(s_pilha_linha) - o, "%s%s=%lu",
                    k ? ";" : "", s_pilha_nome[k], (unsigned long)s_pilha_min[k]);
    }
    snprintf(s_pilha_linha + o, sizeof(s_pilha_linha) - o, "\n");
    anexar("pilhas", "hora,uptime_s,tarefa=pilha_livre_min_bytes\n", s_pilha_linha, 1, sizeof(s_pilha_linha));
  }

  // Eventos pendentes vao ao cartao de uma vez.
  if (s_n_pend > 0) {
    anexar("eventos", "hora,uptime_s,evento\n", &s_pend[0][0], s_n_pend, sizeof(s_pend[0]));
    s_n_pend = 0;
  }
}

static volatile bool s_zerar_pedido = false;
static void zerar_agora(void);

static void vTaskMonitor(void*)
{
  amostrar(true);
  TickType_t marca = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&marca, pdMS_TO_TICKS(1000));
    if (s_zerar_pedido) { s_zerar_pedido = false; zerar_agora(); }
    // TELA APAGADA: uma amostra a cada 10 s. A foto das tarefas e uma secao
    // critica (~1 ms, a pilha de cada tarefa lida byte a byte) e ninguem olha
    // o app Tarefas no escuro; os alarmes de RAM/CPU so chegam ate 10 s depois.
    // Acordou, volta a 1 s na hora (a CPU de cada janela vem de deltas, vale
    // para qualquer intervalo).
    static uint32_t s_pulou = 0;
    if (tab5_power_is_sleeping() && ++s_pulou < 10) { continue; }
    s_pulou = 0;
    amostrar(false);
  }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void tab5_monitor_iniciar(void)
{
  if (s_mutex) { return; }
#if ( configGENERATE_RUN_TIME_STATS != 1 )
  ESP_LOGE(TAG, "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS desligado - sem CPU por tarefa");
#endif
  s_mutex  = xSemaphoreCreateMutex();
  // Na PSRAM: so a tarefa do monitor le isto, e o uxTaskGetSystemState
  // suspende o escalonador, nao desliga o cache.
  s_status = (TaskStatus_t*)heap_caps_malloc(MON_MAX_TAREFAS * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
  if (!s_mutex || !s_status) { ESP_LOGE(TAG, "sem memoria"); return; }
  {
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(vTaskMonitor, "monitor", 6144, nullptr, 1, &h, tskNO_AFFINITY);
    tab5_owner_register(h, OWNER_SERVICE, "Monitor");   // ver hal/tab5_owners.hpp
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
  const int inicio = (s_h_i - n + MON_HIST_N) % MON_HIST_N;
  for (int k = 0; k < n; ++k) {
    const int i = (inicio + k) % MON_HIST_N;
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

// Chamada pela INTERFACE: so pede. Quem zera e a tarefa do monitor, no
// comeco da proxima amostra - o estado dos gatilhos e a queue do cartao sao
// dela, e mexer neles de outra tarefa seria corrida.
void tab5_monitor_zerar_picos(void) { s_zerar_pedido = true; }

static void zerar_agora(void)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const uint32_t agora = uptime_s();
  s_foto.cpu_pico = s_foto.cpu; s_foto.cpu_pico_em = agora;
  for (int i = 0; i < s_foto.n; ++i) { s_foto.t[i].pct_pico = s_foto.t[i].pct; s_foto.t[i].pct_pico_em = agora; }
  s_foto.ram.min_livre   = s_foto.ram.livre;         s_foto.ram.min_livre_em = agora;
  s_foto.ram.min_bloco   = s_foto.ram.maior_bloco;   s_foto.ram.min_bloco_em = agora;
  s_foto.psram.min_livre = s_foto.psram.livre;       s_foto.psram.min_livre_em = agora;
  s_foto.psram.min_bloco = s_foto.psram.maior_bloco; s_foto.psram.min_bloco_em = agora;
  xSemaphoreGive(s_mutex);
  s_pilha_n = 0;
  pool_pilhas_zerar();
  s_ram_recorde_avisado = UINT32_MAX;
  evento("picos zerados pelo usuario");
}
