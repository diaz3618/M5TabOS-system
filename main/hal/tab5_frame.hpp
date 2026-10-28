// HAL DE FRAME - a camada mais low da nova base de tela (COMPOSITOR.md,
// etapa 1). Duas pecas de hardware que a base antiga nunca usou.
//
// (A prioridade da tela no barramento AXI NAO mora aqui: e do
// hal/tab5_bus, ligada no boot. Uma copia dela viveu aqui por algumas
// horas e brigou com a original.)
//
// 1. O FIM DO FRAME. O driver so troca de page quando o DMA do panel
//    reinicia (mipi_dsi_dma_trans_done_cb le cur_fb_index nesse instante), e
//    avisa por on_frame_buf_complete. Esperar esse aviso depois de pedir a
//    troca e o que garante que a page que saiu nao esta mais sendo lida -
//    so entao da para escrever nela sem rasgar a imagem.
//
// 2. COPIA POR HARDWARE (DMA2D puro, esp_async_fbcpy; o PPA so de reserva).
//    Copia um retangulo de uma page para a outra sem a CPU, em linha
//    corrida - 3x mais rapido que o motor SRM do PPA (medido: 'tela banco').
//    E como as areas mudadas de um frame sao levadas para a outra page.
//
// E um contador de UNDERRUN, porque sem measurement "melhorou" e so opiniao.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"

// Retangulo em coordenadas FISICAS do framebuffer (retrato, 720x1280).
typedef struct {
  int16_t x, y, l, a;
} tab5_rect_t;

// Liga as tres pecas ao panel atual. Idempotente por handle: o sleep
// destroi e recria o panel, e a proxima chamada registra de novo.
bool tab5_frame_init(esp_lcd_panel_handle_t panel);

// Esquece o panel. Chamar ao destruir o panel (sleep).
void tab5_frame_shutdown(void);

// Descarta um aviso de end de frame que tenha chegado antes. Chamar LOGO
// DEPOIS de pedir a troca - ver o porque em tab5_frame.cpp.
void tab5_frame_arm_wait(void);

// Espera o panel comecar a scan a page nova. false = estourou o prazo.
bool tab5_frame_wait_for_swap(uint32_t timeout_ms);

// Copia o retangulo 'r' de 'source' para o MESMO lugar de 'target' (imagens
// 720x1280 RGB565: as paginas do panel e a tela virtual), por DMA2D.
// Bloqueia ate terminar.
bool tab5_frame_copy(void* target, const void* source, const tab5_rect_t& r);

// O mesmo, com ORIGEM em outro lugar: o retangulo 'r' do target recebe o
// retangulo do mesmo size que comeca em (ox, oy) na source. E a scroll
// pelo DMA: a page da frente, deslocada, vira a de back. Origem e target
// NAO podem ser o mesmo buffer (o DMA nao trata sobreposicao).
bool tab5_frame_copy_from(void* target, const tab5_rect_t& r, const void* source, int ox, int oy);

// Diagnostico.
uint32_t tab5_frame_underruns(void);   // avisos de underrun desde o boot
uint32_t tab5_frame_count(void);     // frames varridos pelo panel

// Frames que ATRASARAM (intervalo entre fins de frame 50% acima do periodo
// normal) desde o boot. worst_us = o maior intervalo desde a latest chamada
// (e zera); period_us = o periodo normal medido.
uint32_t tab5_frame_delays(uint32_t* worst_us, uint32_t* period_us);

// Quando (uptime, us) o panel terminou de scan o ultimo frame. Com o
// periodo acima, da a FASE da varredura: who anima pode se alinhar a ela
// (tela de bloqueio). 0 = panel ainda nao varreu nada.
int64_t tab5_frame_last_end_us(void);

// O ultimo atraso: em que core a ISR do panel roda, e que tarefa estava
// rodando em cada core quando ela finalmente foi atendida (t0/t1: 16 bytes).
// Devolve um contador que sobe a cada atraso registrado.
uint32_t tab5_frame_delay_source(int* isr_core, char* t0, char* t1);
int64_t  tab5_frame_delay_time(void);   // uptime (us) do ultimo atraso

// JANELA SEGURA PARA SECAO CRITICA LONGA. Entre dois frames o
// DMA do panel tem ~0,5 ms para ser religado pela ISR - e uma secao critica
// (interrupcoes desligadas no core) que caia nessa folga perde um frame
// inteiro. Foi a causa do "FRAME ATRASADO" com os dois cores em IDLE: a
// foto das tarefas do monitor (uxTaskGetSystemState, ~0,8 ms com a CPU a 360)
// rodava 1x/s em fase fixa e de tempos em tempos caia bem na folga - todos os
// atrasos tinham a mesma fracao de segundo (x,728 s).
//
// Esta funcao volta logo DEPOIS de um end de frame (a ISR ja religou o DMA):
// dali ha ~14 ms ate a proxima folga. Quem vai ficar em secao critica por
// mais de ~100 us chama isto antes. Sem panel (sleep), volta no prazo.
bool tab5_frame_wait_for_end(uint32_t timeout_ms);

// Erros do DSI host lidos a cada frame: devolve quantos frames tiveram
// algum bit desde o boot; st0/st1 = OU dos bits desde a latest chamada (e zera).
uint32_t tab5_frame_dsi_errors(uint32_t* st0, uint32_t* st1);

// PREENCHIMENTO POR HARDWARE (PPA, operacao FILL): pinta o retangulo 'r' de
// 'target' (720x1280 RGB565) com 'color' (RGB565 como fica na memoria), sem a
// CPU. Bloqueia ate terminar. false = indisponivel ou recusado - who chamou
// pinta pela CPU. Na primeira vez confere o pixel escrito; se a color nao
// bater (ordem de bytes), desliga sozinho e avisa no log.
bool tab5_frame_fill(void* target, const tab5_rect_t& r, uint16_t color);

// BANCO DE PROVA DA COPIA. Copia o mesmo retangulo entre dois buffers
// temporarios na PSRAM (alocados e devolvidos aqui) por cada motor
// disponivel - PPA (SRM), DMA2D puro, GDMA AXI e CPU - com o panel scanning
// como sempre, e imprime ms e MB/s de cada um. E a measurement que escolhe o motor
// da copia entre paginas. Chamar do console; leva ~1 s.
void tab5_frame_benchmark(void);
