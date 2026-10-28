// PARADA COOPERATIVA DE UMA TAREFA DE SERVICO.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// Tres servicos (GPS, Mesh, enlace Bluetooth do Mesh) param a propria tarefa
// do mesmo jeito - e cada um escrevia o mecanismo de novo: uma bandeira
// "pediram para parar", outra "a tarefa saiu", e quem pede ESPERA
// PERGUNTANDO a cada 10 ms (for ... vTaskDelay(10)). Tres copias com
// diferencas que ninguem escolheu: no GPS as bandeiras nem eram volatile
// (lidas de uma tarefa, escritas de outra), e o fim de cada espera atrasava
// ate 10 ms depois de a tarefa ja ter saido.
//
// Aqui a tarefa DA um semaforo ao sair e quem pediu ESPERA NELE: acorda no
// instante da saida, sem perguntar nada no meio. A regra de seguranca dos
// tres continua a mesma, agora escrita uma vez so (parada_pedir).
//
// -------------------------------------------------------------------------
// POR QUE COOPERATIVA (e nao vTaskDelete, nem xTaskAbortDelay)
// -------------------------------------------------------------------------
// A tarefa pega mutex e escreve no cartao; mata-la no meio deixa o mutex
// preso para sempre (o FreeRTOS nao revoga posse). E o xTaskAbortDelay, que
// a moldura dos apps usa (app_task.h), tambem NAO serve aqui: ele tira a
// tarefa de QUALQUER espera - inclusive de um xSemaphoreTake(mutex,
// portMAX_DELAY) cujo retorno estes servicos nao conferem. A tarefa seguiria
// achando que tem o mutex, e o Give seguinte, de quem nao e dono, derruba o
// aparelho. Entao a tarefa sai SOZINHA, no ponto de saida dela, onde nao
// segura nada; a espera de quem pediu e o tempo maximo de uma volta (o
// prazo da leitura da UART, tipicamente).
//
// -------------------------------------------------------------------------
// USO
// -------------------------------------------------------------------------
//   static parada_t s_parada;
//
//   iniciar():  if (!parada_preparar(&s_parada)) { ...sem memoria... }
//               if (!pool_criar(...)) { parada_desistir(&s_parada); ... }
//   tarefa:     while (!parada_pedida(&s_parada)) { ...uma volta... }
//               ...soltar o que so a tarefa segura...
//               parada_confirmar(&s_parada);   // a ULTIMA coisa
//   parar():    if (!parada_pedir(&s_parada, 1000)) { return false; }
//               ...so agora desmontar UART, filas, mutex...

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
  SemaphoreHandle_t saiu;     // dado pela tarefa ao sair; quem pediu espera nele
  volatile bool     pedido;   // pediram para parar - a tarefa le a cada volta
  volatile bool     viva;     // ha uma execucao que ainda nao foi recolhida
} parada_t;

// Antes de criar a tarefa. O semaforo nasce uma vez e serve para sempre.
// false = sem memoria (nao crie a tarefa).
static inline bool parada_preparar(parada_t* p)
{
  if (!p->saiu) { p->saiu = xSemaphoreCreateBinary(); }
  if (!p->saiu) { return false; }
  xSemaphoreTake(p->saiu, 0);   // sobra de uma execucao anterior: nao conta para esta
  p->pedido = false;
  p->viva   = true;
  return true;
}

// A tarefa nao chegou a ser criada: desfaz o preparar.
static inline void parada_desistir(parada_t* p) { p->viva = false; p->pedido = false; }

// Na tarefa: verdadeiro quando e hora de sair.
static inline bool parada_pedida(const parada_t* p) { return p->pedido; }

// Na tarefa: a ULTIMA coisa antes de retornar. Daqui em diante quem pediu
// pode desmontar tudo o que ela usava.
static inline void parada_confirmar(parada_t* p) { xSemaphoreGive(p->saiu); }

// A tarefa existe e ainda nao confirmou saida.
static inline bool parada_rodando(const parada_t* p)
{
  return p->viva && p->saiu && uxSemaphoreGetCount(p->saiu) == 0;
}

// Pedido feito e ainda nao recolhido (o "ainda parando" dos servicos).
static inline bool parada_em_curso(const parada_t* p) { return p->viva && p->pedido; }

// PEDE E ESPERA ate 'ms'. 'cutucar' (opcional) e chamado ja e a cada 100 ms
// enquanto espera - para a tarefa que dorme numa espera PROPRIA (o enlace
// Bluetooth espera notificacao e resposta da pilha BLE) e precisa ser
// acordada para enxergar o pedido.
//
// true  = a tarefa saiu: e seguro desmontar o que ela usava. Tambem true se
//         nao havia tarefa.
// false = nao confirmou no prazo. O PEDIDO FICA DE PE e NADA deve ser
//         desmontado: a tarefa sai quando puder, e a proxima chamada (que
//         acha o semaforo ja dado) termina o servico. Baixar o pedido aqui
//         abriria a corrida em que a tarefa ja leu "parar", sai, e ninguem
//         mais recolhe - UART e mutex orfaos com o servico dizendo "de pe".
static inline bool parada_pedir(parada_t* p, uint32_t ms, void (*cutucar)(void) = nullptr)
{
  if (!p->viva) { return true; }
  p->pedido = true;
  const uint32_t fatia = cutucar ? 100 : ms;
  for (uint32_t esperou = 0; ; esperou += fatia) {
    if (cutucar) { cutucar(); }
    if (xSemaphoreTake(p->saiu, pdMS_TO_TICKS(fatia)) == pdTRUE) {
      p->viva   = false;
      p->pedido = false;
      return true;
    }
    if (esperou + fatia >= ms) { return false; }
  }
}
