// PARADA COOPERATIVA DE UMA TAREFA DE SERVICO.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// Tres servicos (GPS, Mesh, enlace Bluetooth of Mesh) param a propria tarefa
// of mesmo jeito - e cada one escrevia o mecanismo of novo: uma bandeira
// "pediram para stop", outra "a tarefa exited", e quem pede ESPERA
// PERGUNTANDO a cada 10 ms (for ... vTaskDelay(10)). Tres copias com
// diferencas que ninguem escolheu: no GPS as bandeiras nem eram volatile
// (lidas of uma tarefa, escritas of outra), e o end of cada espera atrasava
// ate 10 ms depois of a tarefa ja ter saido.
//
// Aqui a tarefa DA one semaforo on exit e quem pediu ESPERA NELE: acorda no
// instante of saida, sem perguntar nada no middle. A regra of seguranca dos
// tres continua a mesma, now escrita uma vez so (parada_pedir).
//
// -------------------------------------------------------------------------
// POR QUE COOPERATIVA (e nao vTaskDelete, nem xTaskAbortDelay)
// -------------------------------------------------------------------------
// A tarefa pega mutex e escreve no cartao; mata-la no middle deixa o mutex
// preso para sempre (o FreeRTOS nao revoga posse). E o xTaskAbortDelay, que
// a moldura dos apps usa (app_task.h), tambem NAO serve aqui: ele tira a
// tarefa of QUALQUER espera - inclusive of one xSemaphoreTake(mutex,
// portMAX_DELAY) cujo return estes servicos nao conferem. A tarefa seguiria
// achando que tem o mutex, e o Give seguinte, of quem nao e dono, derruba o
// aparelho. Entao a tarefa exit SOZINHA, no ponto of saida dela, onde nao
// segura nada; a espera of quem pediu e o time maximum of uma iteration (o
// prazo of leitura of UART, tipicamente).
//
// -------------------------------------------------------------------------
// USO
// -------------------------------------------------------------------------
//   static parada_t s_parada;
//
//   start():  if (!parada_preparar(&s_parada)) { ...sem memoria... }
//               if (!pool_criar(...)) { parada_desistir(&s_parada); ... }
//   tarefa:     while (!parada_pedida(&s_parada)) { ...uma iteration... }
//               ...release o que so a tarefa segura...
//               parada_confirmar(&s_parada);   // a ULTIMA coisa
//   stop():    if (!parada_pedir(&s_parada, 1000)) { return false; }
//               ...so now desmontar UART, filas, mutex...

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
  SemaphoreHandle_t exited;     // dado pela tarefa on exit; quem pediu espera nele
  volatile bool     request;   // pediram para stop - a tarefa le a cada iteration
  volatile bool     alive;     // ha uma execucao que ainda nao foi recolhida
} stop_t;

// Antes of create a tarefa. O semaforo nasce uma vez e serve para sempre.
// false = sem memoria (nao crie a tarefa).
static inline bool stop_prepare(stop_t* p)
{
  if (!p->exited) { p->exited = xSemaphoreCreateBinary(); }
  if (!p->exited) { return false; }
  xSemaphoreTake(p->exited, 0);   // sobra of uma execucao anterior: nao conta para esta
  p->request = false;
  p->alive   = true;
  return true;
}

// A tarefa nao chegou a ser criada: desfaz o prepare.
static inline void stop_abandon(stop_t* p) { p->alive = false; p->request = false; }

// Na tarefa: verdadeiro when e hora of exit.
static inline bool stop_requested(const stop_t* p) { return p->request; }

// Na tarefa: a ULTIMA coisa antes of retornar. Daqui em diante quem pediu
// pode desmontar everything o que ela usava.
static inline void stop_confirm(stop_t* p) { xSemaphoreGive(p->exited); }

// A tarefa existe e ainda nao confirmou saida.
static inline bool stop_running(const stop_t* p)
{
  return p->alive && p->exited && uxSemaphoreGetCount(p->exited) == 0;
}

// Pedido feito e ainda nao recolhido (o "ainda stopping" dos servicos).
static inline bool stop_em_progress(const stop_t* p) { return p->alive && p->request; }

// PEDE E ESPERA ate 'ms'. 'cutucar' (opcional) e chamado ja e a cada 100 ms
// enquanto espera - para a tarefa que dorme numa espera PROPRIA (o enlace
// Bluetooth espera notification e resposta of stack BLE) e precisa ser
// acordada para enxergar o request.
//
// true  = a tarefa exited: e seguro desmontar o que ela usava. Tambem true se
//         nao havia tarefa.
// false = nao confirmou no prazo. O PEDIDO FICA DE PE e NADA deve ser
//         desmontado: a tarefa exit when puder, e a next chamada (que
//         acha o semaforo ja dado) termina o servico. Baixar o request aqui
//         abriria a corrida em que a tarefa ja leu "stop", exit, e ninguem
//         mais recolhe - UART e mutex orfaos com o servico dizendo "of pe".
static inline bool stop_request(stop_t* p, uint32_t ms, void (*cutucar)(void) = nullptr)
{
  if (!p->alive) { return true; }
  p->request = true;
  const uint32_t fatia = cutucar ? 100 : ms;
  for (uint32_t esperou = 0; ; esperou += fatia) {
    if (cutucar) { cutucar(); }
    if (xSemaphoreTake(p->exited, pdMS_TO_TICKS(fatia)) == pdTRUE) {
      p->alive   = false;
      p->request = false;
      return true;
    }
    if (esperou + fatia >= ms) { return false; }
  }
}
