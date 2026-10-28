// HAL DE FRAME - a camada mais low of nova base of screen (COMPOSITOR.md,
// etapa 1). Duas pecas of hardware que a base antiga nunca usou.
//
// (A priority of screen no barramento AXI NAO mora aqui: e of
// hal/tab5_bus, enabled no boot. Uma copia dela viveu aqui por algumas
// horas e brigou com a original.)
//
// 1. O FIM DO FRAME. O driver so troca of page when o DMA of panel
//    reinicia (mipi_dsi_dma_trans_done_cb le cur_fb_index nesse instante), e
//    avisa por on_frame_buf_complete. Esperar esse aviso depois of request a
//    troca e o que garante que a page que exited nao esta mais sendo lida -
//    so entao of para write nela sem rasgar a imagem.
//
// 2. COPIA POR HARDWARE (DMA2D puro, esp_async_fbcpy; o PPA so of reserva).
//    Copia one rectangle of uma page para a outra sem a CPU, em line
//    corrida - 3x mais rapido que o motor SRM of PPA (measured: 'screen banco').
//    E como as areas mudadas of one frame sao levadas para a outra page.
//
// E one contador of UNDERRUN, porque sem measurement "melhorou" e so opiniao.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"

// Retangulo em coordenadas FISICAS of framebuffer (retrato, 720x1280).
typedef struct {
  int16_t x, y, l, a;
} tab5_rect_t;

// Liga as tres pecas on panel current. Idempotente por handle: o sleep
// destroi e recria o panel, e a next chamada registra of novo.
bool tab5_frame_init(esp_lcd_panel_handle_t panel);

// Esquece o panel. Chamar on destruir o panel (sleep).
void tab5_frame_shutdown(void);

// Descarta one aviso of end of frame que tenha chegado antes. Chamar LOGO
// DEPOIS of request a troca - ver o porque em tab5_frame.cpp.
void tab5_frame_arm_wait(void);

// Espera o panel comecar a scan a page nova. false = estourou o prazo.
bool tab5_frame_wait_for_swap(uint32_t timeout_ms);

// Copia o rectangle 'r' of 'source' para o MESMO lugar of 'target' (imagens
// 720x1280 RGB565: as paginas of panel e a screen virtual), por DMA2D.
// Bloqueia ate terminar.
bool tab5_frame_copy(void* target, const void* source, const tab5_rect_t& r);

// O mesmo, com ORIGEM em outro lugar: o rectangle 'r' of target recebe o
// rectangle of mesmo size que comeca em (ox, oy) in source. E a scroll
// pelo DMA: a page of frente, deslocada, vira a of back. Origem e target
// NAO podem ser o mesmo buffer (o DMA nao trata sobreposicao).
bool tab5_frame_copy_from(void* target, const tab5_rect_t& r, const void* source, int ox, int oy);

// Diagnostico.
uint32_t tab5_frame_underruns(void);   // notices of underrun since o boot
uint32_t tab5_frame_count(void);     // frames varridos pelo panel

// Frames que ATRASARAM (intervalo entre fins of frame 50% acima of periodo
// normal) since o boot. worst_us = o maior intervalo since a latest chamada
// (e zera); period_us = o periodo normal measured.
uint32_t tab5_frame_delays(uint32_t* worst_us, uint32_t* period_us);

// Quando (uptime, us) o panel terminou of scan o ultimo frame. Com o
// periodo acima, of a FASE of varredura: who anima pode se alinhar a ela
// (screen of bloqueio). 0 = panel ainda nao varreu nada.
int64_t tab5_frame_last_end_us(void);

// O ultimo atraso: em que core a ISR of panel roda, e que tarefa estava
// running em cada core when ela finalmente foi atendida (t0/t1: 16 bytes).
// Devolve one contador que sobe a cada atraso registrado.
uint32_t tab5_frame_delay_source(int* isr_core, char* t0, char* t1);
int64_t  tab5_frame_delay_time(void);   // uptime (us) of ultimo atraso

// JANELA SEGURA PARA SECAO CRITICA LONGA. Entre dois frames o
// DMA of panel tem ~0,5 ms para ser religado pela ISR - e uma secao critica
// (interrupcoes desligadas no core) que caia nessa folga perde one frame
// inteiro. Foi a causa of "FRAME ATRASADO" com os dois cores em IDLE: a
// foto das tarefas of monitor (uxTaskGetSystemState, ~0,8 ms com a CPU a 360)
// rodava 1x/s em fase fixa e of tempos em tempos caia bem in folga - todos os
// atrasos tinham a mesma fracao of segundo (x,728 s).
//
// Esta funcao iteration logo DEPOIS of one end of frame (a ISR ja religou o DMA):
// dali ha ~14 ms ate a next folga. Quem vai ficar em secao critica por
// mais of ~100 us chama isto antes. Sem panel (sleep), iteration no prazo.
bool tab5_frame_wait_for_end(uint32_t timeout_ms);

// Erros of DSI host lidos a cada frame: devolve quantos frames tiveram
// algum bit since o boot; st0/st1 = OU dos bits since a latest chamada (e zera).
uint32_t tab5_frame_dsi_errors(uint32_t* st0, uint32_t* st1);

// PREENCHIMENTO POR HARDWARE (PPA, operacao FILL): pinta o rectangle 'r' of
// 'target' (720x1280 RGB565) com 'color' (RGB565 como stays in memoria), sem a
// CPU. Bloqueia ate terminar. false = indisponivel ou recusado - who chamou
// pinta pela CPU. Na primeira vez confere o pixel escrito; se a color nao
// bater (order of bytes), desliga sozinho e avisa no log.
bool tab5_frame_fill(void* target, const tab5_rect_t& r, uint16_t color);

// BANCO DE PROVA DA COPIA. Copia o mesmo rectangle entre dois buffers
// temporarios in PSRAM (alocados e devolvidos aqui) por cada motor
// disponivel - PPA (SRM), DMA2D puro, GDMA AXI e CPU - com o panel scanning
// como sempre, e imprime ms e MB/s of cada one. E a measurement que escolhe o motor
// of copia entre paginas. Chamar of console; leva ~1 s.
void tab5_frame_benchmark(void);
