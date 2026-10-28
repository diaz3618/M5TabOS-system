// HAL DE FRAME - a camada mais baixa da nova base de tela (COMPOSITOR.md,
// etapa 1). Duas pecas de hardware que a base antiga nunca usou.
//
// (A prioridade da tela no barramento AXI NAO mora aqui: e do
// hal/tab5_bus, ligada no boot. Uma copia dela viveu aqui por algumas
// horas e brigou com a original.)
//
// 1. O FIM DO FRAME. O driver so troca de pagina quando o DMA do painel
//    reinicia (mipi_dsi_dma_trans_done_cb le cur_fb_index nesse instante), e
//    avisa por on_frame_buf_complete. Esperar esse aviso depois de pedir a
//    troca e o que garante que a pagina que saiu nao esta mais sendo lida -
//    so entao da para escrever nela sem rasgar a imagem.
//
// 2. COPIA POR HARDWARE (DMA2D puro, esp_async_fbcpy; o PPA so de reserva).
//    Copia um retangulo de uma pagina para a outra sem a CPU, em linha
//    corrida - 3x mais rapido que o motor SRM do PPA (medido: 'tela banco').
//    E como as areas mudadas de um frame sao levadas para a outra pagina.
//
// E um contador de UNDERRUN, porque sem medida "melhorou" e so opiniao.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"

// Retangulo em coordenadas FISICAS do framebuffer (retrato, 720x1280).
typedef struct {
  int16_t x, y, l, a;
} tab5_ret_t;

// Liga as tres pecas ao painel atual. Idempotente por handle: o repouso
// destroi e recria o painel, e a proxima chamada registra de novo.
bool tab5_quadro_iniciar(esp_lcd_panel_handle_t painel);

// Esquece o painel. Chamar ao destruir o painel (repouso).
void tab5_quadro_encerrar(void);

// Descarta um aviso de fim de frame que tenha chegado antes. Chamar LOGO
// DEPOIS de pedir a troca - ver o porque em tab5_frame.cpp.
void tab5_quadro_armar_espera(void);

// Espera o painel comecar a varrer a pagina nova. false = estourou o prazo.
bool tab5_quadro_esperar_troca(uint32_t prazo_ms);

// Copia o retangulo 'r' de 'origem' para o MESMO lugar de 'destino' (imagens
// 720x1280 RGB565: as paginas do painel e a tela virtual), por DMA2D.
// Bloqueia ate terminar.
bool tab5_quadro_copiar(void* destino, const void* origem, const tab5_ret_t& r);

// O mesmo, com ORIGEM em outro lugar: o retangulo 'r' do destino recebe o
// retangulo do mesmo tamanho que comeca em (ox, oy) na origem. E a rolagem
// pelo DMA: a pagina da frente, deslocada, vira a de tras. Origem e destino
// NAO podem ser o mesmo buffer (o DMA nao trata sobreposicao).
bool tab5_quadro_copiar_de(void* destino, const tab5_ret_t& r, const void* origem, int ox, int oy);

// Diagnostico.
uint32_t tab5_quadro_underruns(void);   // avisos de underrun desde o boot
uint32_t tab5_quadro_quadros(void);     // frames varridos pelo painel

// Frames que ATRASARAM (intervalo entre fins de frame 50% acima do periodo
// normal) desde o boot. pior_us = o maior intervalo desde a ultima chamada
// (e zera); periodo_us = o periodo normal medido.
uint32_t tab5_quadro_atrasos(uint32_t* pior_us, uint32_t* periodo_us);

// Quando (uptime, us) o painel terminou de varrer o ultimo frame. Com o
// periodo acima, da a FASE da varredura: quem anima pode se alinhar a ela
// (tela de bloqueio). 0 = painel ainda nao varreu nada.
int64_t tab5_quadro_ultimo_fim_us(void);

// O ultimo atraso: em que core a ISR do painel roda, e que tarefa estava
// rodando em cada core quando ela finalmente foi atendida (t0/t1: 16 bytes).
// Devolve um contador que sobe a cada atraso registrado.
uint32_t tab5_quadro_atraso_quem(int* nucleo_isr, char* t0, char* t1);
int64_t  tab5_quadro_atraso_quando(void);   // uptime (us) do ultimo atraso

// JANELA SEGURA PARA SECAO CRITICA LONGA. Entre dois frames o
// DMA do painel tem ~0,5 ms para ser religado pela ISR - e uma secao critica
// (interrupcoes desligadas no core) que caia nessa folga perde um frame
// inteiro. Foi a causa do "FRAME ATRASADO" com os dois cores em IDLE: a
// foto das tarefas do monitor (uxTaskGetSystemState, ~0,8 ms com a CPU a 360)
// rodava 1x/s em fase fixa e de tempos em tempos caia bem na folga - todos os
// atrasos tinham a mesma fracao de segundo (x,728 s).
//
// Esta funcao volta logo DEPOIS de um fim de frame (a ISR ja religou o DMA):
// dali ha ~14 ms ate a proxima folga. Quem vai ficar em secao critica por
// mais de ~100 us chama isto antes. Sem painel (repouso), volta no prazo.
bool tab5_quadro_esperar_fim(uint32_t prazo_ms);

// Erros do DSI host lidos a cada frame: devolve quantos frames tiveram
// algum bit desde o boot; st0/st1 = OU dos bits desde a ultima chamada (e zera).
uint32_t tab5_quadro_erros_dsi(uint32_t* st0, uint32_t* st1);

// PREENCHIMENTO POR HARDWARE (PPA, operacao FILL): pinta o retangulo 'r' de
// 'destino' (720x1280 RGB565) com 'cor' (RGB565 como fica na memoria), sem a
// CPU. Bloqueia ate terminar. false = indisponivel ou recusado - quem chamou
// pinta pela CPU. Na primeira vez confere o pixel escrito; se a cor nao
// bater (ordem de bytes), desliga sozinho e avisa no log.
bool tab5_quadro_preencher(void* destino, const tab5_ret_t& r, uint16_t cor);

// BANCO DE PROVA DA COPIA. Copia o mesmo retangulo entre dois buffers
// temporarios na PSRAM (alocados e devolvidos aqui) por cada motor
// disponivel - PPA (SRM), DMA2D puro, GDMA AXI e CPU - com o painel varrendo
// como sempre, e imprime ms e MB/s de cada um. E a medida que escolhe o motor
// da copia entre paginas. Chamar do console; leva ~1 s.
void tab5_quadro_banco(void);
