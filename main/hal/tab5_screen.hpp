// AS DUAS PAGINAS DO PAINEL, e as primitivas de desenho em area.
//
// O panel MIPI-DSI do Tab5 varre um framebuffer na PSRAM sem stop. O
// driver do IDF ja aloca DOIS (num_fbs = 2 no Panel_DSI do M5GFX) e troca de
// page so mudando um indice (esp_lcd_panel_dpi.c:506-524) - a troca vale no
// proximo end de frame.
//
// Quem escreve nas paginas e SO o compositor (kernel/compositor.cpp). Todo o
// resto do sistema desenha na tela virtual (kernel/virtual_screen.cpp), que o
// panel nunca le. Ver COMPOSITOR.md.
//
// HISTORIA, para who estranhar o size: este modulo tinha
// ~800 linhas de defesa contra escrever na page que o panel varre - faixas,
// respiros, double buffer so para repintura completa, scroll correndo contra
// o feixe. A causa de fundo era a banda de PSRAM pela half (linha de cache
// L2 de 64B em vez de 128B) somada a desenhar na page visivel. As duas
// acabaram: o compositor desenha atras, e a linha de cache e 128B.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <M5Unified.h>
#include "tab5_frame.hpp"

// Prepara as duas paginas. Idempotente: detecta sozinho quando o panel foi
// recriado (o sleep destroi e remonta o DSI, e os framebuffers mudam).
bool tab5_screen_init(void);

// Libera o que foi mounted. Chamar ANTES de destruir o panel (sleep).
void tab5_screen_shutdown(void);

// EPOCA DAS PAGINAS: sobe sempre que as paginas deixam de valer (panel
// encerrado) ou nascem de novo. Le-se de graca - e o que deixa o compositor
// conferir "minha page ainda existe?" a cada escrita sem chamar nada.
extern volatile uint32_t g_tab5_screen_epoch;

// Sobe a cada panel novo. O compositor usa para saber que as paginas que ele
// conhecia morreram (e que a tela inteira precisa ir para as novas).
uint32_t tab5_screen_generation(void);

// ---------------------------------------------------------------------------
// PRIMITIVAS DO COMPOSITOR - ninguem mais deve usar
// ---------------------------------------------------------------------------
// Pagina de back (true) ou da frente (false).
void* tab5_screen_page(bool back);

// Pede a troca SEM esperar. Quem chama espera com tab5_frame_wait_for_swap()
// antes de tocar na page de back de novo. [y0, y1) limita o msync do driver.
bool tab5_screen_request_swap(int y0, int y1);

// ---------------------------------------------------------------------------
// DESENHO EM AREA, no alvo do sistema
// ---------------------------------------------------------------------------
// Para onde clear/scroll escrevem. O kernel aponta para a tela virtual no
// boot; nullptr = M5.Display (so se a tela virtual nao puder existir).
void tab5_screen_set_target(lgfx::LovyanGFX* d);

// Pinta um retangulo de uma color. Dispensa o trabalho quando a area ja esta
// sabidamente nessa color (ver tab5_screen_mark_clean) - who escreve app pede
// limpeza sempre, sem condicao; a decisao e tomada aqui.
void tab5_screen_clear(int x, int y, int width, int height, uint16_t color);

// PARA O SISTEMA: "este retangulo acabou de ficar inteiro desta color". Vale
// durante UMA passada de desenho. Evita a limpeza dobrada (o sistema limpa a
// area util e em seguida o app pede a mesma limpeza).
void tab5_screen_mark_clean(int x, int y, int width, int height, uint16_t color);
void tab5_screen_forget_clean(void);
uint32_t tab5_screen_avoided_clears(void);   // diagnostico

// Desloca o conteudo de uma area 'delta' linhas (>0 sobe, <0 desce) e pinta
// com 'color' a tira exposta. Reaproveitar pixel ja desenhado poupa CPU numa
// scroll. false = |delta| >= height, nada a reaproveitar.
bool tab5_screen_scroll(int x, int y, int width, int height, int delta, uint16_t color);

// Diagnostico: quanto a scroll custou a CPU desde o boot - pixels deslocados
// pelo copyRect e o time gasto nisso (us). Acumulados, nunca zerados: who
// mede guarda uma foto e faz a diferenca.
void tab5_screen_scroll_cost(uint64_t* px, uint64_t* us);

// OBSOLETA - nao faz nada. Era o "orcamento" de escrita para
// respirar entre rajadas na page visivel. Fica so porque o app Cobra (aba do
// jogo) ainda chama; sai quando ele deixar de chamar.
static inline void tab5_screen_spent(size_t) { }
