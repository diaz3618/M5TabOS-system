// AS DUAS PAGINAS DO PAINEL, e as primitivas of drawing em area.
//
// O panel MIPI-DSI of Tab5 varre one framebuffer in PSRAM sem stop. O
// driver of IDF ja aloca DOIS (num_fbs = 2 no Panel_DSI of M5GFX) e troca of
// page so mudando one indice (esp_lcd_panel_dpi.c:506-524) - a troca vale no
// next end of frame.
//
// Quem escreve nas paginas e SO o compositor (kernel/compositor.cpp). Todo o
// resto of sistema desenha in screen virtual (kernel/virtual_screen.cpp), que o
// panel nunca le. Ver COMPOSITOR.md.
//
// HISTORIA, para who estranhar o size: este modulo tinha
// ~800 lines of defesa contra write in page que o panel varre - faixas,
// respiros, double buffer so para repintura complete, scroll correndo contra
// o feixe. A causa of fundo era a banda of PSRAM pela half (line of cache
// L2 of 64B em vez of 128B) somada a draw in page visivel. As duas
// acabaram: o compositor desenha atras, e a line of cache e 128B.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <M5Unified.h>
#include "tab5_frame.hpp"

// Prepara as duas paginas. Idempotente: detecta sozinho when o panel foi
// recriado (o sleep destroi e remonta o DSI, e os framebuffers mudam).
bool tab5_screen_init(void);

// Libera o que foi mounted. Chamar ANTES of destruir o panel (sleep).
void tab5_screen_shutdown(void);

// EPOCA DAS PAGINAS: sobe sempre que as paginas deixam of valer (panel
// encerrado) ou nascem of novo. Le-se of graca - e o que deixa o compositor
// conferir "minha page ainda existe?" a cada escrita sem chamar nada.
extern volatile uint32_t g_tab5_screen_epoch;

// Sobe a cada panel novo. O compositor usa para saber que as paginas que ele
// conhecia morreram (e que a screen inteira precisa ir para as novas).
uint32_t tab5_screen_generation(void);

// ---------------------------------------------------------------------------
// PRIMITIVAS DO COMPOSITOR - ninguem mais deve usar
// ---------------------------------------------------------------------------
// Pagina of back (true) ou of frente (false).
void* tab5_screen_page(bool back);

// Pede a troca SEM wait. Quem chama espera com tab5_frame_wait_for_swap()
// antes of touch in page of back of novo. [y0, y1) limita o msync of driver.
bool tab5_screen_request_swap(int y0, int y1);

// ---------------------------------------------------------------------------
// DESENHO EM AREA, no alvo of sistema
// ---------------------------------------------------------------------------
// Para onde clear/scroll escrevem. O kernel aponta para a screen virtual no
// boot; nullptr = M5.Display (so se a screen virtual nao puder existir).
void tab5_screen_set_target(lgfx::LovyanGFX* d);

// Pinta one rectangle of uma color. Dispensa o work when a area ja esta
// sabidamente nessa color (ver tab5_screen_mark_clean) - who escreve app pede
// limpeza sempre, sem condicao; a decisao e tomada aqui.
void tab5_screen_clear(int x, int y, int width, int height, uint16_t color);

// PARA O SISTEMA: "este rectangle acabou of ficar inteiro desta color". Vale
// durante UMA pass of drawing. Evita a limpeza dobrada (o sistema limpa a
// area util e em seguida o app pede a mesma limpeza).
void tab5_screen_mark_clean(int x, int y, int width, int height, uint16_t color);
void tab5_screen_forget_clean(void);
uint32_t tab5_screen_avoided_clears(void);   // diagnostics

// Desloca o content of uma area 'delta' lines (>0 sobe, <0 desce) e pinta
// com 'color' a tira exposta. Reaproveitar pixel ja drawn poupa CPU numa
// scroll. false = |delta| >= height, nada a reaproveitar.
bool tab5_screen_scroll(int x, int y, int width, int height, int delta, uint16_t color);

// Diagnostico: quanto a scroll custou a CPU since o boot - pixels deslocados
// pelo copyRect e o time gasto nisso (us). Acumulados, nunca zerados: who
// mede guarda uma foto e faz a diferenca.
void tab5_screen_scroll_cost(uint64_t* px, uint64_t* us);

// OBSOLETA - nao faz nada. Era o "orcamento" of escrita para
// respirar entre rajadas in page visivel. Fica so porque o app Cobra (aba of
// jogo) ainda chama; exit when ele deixar of chamar.
static inline void tab5_screen_spent(size_t) { }
