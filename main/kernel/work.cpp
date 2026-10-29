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
  uint32_t   geracao;   // of which "life" of app or request and
  bool       released;     // owner of own data: survive on app (see .hpp)
} request_t;

static QueueHandle_t     s_fila    = nullptr;
static SemaphoreHandle_t s_running = nullptr;   // stuck while one work runs
static volatile uint32_t s_geracao = 1;         // goes up with each app closed
static volatile uint32_t s_geracao_running = 0; // a of work (not released) running

static void vTaskTrabalho(void*)
{
  request_t p;
  for (;;) {
    if (xQueueReceive(s_fila, &p, portMAX_DELAY) != pdTRUE) { continue; }
    if (p.released) {
      // Roda always and out of s_rodando: close the app does not wait for it.
      // The warning only appears if the SAME app is still open.
      const int64_t t0 = esp_timer_get_time();
      p.fn(p.arg);
      ESP_LOGI(TAG, "tarefa em segundo plano concluida em %lld ms", (esp_timer_get_time() - t0) / 1000);
      if (p.geracao == s_geracao) { system_notify(AVISO_TRABALHO); }
      continue;
    }
    xSemaphoreTake(s_running, portMAX_DELAY);
    // Request for an app that has already closed: its buffers may no longer exist.
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

// Closing goes up s_geracao BEFORE of wait s_rodando: the work in
// execution sees the difference right away and can exit in the middle.
bool system_work_cancelled(void) { return s_geracao_running != s_geracao; }

void work_stop_of_app(void)
{
  if (!s_fila) { return; }
  // First invalidate: whatever is in queue (or arrives now) is discarded
  // when the worker takes him out of there (old generation). The queue is NOT reset:
  // one work LOOSE inside and owner of memory that only it releases.
  s_geracao = s_geracao + 1;   // unique writer: the task of interface
  // Then wait for what was already running to finish - and what makes the
  // when_closing() safe to free memory.
  xSemaphoreTake(s_running, portMAX_DELAY);
  xSemaphoreGive(s_running);
}
