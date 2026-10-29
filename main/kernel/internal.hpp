// PRIVATE BRIDGE between core.cpp and shell.cpp - NEVER included by one app.
//
// ------------------------------------------------------------------------
// WHY DOES THIS EXIST (Step 1 of docs/ARCHITECTURE.md)
// ------------------------------------------------------------------------
// system.cpp had grown from 1129 to 2457 lines doing eight things
// different, sharing statics without border none - "the file
// where the next bug will hide", in the words of the document itself.
// It was split in two, WITHOUT CHANGING A LINE OF LOGIC:
//
// core.cpp o ENGINE - agenda of drawing, life cycle of apps
// (open/close), energy policy. Decide WHAT runs and
// WHEN.
// shell.cpp a APPEARANCE - initial screen, status bar, routing of
// touch to the right app, calibration of touch. And "shell" in
// sense of OS (Windows Shell, GNOME Shell): the visual layer
// in app iteration. It is NOT the same "shell" of console.cpp (the
// serial command line interface) - two concepts
// different, two different files, unrelated.
//
// The ONLY exception to "zero change of logic": gerar_toque() (now in
// shell.cpp) called the PRIVATE function area_util() to get the geometry of
// usable area; it became the call tosistema_area() (same thing, publish -
// system_area() and literally "return area_util();"). Swap for this
// avoids exposing an entire function just for one call, without changing any
// result.
//
// app.h remains the ONLY contract that an app sees. This header and
// deliberately cruder than it - extern variables instead of getters -
// because the goal here is not a pretty API, and not duplicating state between
// two files.
#pragma once

#include "app.h"
#include "scroll.hpp"

// -------------------------------------------------------------------------------
// SHARED GEOMETRY - used by core.cpp (area_util(), calculations of
// touch) and shell.cpp (drawing of bar, of home, of navigation).
// -------------------------------------------------------------------------------
static const int ALTURA_BARRA = 64;
static const int ALTURA_NAV   = 72;   // era 96 - too loud with soft font

// VERTICAL TOUCH CORRECTION (the finger slopes below where the eye looks) - the
// because it is in shell.cpp, next to gerar_toque(). Lives here because BOTH
// paths of touch need it: click (shell.cpp) and touch per level
// (core.cpp). Before, only the click corrected it, and the virtual keyboard - which decides the
// key by level - hit 10 px lower than the same finger hit a button.
static const int TOQUE_DY = 10;

// -------------------------------------------------------------------------------
// BOOT SPLASH - turns on/off both files at the same time (the
// DEFINITION lives in shell.cpp, the CALL in core.cpp). View original warning
// along withdraw_splash() in shell.cpp for why it exists.
// -------------------------------------------------------------------------------
#define TAB5_MOSTRAR_SPLASH

// -------------------------------------------------------------------------------
// SHARED STATE - defined in core.cpp, read (never written) by
// shell.cpp.
// -------------------------------------------------------------------------------
extern const app_t* s_app;             // NULL = initial screen
extern const app_t* s_app_loading;  // app with mounting of SD pending
extern const app_t* s_app_recusado;    // app that the system did not open (screen of reason in place)
extern scroll_t    s_rol;             // scrolling of app open - mechanism in kernel/scroll.hpp
extern bool         s_dirty_layout;     // TELA changed - requires complete repainting
extern bool         s_dirty_data;      // so one VALOR changed - requests incremental repainting
extern bool         s_nav_travada;     // navigation stuck (keyboard/screenshot)
extern bool         s_locked;       // screen of blocking in front of everything (home or app are at the bottom)
extern bool         s_panel_notices;   // center of notices open by top of app/home

// THE APP IS HIDDEN behind something in the system (lock or central
// notices). As long as it is true, it does not receive touch, key or warning - no
// is visible, and an app that "reads" what it shows (conversation) would mark it as read
// what no one saw. Just ONE rule, so as not to forget one of the two at any time
// of the input paths.
static inline bool system_app_covered(void) { return s_locked || s_panel_notices; }

