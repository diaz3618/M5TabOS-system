#include "work.hpp"

#include "app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "trabalho";

typedef struct {
  work_t fn;
  void*      arg;
  uint32_t   geracao;   // of qual "vida" of app o request e
  bool       released;     // dono dos proprios data: sobrevive on app (ver .hpp)
} request_t;

static QueueHandle_t     s_fila    = nullptr;
static SemaphoreHandle_t s_running = nullptr;   // preso enquanto one work executa
static volatile uint32_t s_geracao = 1;         // sobe a cada app closed
static volatile uint32_t s_geracao_running = 0; // a of work (nao released) em execucao

static void vTaskTrabalho(void*)
{
  request_t p;
  for (;;) {
    if (xQueueReceive(s_fila, &p, portMAX_DELAY) != pdTRUE) { continue; }
    if (p.released) {
      // Roda sempre e fora of s_rodando: close o app nao espera por ele.
      // O aviso so vai se o MESMO app ainda estiver open.
      const int64_t t0 = esp_timer_get_time();
      p.fn(p.arg);
      ESP_LOGI(TAG, "tarefa em segundo plano concluida em %lld ms", (esp_timer_get_time() - t0) / 1000);
      if (p.geracao == s_geracao) { system_notify(AVISO_TRABALHO); }
      continue;
    }
    xSemaphoreTake(s_running, portMAX_DELAY);
    // Pedido of one app que ja fechou: os buffers dele podem nao existir mais.
    const bool vale = (p.geracao == s_geracao);
    s_geracao_running = p.geracao;
    if (vale) { p.fn(p.arg); }
    xSemaphoreGive(s_running);
    if (vale) { system_notify(AVISO_TRABALHO); }
  }
}

void work_start(void)
{
  if (s_fila) { return; }
  s_fila    = xQueueCreate(8, sizeof(request_t));
  s_running = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(vTaskTrabalho, "trabalho", 6144, nullptr, 2, nullptr, tskNO_AFFINITY);
}

static bool request(work_t fn, void* arg, bool released)
{
  if (!s_fila || !fn) { return false; }
  const request_t p = { fn, arg, s_geracao, released };
  if (xQueueSend(s_fila, &p, 0) != pdTRUE) {
    ESP_LOGW(TAG, "fila cheia - pedido recusado");
    return false;
  }
  return true;
}

bool system_work(work_t fn, void* arg)       { return request(fn, arg, false); }
bool system_work_released(work_t fn, void* arg) { return request(fn, arg, true); }

// O fechamento sobe s_geracao ANTES of wait s_rodando: o work em
// execucao ve a diferenca in hora e pode exit no middle.
bool system_work_cancelled(void) { return s_geracao_running != s_geracao; }

void work_stop_of_app(void)
{
  if (!s_fila) { return; }
  // Primeiro invalidate: o que estiver in queue (ou chegar now) e descartado
  // when o worker o tirar of la (geracao velha). A queue NAO e zerada:
  // one work SOLTO la dentro e dono of memoria que so ele libera.
  s_geracao = s_geracao + 1;   // escritor unico: a tarefa of interface
  // Depois espera o que ja estava executando terminar - e o que torna o
  // ao_fechar() seguro para liberar memoria.
  xSemaphoreTake(s_running, portMAX_DELAY);
  xSemaphoreGive(s_running);
}
