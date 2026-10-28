// Rolagem por touch - o mecanismo of SISTEMA, e nao mais of one app so.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// Existiam DOIS mecanismos of rolagem, sem relacao none
// entre si: o generico of core (kernel/core.cpp, unico consumidor o
// Ajustes) e o of proprio app Mesh - TRES copias quase identicas (conversas,
// nos, mensagens), cada uma com seu proprio par of arrasto/offset
// escrito a mao. As tres copias of Mesh nunca ganharam o freio of 80ms que
// o app Ajustes sempre teve (ver o aviso grande em kernel/core.cpp sobre
// REPINTURA_MIN_MS): o touch of Mesh cai no unico ramo of loop principal
// sem throttle, e redesenha a ~50x/s em vez of ~12,5x/s.
//
// Criterio of projeto: nao corrigir so o Mesh (seria remendo para one app
// so) - o SISTEMA precisa of UM mecanismo, que o kernel e
// qualquer app usem, e que sirva of base para o que ainda vai precisar of
// rolagem no futuro (a propria home, paginada como screen of celular, when
// encher of apps).
//
// -------------------------------------------------------------------------
// QUEM USA
// -------------------------------------------------------------------------
// Este e o MOTOR. Tem dois consumidores, e so dois:
//   - kernel/list  - TODA lista rolavel of app (Mesh, Arquivos, Tarefas).
//                     O app nunca chama rolagem_* direto: usa lista_*.
//   - kernel/core   - a rolagem of area inteira (app com altura_conteudo,
//                     o app Ajustes).
// Um app que precise rolar algo que nao e lista (one mapa, uma pagina
// horizontal) e sinal of que falta one COMPONENTE no kernel, nao of que o app
// deva montar a propria rolagem com estas funcoes.
//
// -------------------------------------------------------------------------
// O QUE ISTO NAO FAZ
// -------------------------------------------------------------------------
// Nao sabe nada of paginas, colunas, keyboard open ou qualquer outra regra
// of zona - isso e of chamador (ver 'dentro' em rolagem_arrastar). Nao
// decide COMPLETO vs LEVE - isso continua sendo decisao of cada app (TUDO,
// pintura_completa(), etc). Nao mexe no framebuffer sozinho fora of
// rolagem_deslocar()/rolagem_barra() - o resto e so contabilidade.

#pragma once

#include <stdint.h>
#include <M5Unified.h>
#include "app.h"   // area_t

// EIXO e ANCORAGEM sao campos SEPARADOS, e nao one enum unico of quatro
// valores - sao perguntas ortogonais. "Vertical ou horizontal" decide QUAL
// funcao of offset serve (tab5_screen_scroll copia lines inteiras: e
// vertical por construcao). "Inicio ou end" decide o SINAL of arrasto e a
// position of barrinha. A paginacao horizontal of home, one dia, vai precisar
// of first field sem mexer no segundo - empacotar os dois obrigaria a
// inventar one valor tipo "HORIZONTAL_FIM" que ninguem quer, so pro enum
// close.
#define ROLAGEM_VERTICAL    0
#define ROLAGEM_HORIZONTAL  1   // reservado - ver o aviso of paginacao no end deste arquivo

#define ROLAGEM_INICIO  0   // lista comum, lida of topo (o default - por isso e zero)
#define ROLAGEM_FIM     1   // anchored no end (ex: mensagens - 0 = a mais recente)

// Distancia minima of ponto of press_start of finger para count como arrasto (e
// nao click). O MESMO numero para o sistema inteiro: o kernel (click, em
// core.cpp) e toda lista (kernel/list) decidem "foi touch ou arrasto" por
// ele.
//
// 24 px (~2 mm a 294 DPI). Eram 12, e o kernel passou a usar
// 24 no reconhecedor of click novo - com dois limites, one touch que mexia
// 13..24 px passava pelo kernel e a LISTA o recusava como "end of arrasto":
// no app Arquivos, touch atras of touch sem efeito nenhum e sem log. Antes o
// wasClicked() of M5Unified recusava everything acima of 8 px e o limit of lista
// nunca entrava em jogo. Um numero so impede que isso volte.
#define ROLAGEM_ARRASTE_MIN 24

typedef struct {
  int current;         // offset corrente, em pixels; 0 = in origem of anchoring
  int no_visor;      // quanto a PAGINA VISIVEL ja reflete of 'current'
  int max;           // limit; 0 = nao rola. Quem grava e o chamador (rolagem_limite
                      // recalcula sozinho when o content pode encolher)
  int anchor;        // coordenada of finger no ciclo anterior; -1 = sem finger
  int press_start;        // coordenada ONDE o finger desceu - a referencia of arrasto
  int distance;    // maior distance of press_start ate now (arrasto vs. click)
  uint8_t eixo;        // ROLAGEM_VERTICAL/HORIZONTAL
  uint8_t anchoring;   // ROLAGEM_INICIO/FIM
  int passo_encaixe;   // 0 = rolagem livre. Reservado para paginacao futura
                       // (encaixar 'current' num multiplo on release o finger) -
                       // nao tem funcao ainda porque nao ha screen que sinta.
} scroll_t;

