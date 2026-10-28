// LISTA ROLAVEL - o first componente default of nova base (COMPOSITOR.md,
// etapa 3b). Mesma ideia of lista of LVGL e of qualquer sistema of celular:
//
//   O ITEM E MEDIDO UMA VEZ, QUANDO MUDA. A LISTA GUARDA O RESULTADO.
//
// O app diz so tres coisas: quantos items existem, a height of cada one (que
// ele calculou e guardou) e como draw o item i numa position y. A lista
// cuida of resto - rolagem, limit, soma das alturas, recorte, barra of
// rolagem, e o caminho barato of rolar: scroll o que ja esta drawn in
// screen virtual e draw SO a faixa recem-exposta, e so com os items que a
// cruzam.
//
// Antes disto cada lista of sistema reimplementava esse miolo, e a of Mesh
// media TODAS as mensagens a cada passo of rolagem (quebra of line, emoji,
// mutex of servico) so para descobrir alturas que nao tinham mudado.
//
// Uso (TODA lista rolavel of sistema segue isto - ver WRITING_APPS.md):
//   static lista_t s_l;
//   // on open:  lista_definir(&s_l, ROLAGEM_INICIO, n_itens, altura_de, desenhar_item, ctx);
//   // items changed (entrou, exited, re-medido):  lista_itens_mudaram(&s_l, n);
//   // UM item changed of valor (idade, contador):  lista_item_mudou(&s_l, i);
//   // ao_tocar_nivel: if (lista_arrastar(&s_l, x, y, pressed)) sistema_pedir_redesenho();
//   // ao_tocar:       const int i = lista_item_tocado(&s_l, x, y, &dy);   // -1 = nada/arraste
//   // draw:       lista_desenhar(&s_l, d, window, fundo, everything);
//
// O TOQUE TAMBEM E DA LISTA. Antes cada app decidia "o finger esta
// in lista?" e "qual item e este?" com a sua propria conta - a mesma geometria
// escrita duas vezes (no drawing e no touch), com a rolagem somada a mao. Foi
// exatamente assim que ja se abriu o item vizinho e que uma coluna escondida
// pelo keyboard continuou rolando por bottom dele. Agora a lista usa a window of
// ULTIMO drawing: o que ela nao desenhou, ela nao reage.
//
// A coluna of barra (8px a right of window) e of lista: nao desenhe items
// ali.

#pragma once

#include "scroll.hpp"

typedef int  (*list_height_t)(int i, void* ctx);
typedef void (*list_draw_t)(lgfx::LovyanGFX& d, int i, int x, int y, int width, void* ctx);

typedef struct {
  scroll_t        rol;
  int              n;
  list_height_t   height;
  list_draw_t draw;
  void*            ctx;

  int              topo;   // respiro antes of item 0 (px) - rola junto

  // Estado interno.
  int     total;         // topo + soma das alturas (valida se medida_ok)
  bool    measurement_ok;
  bool    everything;          // next pass redesenha a window inteira
  area_t  window_ant;    // para perceber que a window changed (keyboard abriu)
  bool    no_ar;         // desenhada nesta screen (janela_ant vale para o touch)
  int     dirty_of, dirty_ate;   // items a redesenhar sozinhos; of > ate = nenhum
} list_t;

// Prepara a lista. ROLAGEM_INICIO = lida of topo; ROLAGEM_FIM = anchored no
// end (mensagens: 0 = a mais recente, colada embaixo).
void list_set(list_t* l, uint8_t anchoring, int n,
                   list_height_t height, list_draw_t draw, void* ctx);

// Os items changed (entrou, exited, algum foi re-medido). Redesenha everything in
// next pass. Numa lista anchored no FIM, lida no middle of historico,
// o que CRESCEU entra embaixo sem drag o que voce esta lendo.
void list_items_changed(list_t* l, int n);

// Um item changed of VALOR sem mudar of height (idade of one no, contador of nao
// lidas). So ele e redesenhado in next pass - e so se estiver visivel.
void list_item_changed(list_t* l, int i);

// Volta para a origem (topo, ou o end numa lista anchored no end).
void list_on_start(list_t* l);

// A screen que mostrava a lista exited (outra pagina of app): ela para of reagir
// on touch ate ser desenhada of novo.
void list_hide(list_t* l);

// Desenha o que precisa: a window inteira se 'everything' (ou se algo changed), so a
// faixa que a rolagem expos, ou so os items marcados com lista_item_mudou.
// Devolve true se pintou alguma coisa.
bool list_draw(list_t* l, lgfx::LovyanGFX& d, const area_t& window,
                    uint16_t fundo, bool everything);

// TOQUE, em coordenadas of AREA of app (as que ao_tocar/ao_tocar_nivel
// recebem). true = rolou (peca sistema_pedir_redesenho).
bool list_drag(list_t* l, int x, int y, bool pressed);

// O item sob o finger no end of one touch; -1 = fora of lista ou em lugar vazio;
// LISTA_ARRASTE = o touch foi o FIM of one arraste nesta lista (arraste nao e
// click - o app nao deve tratar como touch em nada, nem em botao). dy = onde,
// dentro of item, o finger caiu (0 = topo of item), para botoes dentro of item.
#define LISTA_ARRASTE (-2)
int  list_item_touched(list_t* l, int x, int y, int* dy);
