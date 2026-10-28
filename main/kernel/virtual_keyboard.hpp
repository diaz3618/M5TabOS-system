// Teclado virtual do SISTEMA.
//
// Um componente so para todo app que precisa de texto: a conversa (Mesh,
// ESP-NOW), a senha do Wi-Fi, o PIN do Bluetooth. O app nao desenha tecla,
// nao trata dedo e nao trava a navegacao - ele so:
//
//   static tecvirt_t s_kb;
//   abrir:      tecvirt_abrir(&s_kb);                (trava o INICIO)
//   fechar:     tecvirt_fechar(&s_kb);               (tambem no ao_fechar do app)
//   desenhar:   tecvirt_posicionar(&s_kb, a);        (a area, em TELA, em cuja base ele abre)
//               tecvirt_pintar(&s_kb, d, completo);  (TODA passada: e barato quando nada mudou)
//   nivel:      n = tecvirt_nivel(&s_kb, acoes, 8);  (no ao_tocar_nivel; aplica as acoes)
//   clique:     a = tecvirt_clique(&s_kb, xs, ys);   (no ao_tocar, se tecvirt_contem)
//
// Nao substitui o fisico do ExtPort1: com os dois disponiveis o fisico ganha,
// por ser mais rapido e nao roubar tela.

#pragma once
#include "app.h"

// 4 fileiras de caracteres (a ultima com shift e apagar nas pontas) + a de comando.
#define TECVIRT_FILAS    4

// Altura que ele ocupa na area 'a' (a mesma passada a tecvirt_posicionar):
// as fileiras e os vaos. DEPENDE DA ORIENTACAO - em pe as teclas sao mais
// altas, porque la falta largura e sobra altura (ver o .cpp). Quem reserva
// espaco para o teclado pergunta aqui, nunca guarda um numero.
//
// SEM faixa morta no rodape: enquanto ele esta aberto a navegacao fica
// travada e a faixa dela passa a ser do app (sistema_travar_navegacao), entao
// o teclado encosta na barra de baixo e um dedo que passa da fileira de
// comando ainda vale nela.
int tecvirt_altura(const area_t& a);

// O que uma tecla faz (tecvirt_nivel/tecvirt_clique devolvem isto):
//   >0  o CODEPOINT Unicode digitado, JA com o shift aplicado. ASCII na
//       maioria; a segunda pagina de simbolos tem os acentos do portugues
//       (a fonte do sistema os tem) - quem guarda texto converte com
//       tecvirt_utf8, ou usa tecvirt_editar, que ja faz isso.
//   -2  apagar    -3  enviar    -5  fechar o teclado
// Shift e troca de pagina sao do proprio teclado: trocam os rotulos e nunca
// chegam ao app.
#define TECVIRT_APAGAR  (-2)
#define TECVIRT_ENVIAR  (-3)
#define TECVIRT_FECHAR  (-5)

// Identificador estavel de tecla: caracteres = fileira*16 + coluna; especiais = 64 + indice.
#define TECVIRT_NENHUMA (-1)

// Shift como em celular: um toque vale para UMA letra; dois toques seguidos
// travam (caps lock); um toque com ele travado solta.
#define TECVIRT_SHIFT_NAO    0
#define TECVIRT_SHIFT_UMA    1
#define TECVIRT_SHIFT_TRAVA  2

