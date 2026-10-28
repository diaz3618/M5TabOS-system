// POOL DE TAREFAS FIXAS - quem precisa de uma tarefa por um tempo (servico que
// liga e desliga, parte de um app, montagem do cartao, sondagem do Grove) pega
// uma vaga que ja existe, em vez de criar e destruir uma tarefa.
//
// POR QUE. xTaskCreate aloca TCB e pilha no heap interno a cada chamada, e
// vTaskDelete devolve depois - pela tarefa ociosa, sem hora marcada. Criar e
// destruir blocos de 4 a 6 KB na SRAM, o tempo todo, e o que fragmenta o heap
// interno (o "maior bloco" cai mesmo com RAM livre sobrando) e o que faz a
// memoria dos apps depender do que os servicos fizeram antes.
//
// COMO. As pilhas e os TCBs das vagas sao ESTATICOS (.bss): o tamanho total e
// conhecido na compilacao, nunca fragmenta o heap e nao sai do que os apps
// enxergam - a RAM livre medida em tempo de execucao ja desconta as vagas. Cada
// vaga e uma tarefa que dorme num semaphore (custo zero de CPU) e executa a
// funcao que lhe derem; quando a funcao RETORNA, a vaga volta a dormir.
//
// REGRAS PARA QUEM USA
//   - A funcao deve RETORNAR para terminar. Nunca chamar vTaskDelete(NULL):
//     isso mataria a vaga inteira.
//   - Nao guardar o TaskHandle_t depois que a funcao terminou: a vaga e
//     reaproveitada por outra. Use o pool_id_t e pool_acordar(), que confere
//     se a vaga ainda e a mesma.
//   - Sem vaga do tamanho pedido (ou pilha maior que a maior vaga), cai numa
//     tarefa comum e avisa no log. Nada falha: so perde a vantagem.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tab5_owners.hpp"

typedef void (*pool_corpo_t)(void* arg);

// Identidade de uma execucao. 'idx' < 0: nao usou vaga (tarefa comum).
typedef struct {
  int8_t       idx;
  uint32_t     geracao;
  TaskHandle_t handle;   // so valido enquanto a funcao roda
} pool_id_t;

#define POOL_ID_VAZIO { -1, 0, nullptr }

// Sobe as vagas. Chamar uma vez, cedo no boot. Idempotente.
void pool_iniciar(void);

// Executa 'fn(arg)' numa vaga com pilha de pelo menos 'pilha' bytes, na
// prioridade 'prio'. 'dono' (opcional: DONO_SISTEMA + nullptr = nenhum) e
// registrado ANTES de a funcao comecar. 'id' pode ser nullptr (dispara e
// esquece). Devolve false so se nem a tarefa comum coube.
bool pool_criar(pool_id_t* id, pool_corpo_t fn, void* arg, const char* nome,
                uint32_t pilha, UBaseType_t prio,
                dono_tipo_t tipo = DONO_SISTEMA, const char* dono = nullptr);

// Tira a tarefa de qualquer espera com prazo (xTaskAbortDelay), SE ainda for a
// mesma execucao. Seguro de chamar depois que ela terminou.
void pool_acordar(const pool_id_t* id);

// Diagnostico e orcamento.
typedef struct {
  int      vagas;        // quantas existem
  int      ocupadas;     // quantas estao rodando agora
  uint32_t bytes_fixos;  // pilhas + TCBs reservados
  uint32_t dinamicas;    // vezes que faltou vaga e caiu numa tarefa comum
} pool_estado_t;
void pool_estado(pool_estado_t* out);

// Menor pilha livre (bytes) vista por NOME de execucao, inclusive das que ja
// terminaram - o que as vagas repintam ao fim de cada uma. Devolve quantas.
typedef struct { char nome[16]; uint32_t livre_min; } pool_pilha_t;
int pool_pilhas(pool_pilha_t* out, int max);
void pool_pilhas_zerar(void);

// PILHA NA LP SRAM: NAO. Os 32 KB de LP SRAM da P4 parecem um bom lugar para
// pilha de tarefa fria (ficam vazios, e o IDF aceita pilha ali ate durante
// escrita na flash), mas a LP SRAM e MUITO lenta para a CPU principal - na
// casa de centenas de ns por byte. O custo aparece onde nao se espera: o
// uxTaskGetSystemState (a foto do monitor, 1x/s) mede a pilha livre de CADA
// tarefa lendo byte a byte, DENTRO de taskENTER_CRITICAL (interrupcoes
// desligadas + trava do kernel). Com quatro pilhas la, a foto foi de ~0,8 ms
// para ~9 ms e derrubou um quadro do painel (a piscada azul). Qualquer pilha
// fora da L2MEM alonga essa janela - PSRAM inclusive.
