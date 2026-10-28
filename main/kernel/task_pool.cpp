#include "task_pool.h"

#include <atomic>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"   // pxTaskGetStackStart
#include "esp_log.h"

static const char* TAG = "pool";

// CLASSES DE VAGA. O consumo real medido (menor pilha livre em uso de verdade)
// manda aqui: servicos de serial e apps cabem em 4 KB; o Mesh pede 6 KB.
// O total e ~26 KB e substitui o que esses mesmos usos pediam ao heap,
// no pior caso, como blocos separados.
#define VAGA_G  6144
#define VAGA_M  4096

#define PILHA_ESTATICA(nome, bytes) alignas(16) static StackType_t nome[(bytes) / sizeof(StackType_t)]
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
  StackType_t*      pilha;
  uint32_t          bytes;
  TaskHandle_t      h;
  SemaphoreHandle_t acordar;
  std::atomic<int>  state;
  volatile uint32_t geracao;
  pool_corpo_t      fn;
  void*             arg;
  char              name[16];
} vaga_t;

static vaga_t s_v[POOL_N];
static bool   s_pronto = false;
static std::atomic<uint32_t> s_dinamicas{0};

// Menor pilha livre por nome de execucao.
#define PILHAS_N 24
static pool_pilha_t s_pilhas[PILHAS_N];
static int          s_pilhas_n = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void anotar_pilha(const char* nome, uint32_t livre)
{
  taskENTER_CRITICAL(&s_mux);
  int k = 0;
  while (k < s_pilhas_n && strncmp(s_pilhas[k].name, nome, sizeof(s_pilhas[k].name)) != 0) { ++k; }
  if (k < PILHAS_N) {
    if (k == s_pilhas_n) {
      snprintf(s_pilhas[k].name, sizeof(s_pilhas[k].name), "%s", nome);
      s_pilhas[k].livre_min = livre;
      ++s_pilhas_n;
    } else if (livre < s_pilhas[k].livre_min) {
      s_pilhas[k].livre_min = livre;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
}

// Repinta com o padrao 0xA5 a parte LIVRE da pilha, para que o high water mark
// da proxima execucao meca so a dela.
//
// ONDE COMECA A PILHA: NAO e o inicio do buffer (v->pilha). No P4 o IDF guarda
// no pe do buffer a area de contexto dos coprocessadores (FPU e PIE) da tarefa,
// e so depois dela comeca a pilha que o FreeRTOS conhece (pxStack). Nos
// primeiros 32 bytes de pxStack, alinhados a 32, fica o vigia de fim de pilha
// (FREERTOS_WATCHPOINT_END_OF_STACK): qualquer escrita ali derruba o sistema
// com "Breakpoint" - de proposito, e o que pega estouro de pilha na hora.
// Repintar a partir de "buffer + 128" apagava o contexto do FPU/PIE e batia no
// vigia (Guru Meditation ao fechar a Camera). Por isso o inicio vem do proprio
// FreeRTOS, e pula-se o vigia e o canario com folga.
static void __attribute__((noinline)) repintar(void)
{
  const uintptr_t ini_pilha = (uintptr_t)pxTaskGetStackStart(nullptr);
  uint8_t* base = (uint8_t*)(((ini_pilha + 31u) & ~(uintptr_t)31u) + 64u);   // vigia (32 B) + canario, com folga
  uint8_t* topo = (uint8_t*)__builtin_frame_address(0) - 512;
  if (topo > base) { memset(base, 0xA5, (size_t)(topo - base)); }
}

static void vTaskVaga(void* a)
{
  vaga_t* v = (vaga_t*)a;
  for (;;) {
    // xTaskAbortDelay pode acordar aqui sem trabalho: volta a dormir.
    if (xSemaphoreTake(v->acordar, portMAX_DELAY) != pdTRUE) { continue; }
    pool_corpo_t fn = v->fn;
    if (!fn) { continue; }
    ulTaskNotifyTake(pdTRUE, 0);   // aviso que sobrou da execucao anterior

    fn(v->arg);

    tab5_owner_forget(v->h);
    anotar_pilha(v->name, (uint32_t)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
    ulTaskNotifyTake(pdTRUE, 0);
    repintar();
    v->fn = nullptr;
    v->state.store(LIVRE, std::memory_order_release);
  }
}

void pool_iniciar(void)
{
  if (s_pronto) { return; }
  StackType_t* pilhas[POOL_N] = { s_pilha0, s_pilha1, s_pilha2, s_pilha3, s_pilha4, s_pilha5 };
  const uint32_t tam[POOL_N]  = { VAGA_G, VAGA_M, VAGA_M, VAGA_M, VAGA_M, VAGA_M };
  uint32_t fixos = 0;
  for (int i = 0; i < POOL_N; ++i) {
    vaga_t* v = &s_v[i];
    v->pilha = pilhas[i];
    v->bytes = tam[i];
    v->state.store(LIVRE);
    v->acordar = xSemaphoreCreateBinaryStatic(&v->sem_buf);
    char nome[8];
    snprintf(nome, sizeof(nome), "pool%d", i);
    v->h = xTaskCreateStaticPinnedToCore(vTaskVaga, nome, v->bytes, v, 1,
                                         v->pilha, &v->tcb, tskNO_AFFINITY);
    fixos += v->bytes + (uint32_t)sizeof(StaticTask_t) + (uint32_t)sizeof(StaticSemaphore_t);
  }
  s_pronto = true;
  ESP_LOGI(TAG, "%d tarefas fixas, %lu B reservados (pilhas e TCBs estaticos)",
           POOL_N, (unsigned long)fixos);
}

// Execucao em tarefa comum, quando nao ha vaga.
typedef struct { pool_corpo_t fn; void* arg; } dinamica_t;
static void vTaskDinamica(void* a)
{
  dinamica_t d = *(dinamica_t*)a;
  free(a);
  d.fn(d.arg);
  tab5_owner_forget(xTaskGetCurrentTaskHandle());
  vTaskDelete(nullptr);
}

bool pool_criar(pool_id_t* id, pool_corpo_t fn, void* arg, const char* nome,
                uint32_t pilha, UBaseType_t prio, owner_type_t tipo, const char* dono)
{
  if (id) { *id = (pool_id_t)POOL_ID_VAZIO; }
  if (!fn) { return false; }
  if (!s_pronto) { pool_iniciar(); }

  // A menor vaga livre que serve: o Mesh nao toma a de 6 KB quando cabe em 4.
  // As de 4 KB sao as de indice 1..N-1; a de 6 KB e a 0 e fica por ultimo.
  vaga_t* esc = nullptr;
  for (int k = 1; k <= POOL_N && !esc; ++k) {
    vaga_t* v = &s_v[k % POOL_N];
    if (v->bytes < pilha) { continue; }
    int esperado = LIVRE;
    if (v->state.compare_exchange_strong(esperado, OCUPADA, std::memory_order_acq_rel)) { esc = v; }
  }

  if (esc) {
    snprintf(esc->name, sizeof(esc->name), "%s", nome ? nome : "?");
    esc->fn  = fn;
    esc->arg = arg;
    esc->geracao = esc->geracao + 1;
    if (dono) { tab5_owner_register(esc->h, tipo, dono); }
    if (id) { id->idx = (int8_t)(esc - s_v); id->geracao = esc->geracao; id->handle = esc->h; }
    vTaskPrioritySet(esc->h, prio);
    xSemaphoreGive(esc->acordar);
    return true;
  }

  s_dinamicas.fetch_add(1);
  ESP_LOGW(TAG, "sem vaga para '%s' (%lu B) - tarefa comum", nome ? nome : "?", (unsigned long)pilha);
  dinamica_t* d = (dinamica_t*)malloc(sizeof(dinamica_t));
  if (!d) { return false; }
  d->fn = fn; d->arg = arg;
  TaskHandle_t h = nullptr;
  if (xTaskCreatePinnedToCore(vTaskDinamica, nome ? nome : "tarefa", pilha, d, prio, &h,
                              tskNO_AFFINITY) != pdPASS) {
    free(d);
    return false;
  }
  if (dono) { tab5_owner_register(h, tipo, dono); }
  if (id) { id->handle = h; }
  return true;
}

void pool_acordar(const pool_id_t* id)
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

int pool_pilhas(pool_pilha_t* out, int max)
{
  taskENTER_CRITICAL(&s_mux);
  const int n = s_pilhas_n < max ? s_pilhas_n : max;
  memcpy(out, s_pilhas, sizeof(pool_pilha_t) * n);
  taskEXIT_CRITICAL(&s_mux);
  return n;
}

void pool_pilhas_zerar(void)
{
  taskENTER_CRITICAL(&s_mux);
  s_pilhas_n = 0;
  taskEXIT_CRITICAL(&s_mux);
}
