// AS DUAS PAGINAS DO PAINEL, e as primitivas de desenho em area.
//
// O painel MIPI-DSI do Tab5 varre um framebuffer na PSRAM sem parar. O
// driver do IDF ja aloca DOIS (num_fbs = 2 no Panel_DSI do M5GFX) e troca de
// pagina so mudando um indice (esp_lcd_panel_dpi.c:506-524) - a troca vale no
// proximo fim de frame.
//
// Quem escreve nas paginas e SO o compositor (kernel/compositor.cpp). Todo o
// resto do sistema desenha na tela virtual (kernel/virtual_screen.cpp), que o
// painel nunca le. Ver COMPOSITOR.md.
//
// HISTORIA, para quem estranhar o tamanho: este modulo tinha
// ~800 linhas de defesa contra escrever na pagina que o painel varre - faixas,
// respiros, double buffer so para repintura completa, rolagem correndo contra
// o feixe. A causa de fundo era a banda de PSRAM pela metade (linha de cache
// L2 de 64B em vez de 128B) somada a desenhar na pagina visivel. As duas
// acabaram: o compositor desenha atras, e a linha de cache e 128B.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <M5Unified.h>
#include "tab5_frame.hpp"

// Prepara as duas paginas. Idempotente: detecta sozinho quando o painel foi
// recriado (o repouso destroi e remonta o DSI, e os framebuffers mudam).
bool tab5_tela_iniciar(void);

// Libera o que foi montado. Chamar ANTES de destruir o painel (repouso).
void tab5_tela_encerrar(void);

// EPOCA DAS PAGINAS: sobe sempre que as paginas deixam de valer (painel
// encerrado) ou nascem de novo. Le-se de graca - e o que deixa o compositor
// conferir "minha pagina ainda existe?" a cada escrita sem chamar nada.
extern volatile uint32_t g_tab5_tela_epoca;

// Sobe a cada painel novo. O compositor usa para saber que as paginas que ele
// conhecia morreram (e que a tela inteira precisa ir para as novas).
uint32_t tab5_tela_geracao(void);

// ---------------------------------------------------------------------------
// PRIMITIVAS DO COMPOSITOR - ninguem mais deve usar
// ---------------------------------------------------------------------------
// Pagina de tras (true) ou da frente (false).
void* tab5_tela_pagina(bool tras);

// Pede a troca SEM esperar. Quem chama espera com tab5_quadro_esperar_troca()
// antes de tocar na pagina de tras de novo. [y0, y1) limita o msync do driver.
bool tab5_tela_pedir_troca(int y0, int y1);

// ---------------------------------------------------------------------------
// DESENHO EM AREA, no alvo do sistema
// ---------------------------------------------------------------------------
// Para onde limpar/rolar escrevem. O kernel aponta para a tela virtual no
// boot; nullptr = M5.Display (so se a tela virtual nao puder existir).
void tab5_tela_definir_alvo(lgfx::LovyanGFX* d);

// Pinta um retangulo de uma cor. Dispensa o trabalho quando a area ja esta
// sabidamente nessa cor (ver tab5_tela_marcar_limpo) - quem escreve app pede
// limpeza sempre, sem condicao; a decisao e tomada aqui.
void tab5_tela_limpar(int x, int y, int largura, int altura, uint16_t cor);

// PARA O SISTEMA: "este retangulo acabou de ficar inteiro desta cor". Vale
// durante UMA passada de desenho. Evita a limpeza dobrada (o sistema limpa a
// area util e em seguida o app pede a mesma limpeza).
void tab5_tela_marcar_limpo(int x, int y, int largura, int altura, uint16_t cor);
void tab5_tela_esquecer_limpo(void);
uint32_t tab5_tela_limpezas_evitadas(void);   // diagnostico

// Desloca o conteudo de uma area 'delta' linhas (>0 sobe, <0 desce) e pinta
// com 'cor' a tira exposta. Reaproveitar pixel ja desenhado poupa CPU numa
// rolagem. false = |delta| >= altura, nada a reaproveitar.
bool tab5_tela_rolar(int x, int y, int largura, int altura, int delta, uint16_t cor);

// Diagnostico: quanto a rolagem custou a CPU desde o boot - pixels deslocados
// pelo copyRect e o tempo gasto nisso (us). Acumulados, nunca zerados: quem
// mede guarda uma foto e faz a diferenca.
void tab5_tela_rolagem_custo(uint64_t* px, uint64_t* us);

// OBSOLETA - nao faz nada. Era o "orcamento" de escrita para
// respirar entre rajadas na pagina visivel. Fica so porque o app Cobra (aba do
// jogo) ainda chama; sai quando ele deixar de chamar.
static inline void tab5_tela_gastou(size_t) { }
