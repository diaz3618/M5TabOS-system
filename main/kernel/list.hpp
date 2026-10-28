// LISTA ROLAVEL - o primeiro componente padrao da nova base (COMPOSITOR.md,
// etapa 3b). Mesma ideia da lista da LVGL e de qualquer sistema de celular:
//
//   O ITEM E MEDIDO UMA VEZ, QUANDO MUDA. A LISTA GUARDA O RESULTADO.
//
// O app diz so tres coisas: quantos itens existem, a altura de cada um (que
// ele calculou e guardou) e como desenhar o item i numa posicao y. A lista
// cuida do resto - rolagem, limite, soma das alturas, recorte, barra de
// rolagem, e o caminho barato de rolar: deslocar o que ja esta desenhado na
// tela virtual e desenhar SO a faixa recem-exposta, e so com os itens que a
// cruzam.
//
// Antes disto cada lista do sistema reimplementava esse miolo, e a do Mesh
// media TODAS as mensagens a cada passo de rolagem (quebra de linha, emoji,
// mutex do servico) so para descobrir alturas que nao tinham mudado.
//
// Uso (TODA lista rolavel do sistema segue isto - ver WRITING_APPS.md):
//   static lista_t s_l;
//   // ao abrir:  lista_definir(&s_l, ROLAGEM_INICIO, n_itens, altura_de, desenhar_item, ctx);
//   // itens mudaram (entrou, saiu, re-medido):  lista_itens_mudaram(&s_l, n);
//   // UM item mudou de valor (idade, contador):  lista_item_mudou(&s_l, i);
//   // ao_tocar_nivel: if (lista_arrastar(&s_l, x, y, pressionado)) sistema_pedir_redesenho();
//   // ao_tocar:       const int i = lista_item_tocado(&s_l, x, y, &dy);   // -1 = nada/arraste
//   // desenhar:       lista_desenhar(&s_l, d, janela, fundo, tudo);
//
// O TOQUE TAMBEM E DA LISTA. Antes cada app decidia "o dedo esta
// na lista?" e "qual item e este?" com a sua propria conta - a mesma geometria
// escrita duas vezes (no desenho e no toque), com a rolagem somada a mao. Foi
// exatamente assim que ja se abriu o item vizinho e que uma coluna escondida
// pelo teclado continuou rolando por baixo dele. Agora a lista usa a janela do
// ULTIMO desenho: o que ela nao desenhou, ela nao reage.
//
// A coluna da barra (8px a direita da janela) e da lista: nao desenhe itens
// ali.

#pragma once

#include "scroll.hpp"

typedef int  (*lista_altura_t)(int i, void* ctx);
typedef void (*lista_desenhar_t)(lgfx::LovyanGFX& d, int i, int x, int y, int largura, void* ctx);

typedef struct {
  rolagem_t        rol;
  int              n;
  lista_altura_t   altura;
  lista_desenhar_t desenhar;
  void*            ctx;

  int              topo;   // respiro antes do item 0 (px) - rola junto

  // Estado interno.
  int     total;         // topo + soma das alturas (valida se medida_ok)
  bool    medida_ok;
  bool    tudo;          // proxima passada redesenha a janela inteira
  area_t  janela_ant;    // para perceber que a janela mudou (teclado abriu)
  bool    no_ar;         // desenhada nesta tela (janela_ant vale para o toque)
  int     sujo_de, sujo_ate;   // itens a redesenhar sozinhos; de > ate = nenhum
} lista_t;

// Prepara a lista. ROLAGEM_INICIO = lida do topo; ROLAGEM_FIM = ancorada no
// fim (mensagens: 0 = a mais recente, colada embaixo).
void lista_definir(lista_t* l, uint8_t ancoragem, int n,
                   lista_altura_t altura, lista_desenhar_t desenhar, void* ctx);

// Os itens mudaram (entrou, saiu, algum foi re-medido). Redesenha tudo na
// proxima passada. Numa lista ancorada no FIM, lida no meio do historico,
// o que CRESCEU entra embaixo sem arrastar o que voce esta lendo.
void lista_itens_mudaram(lista_t* l, int n);

// Um item mudou de VALOR sem mudar de altura (idade de um no, contador de nao
// lidas). So ele e redesenhado na proxima passada - e so se estiver visivel.
void lista_item_mudou(lista_t* l, int i);

// Volta para a origem (topo, ou o fim numa lista ancorada no fim).
void lista_ao_inicio(lista_t* l);

// A tela que mostrava a lista saiu (outra pagina do app): ela para de reagir
// ao toque ate ser desenhada de novo.
void lista_esconder(lista_t* l);

// Desenha o que precisa: a janela inteira se 'tudo' (ou se algo mudou), so a
// faixa que a rolagem expos, ou so os itens marcados com lista_item_mudou.
// Devolve true se pintou alguma coisa.
bool lista_desenhar(lista_t* l, lgfx::LovyanGFX& d, const area_t& janela,
                    uint16_t fundo, bool tudo);

// TOQUE, em coordenadas da AREA do app (as que ao_tocar/ao_tocar_nivel
// recebem). true = rolou (peca sistema_pedir_redesenho).
bool lista_arrastar(lista_t* l, int x, int y, bool pressionado);

// O item sob o dedo no fim de um toque; -1 = fora da lista ou em lugar vazio;
// LISTA_ARRASTE = o toque foi o FIM de um arraste nesta lista (arraste nao e
// clique - o app nao deve tratar como toque em nada, nem em botao). dy = onde,
// dentro do item, o dedo caiu (0 = topo do item), para botoes dentro do item.
#define LISTA_ARRASTE (-2)
int  lista_item_tocado(lista_t* l, int x, int y, int* dy);
