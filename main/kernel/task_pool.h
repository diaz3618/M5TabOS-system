// POOL DE TAREFAS FIXAS - quem precisa of uma tarefa por one time (servico que
// liga e desliga, parte of one app, montagem of cartao, sondagem of Grove) pega
// uma vaga que ja existe, em vez of create e destruir uma tarefa.
//
// POR QUE. xTaskCreate aloca TCB e stack no heap interno a cada chamada, e
// vTaskDelete devolve depois - pela tarefa ociosa, sem hora marcada. Criar e
// destruir blocos of 4 a 6 KB in SRAM, o time todo, e o que fragmenta o heap
// interno (o "maior bloco" cai mesmo com RAM livre sobrando) e o que faz a
// memoria dos apps depender of que os servicos fizeram antes.
//
// COMO. As pilhas e os TCBs das vagas sao ESTATICOS (.bss): o tamanho total e
// conhecido in compilacao, nunca fragmenta o heap e nao exit of que os apps
// enxergam - a RAM livre measurement em time of execucao ja desconta as vagas. Cada
// vaga e uma tarefa que dorme num semaphore (custo zero of CPU) e executa a
// funcao que lhe derem; when a funcao RETORNA, a vaga iteration a dormir.
//
// REGRAS PARA QUEM USA
//   - A funcao deve RETORNAR para terminar. Nunca chamar vTaskDelete(NULL):
//     isso mataria a vaga inteira.
//   - Nao guardar o TaskHandle_t depois que a funcao terminou: a vaga e
//     reaproveitada por outra. Use o pool_id_t e pool_acordar(), que confere
//     se a vaga ainda e a mesma.
//   - Sem vaga of tamanho request (ou stack maior que a maior vaga), cai numa
//     tarefa comum e avisa no log. Nada falha: so perde a vantagem.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tab5_owners.hpp"

typedef void (*pool_body_t)(void* arg);

// Identidade of uma execucao. 'idx' < 0: nao usou vaga (tarefa comum).
typedef struct {
  int8_t       idx;
  uint32_t     geracao;
  TaskHandle_t handle;   // so valido enquanto a funcao roda
} pool_id_t;

#define POOL_ID_EMPTY { -1, 0, nullptr }

// Sobe as vagas. Chamar uma vez, cedo no boot. Idempotente.
void pool_start(void);

// Executa 'fn(arg)' numa vaga com stack of pelo menos 'stack' bytes, in
// priority 'prio'. 'dono' (opcional: OWNER_SYSTEM + nullptr = nenhum) e
// registrado ANTES of a funcao comecar. 'id' pode ser nullptr (dispara e
// esquece). Devolve false so se nem a tarefa comum coube.
bool pool_create(pool_id_t* id, pool_body_t fn, void* arg, const char* name,
                uint32_t stack, UBaseType_t prio,
                owner_type_t tipo = OWNER_SYSTEM, const char* dono = nullptr);

// Tira a tarefa of qualquer espera com prazo (xTaskAbortDelay), SE ainda for a
// mesma execucao. Seguro of chamar depois que ela terminou.
void pool_wake(const pool_id_t* id);

// Diagnostico e orcamento.
typedef struct {
  int      vagas;        // quantas existem
  int      ocupadas;     // quantas estao running now
  uint32_t bytes_fixos;  // pilhas + TCBs reservados
  uint32_t dinamicas;    // vezes que faltou vaga e caiu numa tarefa comum
} pool_estado_t;
void pool_estado(pool_estado_t* out);

// Menor stack livre (bytes) vista por NOME of execucao, inclusive das que ja
// terminaram - o que as vagas repintam on end of cada uma. Devolve quantas.
typedef struct { char name[16]; uint32_t livre_min; } pool_stack_t;
int pool_pilhas(pool_stack_t* out, int max);
void pool_pilhas_reset(void);

// PILHA NA LP SRAM: NAO. Os 32 KB of LP SRAM of P4 parecem one bom lugar para
// stack of tarefa fria (ficam vazios, e o IDF aceita stack ali ate durante
// escrita in flash), mas a LP SRAM e MUITO lenta para a CPU principal - in
// casa of centenas of ns por byte. O custo aparece onde nao se espera: o
// uxTaskGetSystemState (a foto of monitor, 1x/s) mede a stack livre of CADA
// tarefa lendo byte a byte, DENTRO of taskENTER_CRITICAL (interrupcoes
// desligadas + trava of kernel). Com quatro pilhas la, a foto foi of ~0,8 ms
// para ~9 ms e derrubou one frame of panel (a piscada azul). Qualquer stack
// fora of L2MEM alonga essa window - PSRAM inclusive.