typedef struct {
  area_t   a;                     // area em TELA em cuja base ele abre (tecvirt_posicionar)
  bool     aberto;
  bool     frase;                 // maiuscula automatica no inicio de frase (tecvirt_frase)
  uint8_t  shift;                 // TECVIRT_SHIFT_*
  uint8_t  pagina;                // 0 = letras, 1 e 2 = simbolos
  uint32_t shift_ms;              // quando o shift foi tocado (o segundo toque trava)
  bool     sujo;                  // proxima pintura refaz tudo (abriu, mudou de lugar)
  bool     rotulos;               // shift/pagina trocou: refaz as teclas, sem limpar o fundo
  int16_t  tecla_sob, tecla_pinta;   // acesa sob o dedo / a que esta pintada acesa
  int16_t  ultima_tecla;
  uint32_t tecla_ms;
  uint32_t amostra;               // contador de chamadas a tecvirt_nivel (relogio do eco)
  uint8_t  livre;                 // amostras sem dedo desde que abriu (ver o .cpp)
  // Um por dedo (id do controlador; -1 = livre). 'tecla' = a que esta sob o
  // dedo agora; 'firme' = a que ele digita ao subir (troca so depois de
  // TECLA_FIRME amostras seguidas na nova: 'cand'/'cand_n'). 'eco' =
  // reaparicao do dedo que acabou de soltar (o GT911 perde um dedo por uma
  // amostra): nao digita de novo. 'repetiu' = o APAGAR segurado ja apagou
  // sozinho (a soltura nao apaga de novo).
  struct { int8_t id; bool eco, repetiu; uint8_t cand_n; int16_t tecla, firme, cand, x, y; uint32_t desde, rep_ms; } dedos[SISTEMA_TOQUES_MAX];
  struct { int16_t tecla, x, y; uint32_t amostra, ms; } solta;   // a ultima soltura que digitou
  // Medidas que dependem da largura (fonte de cada tecla especial), feitas
  // uma vez por largura e nao a cada tecla pintada.
  int      med_larg;
  uint8_t  px_esp[8];
} tecvirt_t;

void tecvirt_abrir(tecvirt_t* t);
void tecvirt_fechar(tecvirt_t* t);
static inline bool tecvirt_aberto(const tecvirt_t* t) { return t->aberto; }

// A area (coordenadas de TELA) em cuja BASE ele abre, na largura dela. Mudou,
// a proxima pintura refaz tudo.
void tecvirt_posicionar(tecvirt_t* t, const area_t& a);

// O ponto (TELA) cai na faixa do teclado aberto?
bool tecvirt_contem(const tecvirt_t* t, int xs, int ys);

// Chamar em TODA passada de desenho do app. 'completo' = o app limpou a area
// (o teclado refaz tudo); sem ele, so o que mudou: a tecla que acendeu e a que
// apagou, ou os rotulos depois do shift. Nada mudou, nao desenha nada.
void tecvirt_pintar(tecvirt_t* t, lgfx::LovyanGFX& d, bool completo);

// No ao_tocar_nivel do app, com o teclado aberto: le os dedos (sistema_toques),
// acende a tecla que o ultimo vai digitar, e devolve em 'acoes' as teclas confirmadas nesta
// amostra (na ordem em que os dedos soltaram). Pede o redesenho sozinho.
int tecvirt_nivel(tecvirt_t* t, int* acoes, int max);

// No ao_tocar, ponto em TELA dentro do teclado: rede para o toque rapido demais
// para o nivel ter visto. Devolve a acao, ou 0 (nada a fazer).
int tecvirt_clique(tecvirt_t* t, int xs, int ys);

// MAIUSCULA AUTOMATICA (para texto corrido: a conversa liga; senha, PIN e
// nome nao). Ligada, o app conta ao teclado o texto e o cursor sempre que um
// dos dois muda (e logo depois de abrir): no comeco do texto e depois de
// ". ", "! " ou "? " o shift arma para UMA letra. Caps lock nao e mexido.
void tecvirt_frase(tecvirt_t* t, bool ligada);
void tecvirt_contexto(tecvirt_t* t, const char* texto, int cursor);

// Codepoint -> UTF-8 em 'out' (ate 4 bytes, sem zero no fim). Devolve os bytes.
int tecvirt_utf8(int cp, char out[4]);

// EDICAO SIMPLES, para campo sem cursor (senha, nome): caractere entra no fim,
// APAGAR tira o ultimo caractere inteiro (UTF-8). 'so_ascii' recusa o resto.
// 'cap' = tamanho do buffer (com o zero). Devolve true se a acao era de edicao
// (caractere ou apagar) - ENVIAR e FECHAR ficam com o app.
bool tecvirt_editar(char* buf, size_t cap, int acao, bool so_ascii);
