// Moldura para as tarefas de um aplicativo, com ENCERRAMENTO ORDENADO.
//
// O PROBLEMA que ela resolve: matar uma tarefa de fora com vTaskDelete() para
// ela onde estiver - inclusive DENTRO de um driver, segurando um semaphore
// interno. E o FreeRTOS nao tem como revogar posse: mutex de tarefa morta fica
// tomado para sempre, com o ponteiro do dono apontando para lixo. Nao existe
// "forcar a devolucao".
//
// O UNICO caminho correto e nao matar: pedir para sair e deixar a propria
// tarefa devolver o que pegou. O que torna isso rapido e o xTaskAbortDelay(),
// que tira a tarefa do estado bloqueado NA HORA - ela retorna do
// uart_read_bytes, do xQueueReceive, do vTaskDelay, como se o prazo tivesse
// expirado. Sem ele, seria preciso esperar o timeout natural de cada espera.
//
// COMO USAR:
//
//   static tarefa_app_t s_t;
//
//   static void meu_laco(tarefa_app_t* t) {
//       while (tarefa_app_ativa(t)) {
//           ...trabalho...                      // com esperas LIMITADAS
//       }
//       // aqui devolva o que voce pegou: fclose, xSemaphoreGive, etc.
//   }
//
//   ao_abrir():   tarefa_app_criar(&s_t, meu_laco, "gps", 4096, 3);
//   ao_fechar():  tarefa_app_encerrar(&s_t, 200);   // so entao destrua queues
//
// Quando o loop RETORNA, a moldura avisa quem espera e a vaga do pool de
// tarefas (kernel/task_pool.h) volta a dormir - o app NUNCA chama
// vTaskDelete (mataria a vaga inteira). E o encerrar() so volta depois que a
// tarefa confirmou que saiu.

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "task_pool.h"

typedef struct tarefa_app_s tarefa_app_t;

typedef void (*tarefa_app_corpo_t)(tarefa_app_t* t);

struct tarefa_app_s {
  TaskHandle_t       handle;   // nullptr = nao ha tarefa (a vaga pode ser de outra execucao depois)
  pool_id_t          id;       // a execucao no pool de tarefas fixas (kernel/task_pool.h)
  SemaphoreHandle_t  fim;      // a tarefa entrega quando terminou de limpar
  volatile bool      parar;
  tarefa_app_corpo_t corpo;
};

// Cria a tarefa. Devolve false se faltou memoria (e nada fica pela metade).
bool tarefa_app_criar(tarefa_app_t* t, tarefa_app_corpo_t corpo,
                      const char* nome, uint32_t pilha, UBaseType_t prioridade);

// O loop da tarefa pergunta isto a cada volta.
static inline bool tarefa_app_ativa(const tarefa_app_t* t) { return !t->parar; }

// Pede a saida, desbloqueia a tarefa e ESPERA ela confirmar, ate ms.
// Depois que esta funcao volta, e seguro destruir queues, drivers e buffers.
void tarefa_app_encerrar(tarefa_app_t* t, uint32_t ms);
