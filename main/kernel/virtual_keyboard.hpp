// Virtual keyboard of SYSTEM.
//
// One component for every app that needs text: the conversation (Mesh,
// ESP-NOW), the password of Wi-Fi, the PIN of Bluetooth. The app does not draw key,
// doesn't treat fingers and doesn't stop navigation - it just:
//
// static tecvirt_t s_kb;
// open: tecvirt_abrir(&s_kb);                (START locks)
// close: tecvirt_close(&s_kb);               (also in ao_close of app)
// draw: tecvirt_posicionar(&s_kb, a);        (the area, on SCREEN, at whose base it opens)
// tecvirt_pintar(&s_kb, d, complete);  (TODA pass: is cheap when nothing changes)
// level: n = tecvirt_nivel(&s_kb, actions, 8);  (in ao_tocar_nivel; applies the actions)
// click: a = tecvirt_clique(&s_kb, xs, ys);   (when touching, if tecvirt_contains)
//
// Does not replace the physical of ExtPort1: with both available the physical wins,
// for being faster and not stealing screen.

#pragma once
#include "app.h"

// 4 rows of characters (the last one with shift and erase at the ends) + the of command.
#define TECVIRT_FILAS    4

// Height it occupies in area 'a' (the same passes to tecvirt_posicionar):
// the rows and the gaps. DEPENDS ON ORIENTATION - upright the keys are more
// high, because it lacks width and leaves height (see the .cpp). Who books
// space for the keyboard asks here, never stores a number.
//
// NO dead stripe on the footer: while it is open a navigation stays
// locked and its track becomes of app (sistema_travar_navegacao), so
// the keyboard touches the bar of bottom and one finger that passes from row of
// command still applies to her.
int virtual_keyboard_height(const area_t& a);

// What a key does (tecvirt_nivel/tecvirt_clique return this):
// >0 the CODEPOINT Unicode typed, JA with the shift applied. ASCII in
// majority; the second page of symbols has Portuguese accents
// (the system font has them) - whoever saves text converts with
// tecvirt_utf8, or use tecvirt_editar, which already does this.
// -2 erase -3 send -5 close the keyboard
// Shift and page changes are of their own keyboard: they change the labels and never
// arrive on app.
#define TECVIRT_APAGAR  (-2)
#define TECVIRT_ENVIAR  (-3)
#define TECVIRT_FECHAR  (-5)

// Stable identifier of key: characters = row*16 + column; specials = 64 + index.
#define TECVIRT_NENHUMA (-1)

// Shift as on cell phones: one touch is valid for ONE letter; two touches in a row
// lock (caps lock); one touch with it locked released.
#define TECVIRT_SHIFT_NAO    0
#define TECVIRT_SHIFT_UMA    1
#define TECVIRT_SHIFT_TRAVA  2

typedef struct {
  area_t   a;                     // area on SCREEN on whose base it opens (tecvirt_positionar)
  bool     open;
  bool     phrase;                 // automatic capitalization at the start of phrase (tecvirt_frase)
  uint8_t  shift;                 // TECVIRT_SHIFT_*
  uint8_t  pagina;                // 0 = letters, 1 and 2 = symbols
  uint32_t shift_ms;              // when the shift was touched (the second touch hangs)
  bool     dirty;                  // next paint remakes everything (opened, changed of place)
  bool     rotulos;               // shift/page changed: redoes the keys, without cleaning the background
  int16_t  key_sob, key_pinta;   // lit under the finger / the one that is painted lit
  int16_t  ultima_key;
  uint32_t key_ms;
  uint32_t sample;               // counter of calls to tecvirt_nivel (clock of echo)
  uint8_t  livre;                 // samples without finger since it opened (see the .cpp)
  // One per finger (id of controller; -1 = free). 'key' = the one under the
  // finger now; 'firme' = the one he types on raise (exchange only after of
  // TECLA_FIRME consecutive samples in new: 'cand'/'cand_n'). 'echo' =
  // reappearance of finger that has just been released (the GT911 loses one finger for a
  // sample): does not type new. 'repeated' = the insured DELETE has already deleted
  // alone (release does not erase it again).
  struct { int8_t id; bool eco, repetiu; uint8_t cand_n; int16_t key, firme, cand, x, y; uint32_t since, rep_ms; } fingers[SYSTEM_MAX_TOUCHES];
  struct { int16_t key, x, y; uint32_t sample, ms; } released;   // the last release you typed
  // Measurements that depend on width (source of each special key), made
  // once per width and not for each key painted.
  int      med_larg;
  uint8_t  px_esp[8];
} virtual_keyboard_t;

void virtual_keyboard_open(virtual_keyboard_t* t);
void virtual_keyboard_close(virtual_keyboard_t* t);
static inline bool virtual_keyboard_is_open(const virtual_keyboard_t* t) { return t->open; }

// The area (coordinates of TELA) in whose BASE it opens, in width of it. Changed,
// next paint remakes everything.
void virtual_keyboard_position(virtual_keyboard_t* t, const area_t& a);

// Does the point (SCREEN) fall in range of keyboard open?
bool virtual_keyboard_contains(const virtual_keyboard_t* t, int xs, int ys);

// Call in ALL pass of drawing of app. 'complete' = the app cleaned the area
// (the keyboard redoes everything); without it, only what changed: the key that turned on and the one that
// deleted, or the labels after shift. Nothing changed, nothing is drawn.
void virtual_keyboard_paint(virtual_keyboard_t* t, lgfx::LovyanGFX& d, bool complete);

// In ao_tocar_nivel of app, with the keyboard open: read the fingers (system_toques),
// lights up the key that the last one will type, and returns in 'actions' the keys confirmed in this
// sample (in order in which the fingers were released). Ask for the redraw alone.
int virtual_keyboard_level(virtual_keyboard_t* t, int* actions, int max);

// When touching, dot on TELA inside keyboard: touch network too fast
// for the level to have seen. Returns the action, or 0 (nothing to do).
int virtual_keyboard_click(virtual_keyboard_t* t, int xs, int ys);

// AUTOMATIC CAPITAL (for plain text: the conversation starts; password, PIN and
// name no). On, the app counts text and the cursor whenever one
// of the two changes (and right after of open): at the beginning of text and after of
// ". ", "! " or "? " shift sets to ONE letter. Caps lock is not changed.
void virtual_keyboard_phrase(virtual_keyboard_t* t, bool enabled);
void virtual_keyboard_context(virtual_keyboard_t* t, const char* text, int cursor);

// Codepoint -> UTF-8 in 'out' (up to 4 bytes, without zero at the end). Returns the bytes.
int virtual_keyboard_utf8(int cp, char out[4]);

// SIMPLE EDITION, for field without cursor (password, name): character enters the end,
// DELETE takes out the last entire character (UTF-8). 'so_ascii' refuses the rest.
// 'cap' = size of buffer (with zero). Returns true if the action was of edit
// (character or erase) - SEND and CLOSE remain with the app.
bool virtual_keyboard_edit(char* buf, size_t cap, int acao, bool ascii_only);