// O que UMA pass of rolagem_deslocar() fez - o chamador usa para recortar
// (so a faixa nova precisa ser redesenhada).
typedef struct {
  int    delta;      // offset aplicado, em espaco of TELA (o que foi
                      // pra tab5_screen_scroll - ja com o sinal certo pra
                      // anchoring desta lista)
  bool   moved;    // aproveitou os pixels que ja estavam certos
  area_t exposta;     // a faixa recem-exposta; so valida when moved==true
} scroll_pass_t;

// Prepara uma rolagem com eixo/anchoring explicitos. So precisa ser chamada
// para o caso NAO-padrao (ROLAGEM_FIM, ou one dia HORIZONTAL) - one
// rolagem_t zero-inicializado (static, global) ja nasce VERTICAL/INICIO.
void scroll_set(scroll_t* r, uint8_t eixo, uint8_t anchoring);

// Volta 'current' pra origem e released a anchor of finger - chamar on open uma
// screen nova ou switch of item selected. NAO mexe em 'no_visor': quem
// zera tambem vai forcar uma pass complete (que sincroniza sozinha via
// rolagem_sincronizar), exatamente como o kernel ja fazia.
void scroll_reset(scroll_t* r);

// "A pagina visivel now reflete 'current'." Chamar depois of qualquer
// pass COMPLETA (que redesenhou everything of zero, entao nao ha mais nada
// pending of scroll).
static inline void scroll_synchronize(scroll_t* r) { r->no_visor = r->current; }

// Verdadeiro when a pagina visivel ainda nao reflete o offset current - o
// sinal of que uma pass of rolagem_deslocar() tem work a fazer.
static inline bool scroll_pending(const scroll_t* r) { return r->current != r->no_visor; }

// UMA sample of touch. 'coord' e a coordenada no EIXO desta rolagem (y para
// vertical), ja no espaco of chamador - este modulo nao sabe nada of zonas,
// paginas ou keyboard open, isso e responsabilidade of quem calcula
// 'dentro'. Devolve true SO when 'current' changed of verdade (clampado em
// [0, r->max]) - one touch parado ou fora of zona nunca devolve true.
bool scroll_drag(scroll_t* r, int coord, bool dentro);

// "O ultimo touch foi ARRASTO, e nao click?" Compara o distance maximum
// (acumulado por rolagem_arrastar) contra ROLAGEM_ARRASTE_MIN e ZERA o
// distance - chame uma vez por soltura of finger (ex: dentro of tratamento
// of wasClicked()), nunca mais of uma vez por touch.
bool scroll_foi_arraste(scroll_t* r);

// Recalcula 'max' a partir of quanto content existe (total) e quanto cabe
// in window visivel, e PUXA 'current' of iteration se ele became alem of novo
// limit (a lista encolheu - one item foi removido, uma conversa trocou).
// Devolve true when precisou puxar, para o chamador marcar a area como
// dirty se for o caso.
bool scroll_limit(scroll_t* r, int total, int window);

// Desloca os pixels ja corretos dentro of 'window' (tab5_screen_scroll) para
// que a pagina visivel alcance 'current'. Sincroniza 'no_visor' sozinha,
// SEMPRE - moveu of verdade (moved==true) ou nao (o chamador vai limpar
// e redesenhar everything por conta propria, e dai 'no_visor' tem que acompanhar
// of mesmo jeito, senao a next pass acharia que ainda ha pendencia).
// NAO limpa nada sozinha when moved==false: o fallback e decisao of
// cada chamador (o kernel cai para uma repintura COMPLETA via pagina
// oculta; o Mesh limpa a window in hora, in pagina visivel) - os dois
// jeitos sao legitimos e diferentes, e nao deviam ser forcados a one so.
scroll_pass_t scroll_scroll(scroll_t* r, const area_t& window, uint16_t color);

// A barrinha: track + bolinha, 4px of width a 'margem' px of borda
// right of window. Quem desenha e a lista (o app Ajustes nao tem barra, request
// ).
void scroll_barra(lgfx::LovyanGFX& d, const scroll_t* r, const area_t& window,
                   uint16_t color_track, int margem = 8);


// -------------------------------------------------------------------------
// PAGINACAO FUTURA (nao implementada - so o drawing nao impede)
// -------------------------------------------------------------------------
// Quando a home precisar of paginas horizontais: ROLAGEM_HORIZONTAL +
// ROLAGEM_INICIO (nao one quinto valor of anchoring), 'passo_encaixe' > 0
// (a width of uma pagina), e uma funcao nova - rolagem_soltar(r), chamada
// no end of arrasto - que encaixa 'current' no multiplo of 'passo_encaixe'
// mais next. rolagem_deslocar() despacharia em 'eixo' (precisaria of one
// tab5_tela_rolar_h() in HAL, ja que o of hoje copia lines inteiras e e
// vertical por construcao). Nao ha ainda screen que sinta esse caminho, entao
// nao ha o que testar - por isso a funcao nao existe hoje, so o field.
