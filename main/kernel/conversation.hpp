// CONVERSA - o componente of chat of sistema.
//
// Baloes of message, barra of escrita, keyboard virtual (e o fisico of
// ExtPort1), rolagem e divisor of dia - everything o que one app of message
// precisa, num lugar so. Nasceu of app Mesh (mesmas medidas, cores e regras,
// medidas em hardware on longo of setembro), para o ESP-NOW nao ser uma copia
// dele e o next app of message nao ser uma terceira.
//
// O APP SO RESPONDE TRES PERGUNTAS, por funcao:
//   - quantas mensagens ha            (count)
//   - como e a message i (0 = a mais antiga)   (read)
//   - envie este text                (send)
// O resto e of componente. Ele guarda as medidas of cada bubble (medir text e
// caro) e so re-mede when o app avisa que algo changed (conversa_mudou).
//
// COMO USAR (ver app_espnow/app_espnow.cpp):
//   ao_abrir:        conversa_abrir(&s_c, &s_p, count, read, send, nullptr);
//   ao_fechar:       conversa_fechar(&s_c);
//   ao_aviso:        conversa_mudou(&s_c);        // chegou message, changed estado
//   draw:        conversa_desenhar(&s_c, d, rectangle, complete, pode_escrever, reason);
//   ao_tocar:        conversa_tocar(&s_c, x, y);
//   ao_tocar_nivel:  conversa_tocar_nivel(&s_c, x, y, pressed);
//   ao_teclar:       conversa_teclar(&s_c, text, code, mod);
//
// O RETANGULO vai ate a BASE of area of app e ocupa a width inteira dela: o
// keyboard virtual abre por top of parte of bottom, e a barra of escrita sobe
// junto. Em top dele o app poe o que quiser (a barra of informacoes of
// ESP-NOW, por exemplo).
#pragma once

#include "app.h"
#include "list.hpp"
#include "paint.hpp"
#include "virtual_keyboard.hpp"

#define CONVERSA_TEXTO        256
// Bytes of UMA line desenhada. Com acento (2 bytes) e emoji (4), 64 acabava
// antes of width of screen; 128 cobre uma line inteira mesmo so of emoji.
#define CONVERSA_LINHA_BYTES  128
#define CONVERSA_MAX_LINHAS   16

typedef enum {
  CONVERSA_ENVIO_NADA = 0,   // recebida (sem marca)
  CONVERSA_ENVIO_PENDENTE,
  CONVERSA_ENVIO_OK,
  CONVERSA_ENVIO_FALHOU,
  CONVERSA_ENVIO_SEM_RESPOSTA,   // o prazo acabou sem o radio dizer nada (Mesh)
  CONVERSA_ENVIO_OUVIDO,         // a rede ouviu, o destino nao confirmou (visto cinza)
} conversation_send_state_t;

typedef struct {
  char     text[CONVERSA_TEXTO];
  bool     minha;               // a right, com a marca of send_state
  uint32_t when;              // epoch UTC; 0 = sem data (nao abre divisor of dia)
  char     name[16];            // rotulo of remetente ("" = sem rotulo)
  uint16_t color_name;
  char     meta[24];            // line pequena embaixo ("" = sem)
  uint16_t color_meta;
  uint8_t  send_state;               // conversa_envio_t
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
  paint_t*        p;    // a paint of APP: keyboard abrindo/fechando pede pass complete
  // TETO DO RASCUNHO EM BYTES (0 = o buffer inteiro). O app define depois of
  // conversa_abrir when o protocolo dele aceita menos que CONVERSA_TEXTO -
  // a barra para of aceitar letra, em vez of a message exit cortada.
  int               limit;
  // COR DO BOTAO DE ENVIAR (0 = a default of sistema). O app pinta com a
  // identidade dele - o Mesh usa o verde of marca of Meshtastic.
  uint16_t          color_send;

  // Estado interno - o app nao mexe.
  list_t   lista;
  struct conversation_bubble_s* bal;   // PSRAM, 'cap' entries
  int       n, cap, larg;
  bool      medir;                // re-medir os baloes in next pass
  area_t    ret;                  // o rectangle of ultimo drawing (screen)
  char      rascunho[CONVERSA_TEXTO];
  virtual_keyboard_t kb;                   // o keyboard virtual of sistema (fingers, return, drawing)
  // BARRA DE ESCRITA. 'cursor' e o offset em BYTES dentro of rascunho
  // (sempre in fronteira of one caractere UTF-8). A quebra em lines guarda o
  // start of cada line (bar_ini[n] = end of text) para o touch no field
  // achar a position sem re-medir, e so e refeita when o text ou a width
  // mudam - nao a cada pass.
  int       cursor;
  char      barra_txt[CONVERSA_TEXTO];   // o text of ultima pass (para o "changed")
  int       barra_base, barra_modo, barra_larg, barra_cursor, barra_topo;
  int       barra_env;                   // o botao of send como foi pintado (aceso/apagado)
  int16_t   bar_ini[CONVERSA_MAX_LINHAS + 2];
  int       bar_n, bar_topo, bar_nl;     // lines of text, primeira visivel, quantas visiveis
  int       bar_x0, bar_y0, bar_passo;   // onde a primeira line visivel foi desenhada (screen)
  bool      bar_rascunho;                // o que esta in barra e o rascunho (nao a dica)
  lgfx::LovyanGFX* disp;                 // para medir text on touch no field (so a tarefa of interface)
  bool      pode_write;        // o of ultimo drawing (o touch obedece o que esta in screen)
  area_t    tec;                  // onde o keyboard abre (screen); width 0 = o proprio rectangle
} conversation_t;

void conversation_open(conversation_t* c, paint_t* p, conversation_count_t count, conversation_read_t read,
                    conversation_send_t send, void* ctx);
void conversation_close(conversation_t* c);
void conversation_changed(conversation_t* c);

// 'pode_escrever' false: a barra vira aviso ('reason') e nao abre keyboard.
void conversation_draw(conversation_t* c, lgfx::LovyanGFX& d, const area_t& r,
                       bool complete, bool pode_write, const char* reason);

// Coordenadas of AREA of app - as que ao_tocar/ao_tocar_nivel recebem.
void conversation_touch(conversation_t* c, int x, int y);
void conversation_touch_level(conversation_t* c, int x, int y, bool pressed);
void conversation_type(conversation_t* c, const char* text, uint8_t code, uint8_t mod);
bool conversation_keyboard_open(const conversation_t* c);

// O TECLADO MAIS LARGO QUE A CONVERSA. No Mesh a conversa e so a
// coluna of right, mas o keyboard precisa of width inteira of area (key
// estreita e erro of digitacao). 'ka' em coordenadas of TELA, com a MESMA base
// of rectangle of conversa. Chamar antes of conversa_desenhar.
void conversation_area_keyboard(conversation_t* c, const area_t& ka);

// A conversa mostrada changed (outro canal, outra pessoa): fecha o keyboard, iteration
// on end of lista e re-mede. O rascunho stays - e of user, nao of conversa.
void conversation_switch(conversation_t* c);
void conversation_close_keyboard(conversation_t* c);

// Quebra text em lines que cabem em 'largura_px' (por palavra; por caractere
// so when uma palavra sozinha nao cabe). Publica para quem desenha text
// corrido com a mesma regra dos baloes (o Mesh usa).
int conversation_break_lines(lgfx::LovyanGFX& d, const char* txt, int width_px,
                            char lines[][CONVERSA_LINHA_BYTES], int max_lines);
