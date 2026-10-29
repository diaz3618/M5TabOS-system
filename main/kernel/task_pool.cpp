#include "task_pool.h"

#include <atomic>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"   // pxTaskGetStackStart
#include "esp_log.h"

static const char* TAG = "pool";

// VACANCY CLASSES. Actual consumption measured (smallest free stack in actual use)
// send it here: serial services and apps fit in 4 KB; Mesh asks for 6 KB.
// The total is ~26 KB and replaces what these same uses requested on heap,
// in the worst case, as separate blocks.
#define VAGA_G  6144
#define VAGA_M  4096

#define PILHA_ESTATICA(name, bytes) alignas(16) static StackType_t name[(bytes) / sizeof(StackType_t)]
PILHA_ESTATICA(s_pilha0, VAGA_G);
PILHA_ESTATICA(s_pilha1, VAGA_M);
PILHA_ESTATICA(s_pilha2, VAGA_M);
PILHA_ESTATICA(s_pilha3, VAGA_M);
PILHA_ESTATICA(s_pilha4, VAGA_M);
PILHA_ESTATICA(s_pilha5, VAGA_M);

#define POOL_N 6

enum { LIVRE = 0, OCUPADA = 1 };

typedef struct {
  StaticTask_t      tcb;
  StaticSemaphore_t sem_buf;
  StackType_t*      stack;
  uint32_t          bytes;
  TaskHandle_t      h;
  SemaphoreHandle_t wake;
  std::atomic<int>  state;
  volatile uint32_t geracao;
  pool_body_t      fn;
  void*             arg;
  char              name[16];
} vaga_t;

static vaga_t s_v[POOL_N];
static bool   s_pronto = false;
static std::atomic<uint32_t> s_dinamicas{0};