// THERE IS AN APP REALLY OPEN: ao_abrir() has already run. Not even "opening..." (SD
// mounting) nor refused (screen of reason of system) - in these two the app does not
// has no state, and none of its functions can be called. ONE rule only,
// for all paths that call the app.
static inline bool app_vivo(void) { return s_app && s_app != s_app_loading && s_app != s_app_recusado; }

// -------------------------------------------------------------------------------
// FUNCTIONS FROM core.cpp, called by shell.cpp (app lifecycle).
// -------------------------------------------------------------------------------
// Who called this running in interface task?
bool system_in_interface(void);

// Change the clock of panel (destroys and recreates the pages) IN THE TASK OF
// INTERFACE - of another task, leave the request and wait up to 5 s.
bool system_switch_clock_panel(int mhz);

// COMPLETE PASS BY APP (open app, return to home), accumulated since the
// boot, to the 'screen' of console. The three phases add up to an approximate total:
// bar of status, cleaning of area (+ bar of navigation) and content
// (the draw() of app, or the home).
typedef struct {
  const char* name;
  uint32_t n;
  uint32_t max_us;
  uint64_t us, us_barra, us_limpar, us_content;
} system_pass_t;
int system_passes_complete(system_pass_t* out, int max);

bool open_app(const app_t* app);
void close_app(void);

// -------------------------------------------------------------------------------
// FUNCTIONS FROM shell.cpp, called by core.cpp (drawing and the main loop).
// -------------------------------------------------------------------------------
#ifdef TAB5_MOSTRAR_SPLASH
void draw_splash(void);
#endif
void tratar_touch(int tx, int ty);
void draw_barra_status(lgfx::LovyanGFX& d, bool complete);
bool barra_changed(void);
void draw_barra_nav(lgfx::LovyanGFX& d);
void draw_home(lgfx::LovyanGFX& d, const area_t& a);
// HOME PAGES. How many lines of icons fit per page; with more apps
// this means that the home page gets dots at the bottom and the side drag changes the page.
// 4 = everything that the area supports today (12 apps). Download to 3 to test
// second page with apps that already exist.
#define TAB5_HOME_LINHAS_MAX 4

// GESTURES: called in release of finger with the total offset (dx, dy) since a
// press_start, the finger that woke up the screen and the common click have already been discarded. At home
// change of page; With lock enabled, drag to top unlocks.
void tratar_gesto(int dx, int dy);

// MOVE ICON ON HOME. home_segurar() is called every iteration with finger in
// screen (home open, without blocking): after HOME_SEGURAR_MS stopped on one
// icon he "gets up" and starts to follow the finger; returns true while there is icon
// raised, and then the touch does not become a click or a gesture. home_soltar() in
// release writes the new order. home_ordem_carr() once, before of laco.
bool home_segurar(int tx, int ty, uint32_t now, uint32_t segurando_ms, int desvio);
bool home_soltar(void);
void home_ordem_carregar(void);

// BLOCK. draw_bloqueio() paints the entire useful area (complete) or just the
// that changed since the last time (clock, warning); the top status bar
// continues to of always. blocking_screen()/unblocking_screen() only change the state
// and ask for complete repainting.
void draw_bloqueio(lgfx::LovyanGFX& d, const area_t& a, bool complete);
void bloquear_screen(void);
// THE SCREEN HAS JUST TURNED ON (boot or wake up): the ONLY rule of when a
// lock screen appears. Setting enabled and not yet blocked: blocks;
// already blocked: draws another animation. Whoever turns on the screen calls this, and that's it.
void lockscreen_on_enable_screen(void);
void desbloquear_screen(void);

// NOTICE CENTER (shell.cpp). Tapping the status bar opens a list of
// ALL active sources (notif_listar), one line per app, per top of area
// useful; touch a line opens its app, touch outside (or the new bar)
// closes. panel_avisos_mudou() says on loop that the open list has aged.
void panel_notices_open(bool open);
bool panel_notices_changed(void);
void draw_panel_notices(lgfx::LovyanGFX& d, const area_t& a, bool complete);

void atualizar_line_servicos(void);   // tests and redesigns the track GROVE/CARTAO of home, if changed

// Diagnosis of touch enabled via console ('touch') - kernel/shell.cpp.
bool diag_touch_enabled(void);
