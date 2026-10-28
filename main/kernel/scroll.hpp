// Rolagem por toque - o mecanismo do SISTEMA, e nao mais de um app so.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// Existiam DOIS mecanismos de rolagem, sem relacao nenhuma
// entre si: o generico do core (kernel/core.cpp, unico consumidor o
// Ajustes) e o do proprio app Mesh - TRES copias quase identicas (conversas,
// nos, mensagens), cada uma com seu proprio par de arrasto/deslocamento
// escrito a mao. As tres copias do Mesh nunca ganharam o freio de 80ms que
// o app Ajustes sempre teve (ver o aviso grande em kernel/core.cpp sobre
// REPINTURA_MIN_MS): o toque do Mesh cai no unico ramo do loop principal
// sem throttle, e redesenha a ~50x/s em vez de ~12,5x/s.
//
// Criterio de projeto: nao corrigir so o Mesh (seria remendo para um app
// so) - o SISTEMA precisa de UM mecanismo, que o kernel e
// qualquer app usem, e que sirva de base para o que ainda vai precisar de
// rolagem no futuro (a propria home, paginada como tela de celular, quando
// encher de apps).
//
// -------------------------------------------------------------------------
// QUEM USA
// -------------------------------------------------------------------------
// Este e o MOTOR. Tem dois consumidores, e so dois:
//   - kernel/list  - TODA lista rolavel de app (Mesh, Arquivos, Tarefas).
//                     O app nunca chama rolagem_* direto: usa lista_*.
//   - kernel/core   - a rolagem da area inteira (app com altura_conteudo,
//                     o app Ajustes).
// Um app que precise rolar algo que nao e lista (um mapa, uma pagina
// horizontal) e sinal de que falta um COMPONENTE no kernel, nao de que o app
// deva montar a propria rolagem com estas funcoes.
//
// -------------------------------------------------------------------------
// O QUE ISTO NAO FAZ
// -------------------------------------------------------------------------
// Nao sabe nada de paginas, colunas, teclado aberto ou qualquer outra regra
// de zona - isso e do chamador (ver 'dentro' em rolagem_arrastar). Nao
// decide COMPLETO vs LEVE - isso continua sendo decisao de cada app (TUDO,
// pintura_completa(), etc). Nao mexe no framebuffer sozinho fora de
// rolagem_deslocar()/rolagem_barra() - o resto e so contabilidade.

#pragma once

#include <stdint.h>
#include <M5Unified.h>
#include "app.h"   // area_t

// EIXO e ANCORAGEM sao campos SEPARADOS, e nao um enum unico de quatro
// valores - sao perguntas ortogonais. "Vertical ou horizontal" decide QUAL
// funcao de deslocamento serve (tab5_tela_rolar copia linhas inteiras: e
// vertical por construcao). "Inicio ou fim" decide o SINAL do arrasto e a
// posicao da barrinha. A paginacao horizontal da home, um dia, vai precisar
// do primeiro campo sem mexer no segundo - empacotar os dois obrigaria a
// inventar um valor tipo "HORIZONTAL_FIM" que ninguem quer, so pro enum
// fechar.
#define ROLAGEM_VERTICAL    0
#define ROLAGEM_HORIZONTAL  1   // reservado - ver o aviso de paginacao no fim deste arquivo

#define ROLAGEM_INICIO  0   // lista comum, lida do topo (o padrao - por isso e zero)
#define ROLAGEM_FIM     1   // ancorada no fim (ex: mensagens - 0 = a mais recente)

// Distancia minima do ponto de descida do dedo para contar como arrasto (e
// nao clique). O MESMO numero para o sistema inteiro: o kernel (clique, em
// core.cpp) e toda lista (kernel/list) decidem "foi toque ou arrasto" por
// ele.
//
// 24 px (~2 mm a 294 DPI). Eram 12, e o kernel passou a usar
// 24 no reconhecedor de clique novo - com dois limites, um toque que mexia
// 13..24 px passava pelo kernel e a LISTA o recusava como "fim de arrasto":
// no app Arquivos, toque atras de toque sem efeito nenhum e sem log. Antes o
// wasClicked() do M5Unified recusava tudo acima de 8 px e o limite da lista
// nunca entrava em jogo. Um numero so impede que isso volte.
#define ROLAGEM_ARRASTE_MIN 24

typedef struct {
  int atual;         // offset corrente, em pixels; 0 = na origem da ancoragem
  int no_visor;      // quanto a PAGINA VISIVEL ja reflete de 'atual'
  int max;           // limite; 0 = nao rola. Quem grava e o chamador (rolagem_limite
                      // recalcula sozinho quando o conteudo pode encolher)
  int ancora;        // coordenada do dedo no ciclo anterior; -1 = sem dedo
  int descida;        // coordenada ONDE o dedo desceu - a referencia do arrasto
  int afastamento;    // maior afastamento da descida ate agora (arrasto vs. clique)
  uint8_t eixo;        // ROLAGEM_VERTICAL/HORIZONTAL
  uint8_t ancoragem;   // ROLAGEM_INICIO/FIM
  int passo_encaixe;   // 0 = rolagem livre. Reservado para paginacao futura
                       // (encaixar 'atual' num multiplo ao soltar o dedo) -
                       // nao tem funcao ainda porque nao ha tela que sinta.
} rolagem_t;