// Smallest free stack per name of execution.
#define PILHAS_N 24
static pool_stack_t s_pilhas[PILHAS_N];
static int          s_pilhas_n = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void anotar_stack(const char* name, uint32_t livre)
{
  taskENTER_CRITICAL(&s_mux);
  int k = 0;
  while (k < s_pilhas_n && strncmp(s_pilhas[k].name, name, sizeof(s_pilhas[k].name)) != 0) { ++k; }
  if (k < PILHAS_N) {
    if (k == s_pilhas_n) {
      snprintf(s_pilhas[k].name, sizeof(s_pilhas[k].name), "%s", name);
      s_pilhas[k].livre_min = livre;
      ++s_pilhas_n;
    } else if (livre < s_pilhas[k].livre_min) {
      s_pilhas[k].livre_min = livre;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
}

// Repaint the FREE part of stack with the default 0xA5, so that the high water mark
// of next mecca execution only hers.
//
// WHERE THE STACK STARTS: NOT the start of buffer (v->stack). In P4 the IDF keeps
// in the pe of buffer the area of context of the coprocessors (FPU and PIE) of task,
// and only after that does the stack that FreeRTOS knows (pxStack) begin. Us
// first 32 bytes of pxStack, aligned to 32, stays watchman of end of stack
// (FREERTOS_WATCHPOINT_END_OF_STACK): any writing there brings down the system
// with "Breakpoint" - of purpose, and what causes stack overflow in time.
// Repainting from "buffer + 128" erased the context of FPU/PIE and crashed the
// watchman (Guru Meditation on close a Camera). That's why the start comes from itself
// FreeRTOS, and the lookout and the canary are easily skipped.
static void __attribute__((noinline)) repintar(void)
{
  const uintptr_t ini_stack = (uintptr_t)pxTaskGetStackStart(nullptr);
  uint8_t* base = (uint8_t*)(((ini_stack + 31u) & ~(uintptr_t)31u) + 64u);   // lookout (32 B) + canary, with clearance
  uint8_t* topo = (uint8_t*)__builtin_frame_address(0) - 512;
  if (topo > base) { memset(base, 0xA5, (size_t)(topo - base)); }
}

static void vTaskVaga(void* a)
{
  vaga_t* v = (vaga_t*)a;
  for (;;) {
    // xTaskAbortDelay can wake here without work: iteration sleeping.
    if (xSemaphoreTake(v->wake, portMAX_DELAY) != pdTRUE) { continue; }
    pool_body_t fn = v->fn;
    if (!fn) { continue; }
    ulTaskNotifyTake(pdTRUE, 0);   // warning left over from previous execution

    fn(v->arg);

    tab5_owner_forget(v->h);
    anotar_stack(v->name, (uint32_t)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
    ulTaskNotifyTake(pdTRUE, 0);
    repintar();
    v->fn = nullptr;
    v->state.store(LIVRE, std::memory_order_release);
  }
}

void pool_start(void)
{
  if (s_pronto) { return; }
  StackType_t* pilhas[POOL_N] = { s_pilha0, s_pilha1, s_pilha2, s_pilha3, s_pilha4, s_pilha5 };
  const uint32_t size[POOL_N]  = { VAGA_G, VAGA_M, VAGA_M, VAGA_M, VAGA_M, VAGA_M };
  uint32_t fixos = 0;
  for (int i = 0; i < POOL_N; ++i) {
    vaga_t* v = &s_v[i];
    v->stack = pilhas[i];
    v->bytes = size[i];
    v->state.store(LIVRE);
    v->wake = xSemaphoreCreateBinaryStatic(&v->sem_buf);
    char name[8];
    snprintf(name, sizeof(name), "pool%d", i);
    v->h = xTaskCreateStaticPinnedToCore(vTaskVaga, name, v->bytes, v, 1,
                                         v->stack, &v->tcb, tskNO_AFFINITY);
    fixos += v->bytes + (uint32_t)sizeof(StaticTask_t) + (uint32_t)sizeof(StaticSemaphore_t);
  }
  s_pronto = true;
  ESP_LOGI(TAG, "%d tarefas fixas, %lu B reservados (pilhas e TCBs estaticos)",
           POOL_N, (unsigned long)fixos);
}

// Execution in a common task, when there is no vacancy.
typedef struct { pool_body_t fn; void* arg; } dinamica_t;
static void vTaskDinamica(void* a)
{
  dinamica_t d = *(dinamica_t*)a;
  free(a);
  d.fn(d.arg);
  tab5_owner_forget(xTaskGetCurrentTaskHandle());
  vTaskDelete(nullptr);
}

bool pool_create(pool_id_t* id, pool_body_t fn, void* arg, const char* name,
                uint32_t stack, UBaseType_t prio, owner_type_t tipo, const char* dono)
{
  if (id) { *id = (pool_id_t)POOL_ID_EMPTY; }
  if (!fn) { return false; }
  if (!s_pronto) { pool_start(); }

  // The smallest free space that fits: Mesh does not take up to 6 KB when it fits into 4.
  // As of 4 KB are as of index 1..N-1; a of 6 KB and a 0 and stays last.
  vaga_t* esc = nullptr;
  for (int k = 1; k <= POOL_N && !esc; ++k) {
    vaga_t* v = &s_v[k % POOL_N];
    if (v->bytes < stack) { continue; }
    int esperado = LIVRE;
    if (v->state.compare_exchange_strong(esperado, OCUPADA, std::memory_order_acq_rel)) { esc = v; }
  }

  if (esc) {
    snprintf(esc->name, sizeof(esc->name), "%s", name ? name : "?");
    esc->fn  = fn;
    esc->arg = arg;
    esc->geracao = esc->geracao + 1;
    if (dono) { tab5_owner_register(esc->h, tipo, dono); }
    if (id) { id->idx = (int8_t)(esc - s_v); id->geracao = esc->geracao; id->handle = esc->h; }
    vTaskPrioritySet(esc->h, prio);
    xSemaphoreGive(esc->wake);
    return true;
  }

  s_dinamicas.fetch_add(1);
  ESP_LOGW(TAG, "sem vaga para '%s' (%lu B) - tarefa comum", name ? name : "?", (unsigned long)stack);
  dinamica_t* d = (dinamica_t*)malloc(sizeof(dinamica_t));
  if (!d) { return false; }
  d->fn = fn; d->arg = arg;
  TaskHandle_t h = nullptr;
  if (xTaskCreatePinnedToCore(vTaskDinamica, name ? name : "tarefa", stack, d, prio, &h,
                              tskNO_AFFINITY) != pdPASS) {
    free(d);
    return false;
  }
  if (dono) { tab5_owner_register(h, tipo, dono); }
  if (id) { id->handle = h; }
  return true;
}

void pool_wake(const pool_id_t* id)
{
  if (!id) { return; }
  if (id->idx >= 0 && id->idx < POOL_N) {
    vaga_t* v = &s_v[id->idx];
    if (v->geracao == id->geracao && v->state.load(std::memory_order_acquire) == OCUPADA) {
      xTaskAbortDelay(v->h);
    }
  } else if (id->handle) {
    xTaskAbortDelay(id->handle);
  }
}

void pool_estado(pool_estado_t* out)
{
  memset(out, 0, sizeof(*out));
  out->vagas = POOL_N;
  for (int i = 0; i < POOL_N; ++i) {
    if (s_v[i].state.load() == OCUPADA) { ++out->ocupadas; }
    out->bytes_fixos += s_v[i].bytes + (uint32_t)sizeof(StaticTask_t) + (uint32_t)sizeof(StaticSemaphore_t);
  }
  out->dinamicas = s_dinamicas.load();
}

int pool_pilhas(pool_stack_t* out, int max)
{
  taskENTER_CRITICAL(&s_mux);
  const int n = s_pilhas_n < max ? s_pilhas_n : max;
  memcpy(out, s_pilhas, sizeof(pool_stack_t) * n);
  taskEXIT_CRITICAL(&s_mux);
  return n;
}

void pool_pilhas_reset(void)
{
  taskENTER_CRITICAL(&s_mux);
  s_pilhas_n = 0;
  taskEXIT_CRITICAL(&s_mux);
}
