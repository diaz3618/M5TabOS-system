// Registro de energia - SERVICO DO SISTEMA, nao aplicativo.
//
// POR QUE NAO E UM APP: os aplicativos aqui sao criados ao abrir e destruidos
// ao fechar. Se o registro morasse no app Energia, so haveria dado enquanto ele
// estivesse na tela - e a pergunta que ele responde ("a bateria acabou;
// quando?") exige registro CONTINUO, inclusive com a tela
// apagada e com outro app aberto. Entao ele e um servico do sistema, que liga
// no boot e nunca para - sem tarefa propria: roda de carona na tarefa
// "cartao" (tab5_sd_carona), que ja acorda a cada segundo.
//
// COMO GRAVA SEM GASTAR: cartao montado consome. Entao amostra na RAM a cada
// minuto e so DESCARREGA no cartao a cada 10 - montando, anexando e
// desmontando. O cartao fica desligado mais de 99% do tempo, e mesmo assim
// nenhuma amostra se perde.
//
// Arquivo: /sdcard/apps/Sistema/energia.csv
//   ENE,<t_s>,<V>,<mA>,<mW>,<pct>,<MHz>,<repouso>,<estado>,<app>
//
// -------------------------------------------------------------------------
// SEGUNDO ARQUIVO, NA MESMA JANELA: SAUDE DAS TAREFAS E DO HEAP
// -------------------------------------------------------------------------
// Pedido explicito: uma pasta que guarde, ao longo do tempo, quanta stack
// cada tarefa ainda tem de folga e como anda o heap - para revisar dias
// depois e perceber ANTES um consumo que so cresce, em vez de descobrir
// quando ja virou um Guru Meditation. E o mesmo raciocinio do energia.csv
// (uma curva vale mais que um numero isolado), aplicado a memoria.
//
// Escrito na MESMA descarga que o energia.csv - mesma montagem de cartao,
// mesmo intervalo, nenhuma tarefa nova. So o custo marginal de abrir mais um
// arquivo enquanto o cartao ja esta de pe.
//
// Arquivo: /sdcard/apps/Sistema/tarefas.csv
//   TASKS,<t_s>,<n_tarefas>,<heap_int_livre>,<heap_int_minimo>,<psram_livre>,<psram_minimo>
//   TASK,<t_s>,<nome>,<pilha_livre_bytes>,<core>,<prioridade>   (uma por tarefa)
//
// pilha_livre_bytes e o HIGH WATER MARK - a menor folga que aquela stack ja
// teve desde que nasceu, nao a folga agora. E o numero que importa: ele so
// cai, nunca sobe, e por isso e o que avisa "esta tarefa chegou perto" antes
// do estouro de verdade.

#pragma once

#include <stdint.h>

// Liga o registro (carona na tarefa "cartao"). Chamar depois do M5.begin()
// e do tab5_sd_iniciar().
void registro_iniciar(void);

// Nome do app em primeiro plano, para o sistema informar ao registro.
// Passar NULL quando estiver na tela inicial.
void registro_app_atual(const char* nome);

// Quantas amostras estao esperando na RAM, e quantas ja foram para o cartao.
// Serve para o app Energia mostrar que o registro esta vivo.
void registro_estado(int* na_ram, uint32_t* gravadas, uint32_t* falhas);
