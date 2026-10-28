// Teclado virtual of SISTEMA.
//
// Um componente so para todo app que precisa of text: a conversa (Mesh,
// ESP-NOW), a senha of Wi-Fi, o PIN of Bluetooth. O app nao desenha key,
// nao trata finger e nao trava a navigation - ele so:
//
//   static tecvirt_t s_kb;
//   open:      tecvirt_abrir(&s_kb);                (trava o INICIO)
//   close:     tecvirt_fechar(&s_kb);               (tambem no ao_fechar of app)
//   draw:   tecvirt_posicionar(&s_kb, a);        (a area, em TELA, em cuja base ele abre)
//               tecvirt_pintar(&s_kb, d, complete);  (TODA pass: e barato when nada changed)
//   level:      n = tecvirt_nivel(&s_kb, actions, 8);  (no ao_tocar_nivel; aplica as actions)
//   click:     a = tecvirt_clique(&s_kb, xs, ys);   (no ao_tocar, se tecvirt_contem)
//
// Nao substitui o fisico of ExtPort1: com os dois disponiveis o fisico ganha,
// por ser mais rapido e nao roubar screen.

#pragma once
#include "app.h"

// 4 fileiras of caracteres (a ultima com shift e erase nas pontas) + a of comando.
#define TECVIRT_FILAS    4

// Altura que ele ocupa in area 'a' (a mesma pass a tecvirt_posicionar):
// as fileiras e os vaos. DEPENDE DA ORIENTACAO - em pe as teclas sao mais
// altas, porque la falta width e sobra height (ver o .cpp). Quem reserva
// espaco para o keyboard pergunta aqui, nunca guarda one numero.
//
// SEM faixa morta no rodape: enquanto ele esta open a navigation stays
// travada e a faixa dela passa a ser of app (sistema_travar_navegacao), entao
// o keyboard encosta in barra of bottom e one finger que passa of fileira of
// comando ainda vale nela.
int virtual_keyboard_height(const area_t& a);

// O que uma key faz (tecvirt_nivel/tecvirt_clique devolvem isto):
//   >0  o CODEPOINT Unicode digitado, JA com o shift aplicado. ASCII in
//       maioria; a segunda pagina of simbolos tem os acentos of portugues
//       (a fonte of sistema os tem) - quem guarda text converte com
//       tecvirt_utf8, ou usa tecvirt_editar, que ja faz isso.
//   -2  erase    -3  send    -5  close o keyboard
// Shift e troca of pagina sao of proprio keyboard: trocam os rotulos e nunca
// chegam on app.
#define TECVIRT_APAGAR  (-2)
#define TECVIRT_ENVIAR  (-3)
#define TECVIRT_FECHAR  (-5)

// Identificador estavel of key: caracteres = fileira*16 + coluna; especiais = 64 + indice.
#define TECVIRT_NENHUMA (-1)

// Shift como em celular: one touch vale para UMA letra; dois touches seguidos
// travam (caps lock); one touch com ele travado released.
#define TECVIRT_SHIFT_NAO    0
#define TECVIRT_SHIFT_UMA    1
#define TECVIRT_SHIFT_TRAVA  2

