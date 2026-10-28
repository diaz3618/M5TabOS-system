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
  trabalho_t fn;
  void*      arg;
  uint32_t   geracao;   // de qual "vida" do app o pedido e
  bool       solto;     // dono dos proprios dados: sobrevive ao app (ver .hpp)
} pedido_t;

static QueueHandle_t     s_fila    = nullptr;
static SemaphoreHandle_t s_rodando = nullptr;   // preso enquanto um trabalho executa
static volatile uint32_t s_geracao = 1;         // sobe a cada app fechado
static volatile uint32_t s_geracao_rodando = 0; // a do trabalho (nao solto) em execucao

static void vTaskTrabalho(void*)
{
  pedido_t p;
  for (;;) {
    if (xQueueReceive(s_fila, &p, portMAX_DELAY) != pdTRUE) { continue; }
    if (p.solto) {
      // Roda sempre e fora de s_rodando: fechar o app nao espera por ele.
      // O aviso so vai se o MESMO app ainda estiver aberto.
      const int64_t t0 = esp_timer_get_time();
      p.fn(p.arg);
      ESP_LOGI(TAG, "tarefa em segundo plano concluida em %lld ms", (esp_timer_get_time() - t0) / 1000);
      if (p.geracao == s_geracao) { sistema_avisar(AVISO_TRABALHO); }
      continue;
    }
    xSemaphoreTake(s_rodando, portMAX_DELAY);
    // Pedido de um app que ja fechou: os buffers dele podem nao existir mais.
    const bool vale = (p.geracao == s_geracao);
    s_geracao_rodando = p.geracao;
    if (vale) { p.fn(p.arg); }
    xSemaphoreGive(s_rodando);
    if (vale) { sistema_avisar(AVISO_TRABALHO); }
  }
}

void trabalho_iniciar(void)
{
  if (s_fila) { return; }
  s_fila    = xQueueCreate(8, sizeof(pedido_t));
  s_rodando = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(vTaskTrabalho, "trabalho", 6144, nullptr, 2, nullptr, tskNO_AFFINITY);
}

static bool pedir(trabalho_t fn, void* arg, bool solto)
{
  if (!s_fila || !fn) { return false; }
  const pedido_t p = { fn, arg, s_geracao, solto };
  if (xQueueSend(s_fila, &p, 0) != pdTRUE) {
    ESP_LOGW(TAG, "fila cheia - pedido recusado");
    return false;
  }
  return true;
}

bool sistema_trabalho(trabalho_t fn, void* arg)       { return pedir(fn, arg, false); }
bool sistema_trabalho_solto(trabalho_t fn, void* arg) { return pedir(fn, arg, true); }

// O fechamento sobe s_geracao ANTES de esperar s_rodando: o trabalho em
// execucao ve a diferenca na hora e pode sair no meio.
bool sistema_trabalho_cancelado(void) { return s_geracao_rodando != s_geracao; }

void trabalho_encerrar_do_app(void)
{
  if (!s_fila) { return; }
  // Primeiro invalida: o que estiver na queue (ou chegar agora) e descartado
  // quando o worker o tirar de la (geracao velha). A queue NAO e zerada:
  // um trabalho SOLTO la dentro e dono de memoria que so ele libera.
  s_geracao = s_geracao + 1;   // escritor unico: a tarefa de interface
  // Depois espera o que ja estava executando terminar - e o que torna o
  // ao_fechar() seguro para liberar memoria.
  xSemaphoreTake(s_rodando, portMAX_DELAY);
  xSemaphoreGive(s_rodando);
}