// O que UMA passada de rolagem_deslocar() fez - o chamador usa para recortar
// (so a faixa nova precisa ser redesenhada).
typedef struct {
  int    delta;      // deslocamento aplicado, em espaco de TELA (o que foi
                      // pra tab5_tela_rolar - ja com o sinal certo pra
                      // ancoragem desta lista)
  bool   deslocou;    // aproveitou os pixels que ja estavam certos
  area_t exposta;     // a faixa recem-exposta; so valida quando deslocou==true
} rolagem_passada_t;

// Prepara uma rolagem com eixo/ancoragem explicitos. So precisa ser chamada
// para o caso NAO-padrao (ROLAGEM_FIM, ou um dia HORIZONTAL) - um
// rolagem_t zero-inicializado (static, global) ja nasce VERTICAL/INICIO.
void rolagem_definir(rolagem_t* r, uint8_t eixo, uint8_t ancoragem);

// Volta 'atual' pra origem e solta a ancora do dedo - chamar ao abrir uma
// tela nova ou trocar de item selecionado. NAO mexe em 'no_visor': quem
// zera tambem vai forcar uma passada completa (que sincroniza sozinha via
// rolagem_sincronizar), exatamente como o kernel ja fazia.
void rolagem_zerar(rolagem_t* r);

// "A pagina visivel agora reflete 'atual'." Chamar depois de qualquer
// passada COMPLETA (que redesenhou tudo do zero, entao nao ha mais nada
// pendente de deslocar).
static inline void rolagem_sincronizar(rolagem_t* r) { r->no_visor = r->atual; }

// Verdadeiro quando a pagina visivel ainda nao reflete o offset atual - o
// sinal de que uma passada de rolagem_deslocar() tem trabalho a fazer.
static inline bool rolagem_pendente(const rolagem_t* r) { return r->atual != r->no_visor; }

// UMA amostra de toque. 'coord' e a coordenada no EIXO desta rolagem (y para
// vertical), ja no espaco do chamador - este modulo nao sabe nada de zonas,
// paginas ou teclado aberto, isso e responsabilidade de quem calcula
// 'dentro'. Devolve true SO quando 'atual' mudou de verdade (clampado em
// [0, r->max]) - um toque parado ou fora da zona nunca devolve true.
bool rolagem_arrastar(rolagem_t* r, int coord, bool dentro);

// "O ultimo toque foi ARRASTO, e nao clique?" Compara o afastamento maximo
// (acumulado por rolagem_arrastar) contra ROLAGEM_ARRASTE_MIN e ZERA o
// afastamento - chame uma vez por soltura do dedo (ex: dentro do tratamento
// de wasClicked()), nunca mais de uma vez por toque.
bool rolagem_foi_arraste(rolagem_t* r);

// Recalcula 'max' a partir de quanto conteudo existe (total) e quanto cabe
// na janela visivel, e PUXA 'atual' de volta se ele ficou alem do novo
// limite (a lista encolheu - um item foi removido, uma conversa trocou).
// Devolve true quando precisou puxar, para o chamador marcar a area como
// suja se for o caso.
bool rolagem_limite(rolagem_t* r, int total, int janela);

// Desloca os pixels ja corretos dentro de 'janela' (tab5_tela_rolar) para
// que a pagina visivel alcance 'atual'. Sincroniza 'no_visor' sozinha,
// SEMPRE - moveu de verdade (deslocou==true) ou nao (o chamador vai limpar
// e redesenhar tudo por conta propria, e dai 'no_visor' tem que acompanhar
// do mesmo jeito, senao a proxima passada acharia que ainda ha pendencia).
// NAO limpa nada sozinha quando deslocou==false: o fallback e decisao de
// cada chamador (o kernel cai para uma repintura COMPLETA via pagina
// oculta; o Mesh limpa a janela na hora, na pagina visivel) - os dois
// jeitos sao legitimos e diferentes, e nao deviam ser forcados a um so.
rolagem_passada_t rolagem_deslocar(rolagem_t* r, const area_t& janela, uint16_t cor);

// A barrinha: trilha + bolinha, 4px de largura a 'margem' px da borda
// direita da janela. Quem desenha e a lista (o app Ajustes nao tem barra, pedido
// ).
void rolagem_barra(lgfx::LovyanGFX& d, const rolagem_t* r, const area_t& janela,
                   uint16_t cor_trilha, int margem = 8);


// -------------------------------------------------------------------------
// PAGINACAO FUTURA (nao implementada - so o desenho nao impede)
// -------------------------------------------------------------------------
// Quando a home precisar de paginas horizontais: ROLAGEM_HORIZONTAL +
// ROLAGEM_INICIO (nao um quinto valor de ancoragem), 'passo_encaixe' > 0
// (a largura de uma pagina), e uma funcao nova - rolagem_soltar(r), chamada
// no fim do arrasto - que encaixa 'atual' no multiplo de 'passo_encaixe'
// mais proximo. rolagem_deslocar() despacharia em 'eixo' (precisaria de um
// tab5_tela_rolar_h() na HAL, ja que o de hoje copia linhas inteiras e e
// vertical por construcao). Nao ha ainda tela que sinta esse caminho, entao
// nao ha o que testar - por isso a funcao nao existe hoje, so o campo.
