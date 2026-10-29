// CONVERSATION - the chat component of the system.
//
// Message balloons, writing bar, virtual keyboard (and the physical
// ExtPort1), scrolling and divider of day - everything what one app of message
// needs it, in a private place. Born from the Mesh app (same measurements, colors and rules,
// hardware measurements throughout September), so ESP-NOW is not a copy
// of it and the next app of message will not be a third one.
//
// THE APP ONLY ANSWERS THREE QUESTIONS, by function:
// - how many messages there are (count)
// - how is message i (0 = oldest) (read)
// - send this text (send)
// The rest is of components. It stores the measurements of each bubble (measure text and
// expensive) and only re-measures when the app warns that something has changed (conversa_mudou).
//
// HOW TO USE (see app_espnow/app_espnow.cpp):
// ao_abrir: conversa_abrir(&s_c, &s_p, count, read, send, nullptr);
// when_close: conversation_close(&s_c);
// ao_aviso: conversa_mudou(&s_c);        // arrived message, changed state
// draw: conversa_desenhar(&s_c, d, rectangle, complete, pode_escrita, reason);
// when_touch: conversation_tocar(&s_c, x, y);
// ao_tocar_nivel: conversation_tocar_nivel(&s_c, x, y, pressed);
// when_typing: conversation_typing(&s_c, text, code, mod);
//
// THE RECTANGULE goes to the BASE of area of app and occupies its entire width: the
// virtual keyboard opens at the top of the bottom, and the writing bar goes up
// together. At the top of it, the app puts whatever you want (the information bar of
// ESP-NOW, for example).
#pragma once

#include "app.h"
#include "list.hpp"
#include "paint.hpp"
#include "virtual_keyboard.hpp"

#define CONVERSA_TEXTO        256
// Bytes of A drawn line. With accent (2 bytes) and emoji (4), 64 ended
// before width of screen; 128 covers an entire line even just of emoji.
#define CONVERSA_LINHA_BYTES  128
#define CONVERSA_MAX_LINHAS   16

typedef enum {
  CONVERSA_ENVIO_NADA = 0,   // received (no mark)
  CONVERSA_ENVIO_PENDENTE,
  CONVERSA_ENVIO_OK,
  CONVERSA_ENVIO_FALHOU,
  CONVERSA_ENVIO_SEM_RESPOSTA,   // the deadline ended without the radio saying anything (Mesh)
  CONVERSA_ENVIO_OUVIDO,         // the network heard, fate did not confirm (gray seen)
} conversation_send_state_t;

typedef struct {
  char     text[CONVERSA_TEXTO];
  bool     minha;               // to right, with the tag of send_state
  uint32_t when;              // UTC epoch; 0 = no date (does not open day divider)
  char     name[16];            // label of sender ("" = no label)
  uint16_t color_name;
  char     meta[24];            // small line at the bottom ("" = without)
  uint16_t color_meta;
  uint8_t  send_state;               // conversation_sending_t
} conversation_msg_t;

typedef int  (*conversation_count_t)(void* ctx);
typedef bool (*conversation_read_t)(int i, conversation_msg_t* out, void* ctx);
typedef bool (*conversation_send_t)(const char* text, void* ctx);

struct conversation_bubble_s;

typedef struct {
  conversation_count_t count;
  conversation_read_t    read;
  conversation_send_t send;
  void*             ctx;
  paint_t*        p;    // a paint of APP: keyboard opening/closing asks for pass complete
  // DRAFT CEILING IN BYTES (0 = the entire buffer). The app defines after
  // conversa_abrir when its protocol accepts less than CONVERSA_TEXTO -
  // the bar to accept letter, instead of a message exit cut.
  int               limit;
  // SEND BUTTON COLOR (0 = a system default). The app paints with
  // his identity - Mesh uses the green of Meshtastic.
  uint16_t          color_send;

  // Internal status - the app does not move.
  list_t   lista;
  struct conversation_bubble_s* bal;   // PSRAM, 'cap' entries
  int       n, cap, larg;
  bool      medir;                // re-measure the balloons in next pass
  area_t    ret;                  // the rectangle of last drawing (screen)
  char      rascunho[CONVERSA_TEXTO];
  virtual_keyboard_t kb;                   // the virtual keyboard of system (fingers, return, drawing)
  // WRITE BAR. 'cursor' and the offset in BYTES within draft
  // (always on the border of one UTF-8 character). Breaking into lines keeps the
  // start of each line (bar_ini[n] = end of text) for touch in the field
  // find the position without re-measuring, and it is only redone when the text or width
  // change - not with each pass.
  int       cursor;
  char      barra_txt[CONVERSA_TEXTO];   // the text of last pass (for "changed")
  int       barra_base, barra_modo, barra_larg, barra_cursor, barra_topo;
  int       barra_env;                   // the of send button as it was painted (on/off)
  int16_t   bar_ini[CONVERSA_MAX_LINHAS + 2];
  int       bar_n, bar_topo, bar_nl;     // lines of text, first visible, how many visible
  int       bar_x0, bar_y0, bar_passo;   // where the first visible line was drawn (screen)
  bool      bar_rascunho;                // what is in bar is the draft (not the tip)
  lgfx::LovyanGFX* disp;                 // to measure text on touch in the field (only the interface task)
  bool      pode_write;        // the last drawing (the touch obeys what is on screen)
  area_t    tec;                  // where the keyboard opens (screen); width 0 = the rectangle itself
} conversation_t;

void conversation_open(conversation_t* c, paint_t* p, conversation_count_t count, conversation_read_t read,
                    conversation_send_t send, void* ctx);
void conversation_close(conversation_t* c);
void conversation_changed(conversation_t* c);

// 'can_escriva' false: the bar becomes a warning ('reason') and does not open keyboard.
void conversation_draw(conversation_t* c, lgfx::LovyanGFX& d, const area_t& r,
                       bool complete, bool pode_write, const char* reason);

// Coordinates of AREA of app - those that ao_tocar/ao_tocar_nivel receive.
void conversation_touch(conversation_t* c, int x, int y);
void conversation_touch_level(conversation_t* c, int x, int y, bool pressed);
void conversation_type(conversation_t* c, const char* text, uint8_t code, uint8_t mod);
bool conversation_keyboard_open(const conversation_t* c);

// THE KEYBOARD BIGGER THAN CONVERSATION. In Mesh, the conversation is just the
// column of right, but the keyboard needs the entire width of area (key
// narrow and typo). 'ka' in coordinates of TELA, with the SAME base
// of rectangle of conversation. Call before of conversa_desenhar.
void conversation_area_keyboard(conversation_t* c, const area_t& ka);

// The conversation shown changed (another channel, another person): close the keyboard, iteration
// on end of list and re-measure. The draft stays - is of user, not of conversation.
void conversation_switch(conversation_t* c);
void conversation_close_keyboard(conversation_t* c);

// Wrap text into lines that fit in 'px_width' (per word; per character
// only when a word alone doesn't fit). Publishes for those who draw text
// run with the same balloon rule (Mesh uses it).
int conversation_break_lines(lgfx::LovyanGFX& d, const char* txt, int width_px,
                            char lines[][CONVERSA_LINHA_BYTES], int max_lines);