typedef struct {
  area_t   a;                     // area em TELA em cuja base ele abre (tecvirt_posicionar)
  bool     open;
  bool     phrase;                 // maiuscula automatica no start of phrase (tecvirt_frase)
  uint8_t  shift;                 // TECVIRT_SHIFT_*
  uint8_t  pagina;                // 0 = letras, 1 e 2 = simbolos
  uint32_t shift_ms;              // when o shift foi touched (o segundo touch trava)
  bool     dirty;                  // next paint refaz everything (abriu, changed of lugar)
  bool     rotulos;               // shift/pagina trocou: refaz as teclas, sem limpar o fundo
  int16_t  key_sob, key_pinta;   // acesa sob o finger / a que esta pintada acesa
  int16_t  ultima_key;
  uint32_t key_ms;
  uint32_t sample;               // contador of chamadas a tecvirt_nivel (clock of eco)
  uint8_t  livre;                 // amostras sem finger since que abriu (ver o .cpp)
  // Um por finger (id of controlador; -1 = livre). 'key' = a que esta sob o
  // finger now; 'firme' = a que ele digita on raise (troca so depois of
  // TECLA_FIRME amostras seguidas in nova: 'cand'/'cand_n'). 'eco' =
  // reaparicao of finger que acabou of release (o GT911 perde one finger por uma
  // sample): nao digita of novo. 'repetiu' = o APAGAR segurado ja apagou
  // sozinho (a soltura nao apaga of novo).
  struct { int8_t id; bool eco, repetiu; uint8_t cand_n; int16_t key, firme, cand, x, y; uint32_t since, rep_ms; } fingers[SISTEMA_TOQUES_MAX];
  struct { int16_t key, x, y; uint32_t sample, ms; } released;   // a ultima soltura que digitou
  // Medidas que dependem of width (fonte of cada key especial), feitas
  // uma vez por width e nao a cada key pintada.
  int      med_larg;
  uint8_t  px_esp[8];
} virtual_keyboard_t;

void virtual_keyboard_open(virtual_keyboard_t* t);
void virtual_keyboard_close(virtual_keyboard_t* t);
static inline bool virtual_keyboard_is_open(const virtual_keyboard_t* t) { return t->open; }

// A area (coordenadas of TELA) em cuja BASE ele abre, in width dela. Mudou,
// a next paint refaz everything.
void virtual_keyboard_position(virtual_keyboard_t* t, const area_t& a);

// O ponto (TELA) cai in faixa of keyboard open?
bool virtual_keyboard_contains(const virtual_keyboard_t* t, int xs, int ys);

// Chamar em TODA pass of drawing of app. 'complete' = o app limpou a area
// (o keyboard refaz everything); sem ele, so o que changed: a key que acendeu e a que
// apagou, ou os rotulos depois of shift. Nada changed, nao desenha nada.
void virtual_keyboard_paint(virtual_keyboard_t* t, lgfx::LovyanGFX& d, bool complete);

// No ao_tocar_nivel of app, com o keyboard open: le os fingers (sistema_toques),
// acende a key que o ultimo vai digitar, e devolve em 'actions' as teclas confirmadas nesta
// sample (in order em que os fingers soltaram). Pede o redraw sozinho.
int virtual_keyboard_level(virtual_keyboard_t* t, int* actions, int max);

// No ao_tocar, ponto em TELA dentro of keyboard: rede para o touch rapido demais
// para o level ter visto. Devolve a acao, ou 0 (nada a fazer).
int virtual_keyboard_click(virtual_keyboard_t* t, int xs, int ys);

// MAIUSCULA AUTOMATICA (para text corrido: a conversa liga; senha, PIN e
// name nao). Ligada, o app conta on keyboard o text e o cursor sempre que one
// dos dois muda (e logo depois of open): no comeco of text e depois of
// ". ", "! " ou "? " o shift arma para UMA letra. Caps lock nao e mexido.
void virtual_keyboard_phrase(virtual_keyboard_t* t, bool enabled);
void virtual_keyboard_context(virtual_keyboard_t* t, const char* text, int cursor);

// Codepoint -> UTF-8 em 'out' (ate 4 bytes, sem zero no end). Devolve os bytes.
int virtual_keyboard_utf8(int cp, char out[4]);

// EDICAO SIMPLES, para field sem cursor (senha, name): caractere entra no end,
// APAGAR tira o ultimo caractere inteiro (UTF-8). 'so_ascii' recusa o resto.
// 'cap' = tamanho of buffer (com o zero). Devolve true se a acao era of edicao
// (caractere ou erase) - ENVIAR e FECHAR ficam com o app.
bool virtual_keyboard_edit(char* buf, size_t cap, int acao, bool ascii_only);
