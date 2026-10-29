// COMPOSITOR - the basis of screen of system (docs/COMPOSITOR.md).
//
// DIRECT MODE (the "direct mode" of LVGL with two pages). Every drawing falls
// DIRECT on page behind of panel - there is no longer a third image
// in middle. The object of drawing (kernel/virtual_screen) annotates each rectangle
// touched (comp_invalidar), and the frame has three times:
//
// 1. OPEN (lazy, no first drawing of iteration - comp_abrir):
// waits for the previous frame exchange to finish (only then the back page
// is free). The areas that the previous frame drew are PENDING:
// each one comes from the front so when something is written or read in it
// (comp_preparar), and the rest on present. What is a copy of DMA
// entire overwrite (scroll) is never copied.
// 2. DRAW on the back page, as always.
// 3. PRESENT (comp_present, once per iteration): returns on cache as
// lines drawn and requests a change. DO NOT wait - who waits is the
// next OPEN.
//
// Each changed pixel passes through PSRAM 3 times (the CPU writes; the synchronization reads and
// writes once), and not 5 times like with the virtual screen. And without the virtual screen
// 1.84 MB of PSRAM remains.
//
// Physical coordinates (portrait). Call so of task of interface.

#pragma once

#include <M5Unified.h>
#include "tab5_frame.hpp"

// Marks one rectangle as drawn in this frame.
void comp_invalidar(const tab5_rect_t& r);

// Requests the entire screen in the next frame. Can be called from ANY task
// (the console uses it): only sets one request that the interface responds to when presenting.
void comp_invalidar_tudo(void);

// Opens the frame, if it is not already open, and returns the page behind
// (where draw). nullptr = no panel (sleep, change of clock).
void* comp_abrir(void);

// Before writing OR READing 'r' in front page: brings what the
// previous frame changed there and the one behind still doesn't have it (lazy sync).
// The tracker calls this itself; no one needs it anymore.
void comp_preparar(const tab5_rect_t& r);

// The same, for those who will write 'r' WHOLE and opaque (fillRect, copy of
// DMA): what was pending inside 'r' and discarded without copying - would be
// superscript. Only what stays outside of 'r' remains pending.
void comp_cobrir(const tab5_rect_t& r);

// Has the 'src' rectangle already been drawn IN THIS frame? (Scrolling only copies
// page of front what the front has equal to the back.)
bool comp_desenhado_neste_quadro(const tab5_rect_t& src);

// Takes the rectangle 'r' of page of FRONT, from (ox, oy), to the
// same size in 'r' in back, by DMA. Open the frame. false = did not work
// (whoever called does so via the CPU).
bool comp_copiar_da_frente(const tab5_rect_t& r, int ox, int oy);

// Takes the rectangle 'r' of a 720x1280 RGB565 image (a surface of DMA
// of one app - camera) to the back page, by DMA, and marks it.
bool comp_copiar_imagem(const void* imagem, const tab5_rect_t& r);

// Displays the frame, if there is something drawn (false = nothing to do, or without
// panel - in this case the frame is discarded).
bool comp_apresentar(void);

// Is there something drawn waiting to present?
bool comp_tem_sujeira(void);

// Waits for the requested exchange for the last frame to actually happen. For whom
// needs to frame ON THE SCREEN before following (turning on the wake light).
void comp_esperar_troca(void);

// THE PANEL IS NEW (boot, sleep, change of clock): both pages
// were born with nothing of ours, and there is no where to synchronize. Returns true ONCE
// by new panel - the core responds with one redraw complete. Call only with
// the panel in the air (out of sleep).
bool comp_painel_novo(void);

// Counts how many times the list of areas has been consumed. Who keeps "ja
// invalidated this" uses to know that the list is new.
uint32_t comp_rodada(void);

// Diagnosis: uptime (us) of last mounted frame and how many pixels it took.
void comp_ultimo_quadro(int64_t* when_us, uint32_t* px);

// -------------------------------------------------------------------------------
// MEASUREMENT - the cost of each frame, to compare before and after fiddling.
// -------------------------------------------------------------------------------
// Everything ACCUMULATED since boot and never reset: the console and monitor read on
// same team, and each one keeps their photo and makes a difference. So none
// reader erases the window of another.
//
// Histogram of cost of VOLTA that generated frame (of wake of interface until
// the requested change), in PANEL PERIODS: fit in 1 sweep, in 2, in 3,
// or more. It is the number that decides the fluidity - the iteration that passes 1 period
// leaves the previous frame on screen plus one scan.
#define COMP_FAIXAS 4

typedef struct {
  uint32_t frames;                 // tables presented
  uint32_t cheios;                // frames that changed >= 90% of screen
  uint32_t estouros;              // times the list of areas filled
  uint64_t px_frame;             // drawn pixels (displayed areas)
  uint64_t px_sinc;               // pixels of sync (front -> back)
  uint64_t px_rolar_dma;          // pixels of scrolling copied by DMA
  uint64_t us_copia;              // time locked in DMA copy (all)
  uint64_t us_espera;             // time waiting for previous frame exchange
  uint64_t us_drawing;            // CPU drawing (informed by the core, without copies)
  uint32_t n_drawing;             // passes of drawing measurements
  uint64_t us_drawing_complete;   // ...only COMPLETE passes (open app, home)
  uint32_t n_drawing_complete;
  uint64_t us_iteration;              // entire turns that generated frame
  uint32_t voltas[COMP_FAIXAS];   // histogram of cost of these turns, in periods
  uint64_t px_rolados;            // full scrolling (hal/tab5_screen: DMA or CPU)
  uint64_t us_rolar;
} comp_estat_t;

void comp_estat(comp_estat_t* out);

// The core measures the pass of drawing between comp_marcar_desenho() and
// comp_contar_desenho(us); the time that the composer himself spent on this medium
// (open, synchronize, copy) and discounted - "drawing" and just the CPU painting.
void comp_marcar_desenho(void);
// 'complete' = the pass redraws the entire screen (open/close app).
// Returns the time of CPU of pass (us, without the time of composer).
uint32_t comp_contar_desenho(uint32_t us, bool complete);
void comp_contar_volta(uint32_t us);
