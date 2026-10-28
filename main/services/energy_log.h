// Registro of energia - SERVICO DO SISTEMA, nao aplicativo.
//
// POR QUE NAO E UM APP: os aplicativos aqui sao criados on open e destruidos
// on close. Se o registro morasse no app Energia, so haveria dado enquanto ele
// estivesse in screen - e a pergunta que ele responde ("a battery acabou;
// when?") requires registro CONTINUO, inclusive com a screen
// apagada e com outro app open. Entao ele e one servico of sistema, que liga
// no boot e nunca para - sem tarefa propria: roda of carona in tarefa
// "cartao" (tab5_sd_carona), que ja acorda a cada segundo.
//
// COMO GRAVA SEM GASTAR: cartao montado consome. Entao sample in RAM a cada
// minuto e so DESCARREGA no cartao a cada 10 - montando, anexando e
// desmontando. O cartao stays disabled mais of 99% of time, e mesmo assim
// none sample se perde.
//
// Arquivo: /sdcard/apps/Sistema/energia.csv
//   ENE,<t_s>,<V>,<mA>,<mW>,<pct>,<MHz>,<sleep>,<estado>,<app>
//
// -------------------------------------------------------------------------
// SEGUNDO ARQUIVO, NA MESMA JANELA: SAUDE DAS TAREFAS E DO HEAP
// -------------------------------------------------------------------------
// Pedido explicito: uma pasta que guarde, on longo of time, quanta stack
// cada tarefa ainda tem of folga e como anda o heap - para revisar dias
// depois e perceber ANTES one consumo que so cresce, em vez of descobrir
// when ja virou one Guru Meditation. E o mesmo raciocinio of energia.csv
// (uma curva vale mais que one numero isolado), aplicado a memoria.
//
// Escrito in MESMA descarga que o energia.csv - mesma montagem of cartao,
// mesmo intervalo, none tarefa nova. So o custo marginal of open mais one
// arquivo enquanto o cartao ja esta of pe.
//
// Arquivo: /sdcard/apps/Sistema/tarefas.csv
//   TASKS,<t_s>,<n_tarefas>,<heap_int_livre>,<heap_int_minimo>,<psram_livre>,<psram_minimo>
//   TASK,<t_s>,<name>,<pilha_livre_bytes>,<core>,<priority>   (uma por tarefa)
//
// pilha_livre_bytes e o HIGH WATER MARK - a menor folga que aquela stack ja
// teve since que nasceu, nao a folga now. E o numero que importa: ele so
// cai, nunca sobe, e por isso e o que avisa "esta tarefa chegou perto" antes
// of estouro of verdade.

#pragma once

#include <stdint.h>

// Liga o registro (carona in tarefa "cartao"). Chamar depois of M5.begin()
// e of tab5_sd_init().
void registro_start(void);

// Nome of app em first plano, para o sistema informar on registro.
// Passar NULL when estiver in screen inicial.
void registro_app_current(const char* name);

// Quantas amostras estao esperando in RAM, e quantas ja foram para o cartao.
// Serve para o app Energia mostrar que o registro esta vivo.
void registro_estado(int* in_ram, uint32_t* gravadas, uint32_t* falhas);
