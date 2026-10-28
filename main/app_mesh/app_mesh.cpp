// APP: Meshtastic - a JANELA of servico tab5_mesh.
//
// O app nao possui o enlace: quem fala com o C6L e o servico, que stays of pe
// com esta screen closed. Aqui so se desenha o que ele ja sabe. E o mesmo
// drawing of Ajustes para o WiFi, e e o que permite notify of message nova sem
// o app open.
//
// SEGUE O APARELHO (ORI_SEGUE_IMU no descritor): deitado (1280x720) ou em pe
// (720x1280). Com o keyboard encaixado, sempre deitado - a postura natural, e
// a lista of mensagens cabe muito mais.
//
// LAYOUT DEITADO
//   [trilho]  [conversas]   area principal
//     90px       200px      o resto
//
// O trilho e sempre visivel. A coluna of conversas SO aparece in pagina of
// mensagens - nas outras ela seria espaco morto. E a barra of escrita stays
// dentro of area das mensagens, nao sob as colunas.
//
// LAYOUT EM PE - nao cabem tres colunas em 720 px. Na pagina of mensagens a
// coluna of conversas ocupa o resto of width (com mais espaco para o name),
// e touch numa conversa a abre POR CIMA of everything, in width inteira, com uma
// faixa of VOLTAR em top. Voltar devolve a lista - o mesmo vai-e-volta of
// one app of message of celular. Nas outras paginas o trilho stays, e o
// content se acomoda: a tabela of nos esconde colunas, a malha vira uma
// coluna so, botoes lado a lado encolhem.

#include "list.hpp"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes in PSRAM
#include "work.hpp"
#include "app.h"
#include "tab5_mesh.hpp"
#include "tab5_mesh_mqtt.hpp"   // mesh_mqtt_internet_conectado
#include "tab5_mesh_admin.hpp"
#include "tab5_mesh_reboot_log.hpp"
#include "tab5_mesh_wifi_node.hpp"
#include "tab5_keyboard.hpp"
#include "virtual_keyboard.hpp"
#include "conversation.hpp"
#include "paint.hpp"
#include "scroll.hpp"
#include "tab5_screen.hpp"
#include "tab5_time.hpp"
#include <ctime>
#include "tab5_ui.hpp"
#include "tab5_emoji.hpp"
#include "settings.h"
#include "tab5_gps.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const int TRILHO_L = 90;
static const int CONV_L   = 200;
static const int ITEM_A   = 72;      // height of one item nas colunas

// BOTAO DE APAGAR CONVERSA. Geometria num lugar so, porque drawing e touch
// precisam concordar - e ja foi por concordarem "quase" que as colunas dos nos
// sairam of lugar uma vez.
//
// Fica ABAIXO of name, e nao on lado: on lado obrigaria a encurtar o name of 8
// para 6 caracteres, e o item tem 72px of height com o name ocupando so a
// primeira line. A height estava sobrando; a width, nao.
static const int APAGAR_L  = 30;
static const int APAGAR_A  = 28;
static const int APAGAR_DY = 40;            // a partir of y of item

// LARGURA DA COLUNA DE CONVERSAS: 200 deitado; em pe, everything o que sobra of
// trilho. Gravada in pass que desenha a coluna e lida pelo item e pelo
// touch - os tres concordam porque leem o mesmo numero.
static int s_col_l = CONV_L;
// O "x" stays a 50 px of end of coluna (fora of faixa of barra of rolagem),
// a partir of x of coluna.
static int erase_dx(void) { return s_col_l - 50; }

// EM PE, A CONVERSA ABERTA POR CIMA DA COLUNA. false = a lista; true = a
// conversa in width inteira, com VOLTAR. Deitado nao e consultado (as duas
// aparecem lado a lado).
static bool s_conv_frente = false;
static const int FRENTE_TOPO_A = 84;        // a faixa of VOLTAR e of name
static const int FRENTE_VOLTAR_L = 200, FRENTE_VOLTAR_A = 56, FRENTE_VOLTAR_Y = 14;

static bool em_pe(const area_t& a) { return a.width < a.height; }

// Corta 't' no end, por caractere (UTF-8), ate caber em max_l px in fonte
// que estiver active. Em pe quase todo text longo passa por aqui.
static void cortar_on_caber(lgfx::LovyanGFX& d, char* t, int max_l) { tab5_cortar_on_caber(d, t, max_l); }

// Escolhe a fonte suave of tamanho 'size' para bottom (passo 0,2) ate 't'
// caber em max_l; no menor tamanho, corta. Deixa a fonte escolhida active.
static void text_que_cabe(lgfx::LovyanGFX& d, char* t, int max_l, float size, float size_min)
{
  tab5_text(d, size);
  while (size > size_min && (int)d.textWidth(t) > max_l) {
    size -= 0.2f;
    tab5_text(d, size);
  }
  cortar_on_caber(d, t, max_l);
}

// Ordem of trilho: CONVERSAS first. E a pagina que se usa mais, e a que
// motiva open o app - as outras sao consulta.
// A ORDEM IMPORTA: ela e a mesma dos quatro primeiros glifos (GL_CONVERSAS,
// GL_NOS, GL_MALHA, GL_CONFIG), e o trilho converte a pagina em glifo por
// conversao direta. Trocar a order of one sem o outro poe o icon errado.
typedef enum { PAG_MSGS = 0, PAG_NOS, PAG_MALHA, PAG_NO, PAG_TOTAL } pagina_t;

static pagina_t s_pag = PAG_MSGS;

static int  s_conv = -1;              // conversa open (indice in coluna); -1 = none

// A CONVERSA ABERTA PELA CHAVE, e nao so pelo indice. O indice e a position in
// coluna, e a coluna se reordena (direta com message nova sobe, conversa
// nova, canal desabilitado no no). Guardando so o indice, a screen passava a
// mostrar e a ENVIAR para a conversa vizinha. Quando a versao of order muda
// (tab5_mesh_conversas_versao), o indice e reencontrado por esta chave.
typedef struct { bool ok; bool channel; uint8_t idx; uint32_t no; } chave_t;
static chave_t  s_ab = { };            // a conversa open (ok = false: none)
static uint32_t s_conv_ver = 0;        // a versao of order que s_conv segue

static bool chave_igual(const chave_t& a, const chave_t& b)
{
  if (a.ok != b.ok) { return false; }
  if (!a.ok)        { return true; }
  return a.channel == b.channel && (a.channel ? a.idx == b.idx : a.no == b.no);
}

static chave_t chave_of(int c)
{
  mesh_conversa_t m;
  if (c < 0 || !tab5_mesh_conversa(c, &m)) { return chave_t{ }; }
  return chave_t{ true, m.channel, m.idx, m.no };
}

// A CONVERSA - baloes, barra of escrita, keyboard e rolagem sao o componente of
// sistema (kernel/conversation). Ver conv_ler(), mais abaixo.
EXT_RAM_BSS_ATTR static conversation_t s_c;   // ~1 KB, so a interface toca: PSRAM
static bool s_eh_canal = false;       // conversa open e of canal (rotulo of remetente)
static void atualizar_eh_canal(void);

// Historico of conversa open. Em RAM para a screen draw rapido; a fonte of
// verdade e o cartao.
// HISTORICO NA PSRAM, e 200 em vez of 40.
//
// Eram 7,6KB of RAM INTERNA presos mesmo com o app closed - o maior simbolo
// estatico of sistema inteiro. Na PSRAM, alocado on open e devolvido on
// close, custa zero when ninguem esta conversando; e 200 mensagens sao
// 38KB, irrelevantes nos 29MB livres.
//
// O numero maior nao e capricho: sem rolagem, 40 bastavam porque so cabiam
// umas 8 in screen e o resto era inalcancavel. Com rolagem, o limit deixa of ser
// a screen e passa a ser este vetor - entao ele precisa valer a pena raise.
#define HIST_MAX 200
static mesh_msg_t* s_hist = nullptr;   // o que a screen mostra (so a interface le)
static chave_t     s_hist_chave = { }; // of qual conversa e o s_hist

// A CARGA DO HISTORICO, FORA DA INTERFACE (kernel/work). O
// worker le o cartao para ESTE buffer - nunca o que a screen desenha - e a
// interface so troca os dois no ao_aviso(AVISO_TRABALHO). Antes a leitura era
// feita in tarefa of interface a cada message nova: montar o SD e read a
// conversa inteira com a screen e o touch parados.
static mesh_msg_t*   s_carga = nullptr;
static volatile int  s_carga_n = 0;
static chave_t       s_carga_chave = { };   // pela chave: a order pode mudar ate o worker rodar
static bool          s_carga_cartao = false;   // o request inclui carregar_do_cartao
static volatile bool s_carga_pronta = false;
static bool          s_carga_requested = false;
static bool          s_reload   = false;   // pediram of novo durante a carga
static int  s_n_hist = 0;

// ---------------------------------------------------------------------------
// AS TRES LISTAS DO APP - todas kernel/list
// ---------------------------------------------------------------------------
// Mensagens, conversas e nos rolam cada uma in sua window; o trilho e a
// navigation ficam parados. Por isso nao e a rolagem of sistema
// (altura_conteudo), que rola a area inteira.
//
//   baloes   - a lista of componente of conversa (s_c.lista).
//   s_l_conv - coluna of conversas (ate MESH_MAX_CONVERSAS; cabem umas 7).
//   s_l_nos  - tabela of nos (ate MESH_MAX_NOS; cabem umas 16), abaixo of
//              cabecalho fixo.
//
// A lista cuida of rolar, recortar, draw so o exposto, a barra, e of
// touch (arraste e "qual item"). O app so diz a height e como draw one
// item. Ao switch of pagina, ir_para_pagina() esconde as listas of pagina
// que exited - uma lista que nao esta in screen nao responde a touch.
static list_t s_l_conv;
static list_t s_l_nos;
static bool s_hist_pronto = false;


// Repintura complete pending. Sem isto o app pisca: ele redesenha a 1Hz
// (periodo_ms = 1000) e cada pagina limpava a area inteira antes of write.
// Texto com color of fundo SOBRESCREVE, e nao pisca; quem pisca e o fillRect.
// Mesma licao of Ajustes e of barra of status.
static paint_t s_p;

// Atalho local: dentro of draw() todos os blocos perguntam "esta pass e
// complete?", e write pintura_completa(&s_p) em cada one ficaria ilegivel.
#define TUDO  paint_complete(&s_p)

// How many messages the service had the last time we read the card.
//
// Exists so we of NOT re-read the history on every repaint. Reading means
// mounting the card, scanning the file, and unmounting - expensive, and
// unnecessary when only the drawing changed. We only reload when the
// conversation changes or a new message arrives.
// int64_t, not uint32_t: -1 has to fit as "never read yet" and be
// different from any real tab5_mesh_total_msgs() (which starts at 0)
// - see the big warning next to its use, further below.
static int64_t s_msgs_when_li = -1;

// Versao dos estados of send_state que a screen ja desenhou.
static uint32_t s_envios_ver = 0;

// Roda NO WORKER. So toca s_carga*: a screen nao le nada daqui.
static void work_carregar(void*)
{
  if (s_carga_cartao) { tab5_mesh_carregar_of_cartao(); }
  const chave_t k = s_carga_chave;
  s_carga_n = k.ok ? tab5_mesh_historico_of(k.channel, k.idx, k.no, s_carga, HIST_MAX) : 0;
  s_carga_pronta = true;
}

static bool alocar_hist(void)
{
  if (!s_hist)  { s_hist  = (mesh_msg_t*)heap_caps_malloc(sizeof(mesh_msg_t) * HIST_MAX, MALLOC_CAP_SPIRAM); }
  if (!s_carga) { s_carga = (mesh_msg_t*)heap_caps_malloc(sizeof(mesh_msg_t) * HIST_MAX, MALLOC_CAP_SPIRAM); }
  return s_hist && s_carga;
}

// PEDE a carga (nao le nada aqui). Conversa diferente of que esta in screen: a
// screen esvazia in hora, em vez of mostrar a conversa anterior ate a carga
// chegar.
static void carregar_hist(void)
{
  s_hist_pronto = true;   // request feito; o resultado chega por aviso
  if (!alocar_hist()) { s_n_hist = 0; return; }
  if (!chave_igual(s_hist_chave, s_ab)) {
    s_n_hist = 0;
    s_hist_chave = s_ab;
    atualizar_eh_canal();
    conversation_changed(&s_c);
  }
  if (s_carga_requested) { s_reload = true; return; }
  s_carga_chave  = s_ab;
  s_carga_pronta = false;
  s_carga_requested = system_work(work_carregar, nullptr);
  if (s_carga_requested) { s_carga_cartao = false; }
  else                { s_hist_pronto = false; }   // queue cheia: tenta in next pass
}

// Na interface. Carga terminou -> troca os buffers; qualquer outro aviso
// (message, send_state, nos) -> redesenha, e o draw() confere os contadores.
static void on_aviso(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_carga_pronta) {
    s_carga_pronta = false;
    s_carga_requested = false;
    if (chave_igual(s_carga_chave, s_ab)) {
      mesh_msg_t* t = s_hist; s_hist = s_carga; s_carga = t;
      s_n_hist     = s_carga_n;
      s_hist_chave = s_carga_chave;
      conversation_changed(&s_c);
    }
    // Conversa trocou durante a carga, ou chegou message no middle: of novo.
    if (!chave_igual(s_carga_chave, s_ab) || s_reload) {
      s_reload = false;
      s_hist_pronto = false;
    }
    // A primeira carga tambem trouxe nomes e conversas of cartao: a coluna of
    // conversas precisa ser repintada inteira.
    paint_request(&s_p);
    system_request_redraw_complete();
    return;
  }
  system_request_redraw();
}

// ---------------------------------------------------------------------------
// Desenho
// ---------------------------------------------------------------------------
// "caixa" e o name que este app ja usava para tab5_botao() (ver tab5_ui.hpp) -
// mantido aqui como apelido fino para nao renomear cada chamada. Ate
// isto era outra copia local of mesmo botao que existia em tres
// apps a mais.
static void caixa(lgfx::LovyanGFX& d, int x, int y, int l, int a,
                  const char* txt, uint16_t fundo, uint16_t tinta, float size)
{
  tab5_botao(d, x, y, l, a, txt, fundo, tinta, size);
}

// ---------------------------------------------------------------------------
// GLIFOS DAS COLUNAS
// ---------------------------------------------------------------------------
// As letras "C", "N" e "M" eram a fonte embutida em tamanho 4 - cada pixel dela
// virava one quadrado of 4, e era isso que serrilhava. Mesma solucao dos icones
// of screen inicial: primitivas em fracao of tamanho, que saem limpas em qualquer
// escala e ainda dizem o que a letra so abreviava.
//
// A COR VEM DE FORA porque cada glifo tem dois estados: selected desenha em
// preto sobre o azul of caixa, nao selected em branco sobre o fundo escuro.
// O drawing e o mesmo nos dois - so a tinta muda.
typedef enum { GL_CONVERSAS = 0, GL_NOS, GL_MALHA, GL_CONFIG, GL_DIFUSAO, GL_DIRETO } glifo_t;

// ENGRENAGEM: o sinal of "aqui se configura" - no trilho e on lado of cada
// opcao que of para alterar. Oito dentes em lines grossas, o body e o furo
// com a color of fundo (pass por quem chama).
static void engrenagem(lgfx::LovyanGFX& d, int cx, int cy, int r, uint16_t color, uint16_t fundo)
{
  int dente = r * 2 / 5;
  if (dente < 2) { dente = 2; }
  for (int k = 0; k < 8; ++k) {
    const float ang = (float)M_PI * k / 4.0f;
    tab5_line_grossa(d, cx + (int)(r * 0.55f * cosf(ang)), cy + (int)(r * 0.55f * sinf(ang)),
                   cx + (int)(r * cosf(ang)),         cy + (int)(r * sinf(ang)), dente, color);
  }
  d.fillCircle(cx, cy, r * 3 / 4, color);
  d.fillCircle(cx, cy, r / 3, fundo);
}

static void glifo(lgfx::LovyanGFX& d, glifo_t g, int cx, int cy, int lado, uint16_t color)
{
  const int r = lado / 2;
  int gr = lado / 12;                       // espessura base
  if (gr < 2) { gr = 2; }

  switch (g) {
    case GL_CONVERSAS: {
      // Balao of fala: body arredondado e a ponta embaixo a left. E o
      // drawing universal of "aqui se conversa".
      const int l = (int)(r * 1.5f), a = (int)(r * 1.1f);
      d.drawRoundRect(cx - l/2, cy - a/2 - r/6, l, a, a/3, color);
      d.drawRoundRect(cx - l/2 + 1, cy - a/2 - r/6 + 1, l - 2, a - 2, a/3, color);
      d.fillTriangle(cx - l/6, cy + a/2 - r/6 - 1,
                     cx - l/6 + a/3, cy + a/2 - r/6 - 1,
                     cx - l/3, cy + a/2 + r/3, color);
      break;
    }
    case GL_NOS: {
      // Lista: tres pontos, cada one com a sua line. E one CADASTRO - foi essa a
      // diferenca que faltava para nao se confundir com a malha.
      for (int i = -1; i <= 1; ++i) {
        const int y = cy + i * (r / 2);
        d.fillCircle(cx - r * 3 / 5, y, gr, color);
        d.fillRect(cx - r / 5, y - gr / 2, r, gr, color);
      }
      break;
    }
    case GL_MALHA: {
      // Grafo: one no no centro e quatro em iteration, ligados. Aqui a informacao e
      // a LIGACAO, e nao a lista - por isso as arestas sao o que se ve first.
      const int d4[4][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,1} };
      for (int i = 0; i < 4; ++i) {
        const int x = cx + d4[i][0] * r * 3 / 5;
        const int y = cy + d4[i][1] * r * 3 / 5;
        d.drawLine(cx, cy, x, y, color);
        d.fillCircle(x, y, gr + 1, color);
      }
      d.fillCircle(cx, cy, gr + 2, color);
      break;
    }
    case GL_CONFIG: {
      // A color of fundo of furo: preto sobre o azul of caixa selecionada,
      // o fundo of trilho in nao selecionada - a tinta diz qual e.
      engrenagem(d, cx, cy, r * 3 / 4, color, color == TFT_BLACK ? TFT_SKYBLUE : d.color565(13, 17, 23));
      break;
    }
    case GL_DIFUSAO: {
      // Difusao: one ponto e ondas saindo dele para os dois lados. Fala para
      // todo mundo, e nao para alguem - e a diferenca que o icon precisa
      // carregar contra o GL_DIRETO.
      d.fillCircle(cx, cy, gr + 1, color);
      for (int k = 1; k <= 2; ++k) {
        const int rr = k * r / 2;
        d.drawArc(cx, cy, rr, rr - gr + 1,  30,  150, color);
        d.drawArc(cx, cy, rr, rr - gr + 1, 210,  330, color);
      }
      break;
    }
    case GL_DIRETO: {
      // Uma pessoa: cabeca e ombros. Conversa direta e com ALGUEM.
      d.fillCircle(cx, cy - r / 3, r / 3, color);
      d.fillArc(cx, cy + r * 3 / 4, r * 2 / 3, r * 2 / 3 - gr * 2, 180, 360, color);
      break;
    }
  }
}

// Caixa com glifo no lugar of text. Mesma geometria e mesmas cores of caixa()
// of text, para os dois conviverem sem desalinhar.
static void caixa_glifo(lgfx::LovyanGFX& d, int x, int y, int l, int a,
                        glifo_t g, uint16_t fundo, uint16_t tinta)
{
  d.fillRoundRect(x, y, l, a, 10, fundo);
  const int lado = (l < a ? l : a) * 3 / 5;
  glifo(d, g, x + l / 2, y + a / 2, lado, tinta);
}

// ---------------------------------------------------------------------------
// A MARCA DO MESHTASTIC
// ---------------------------------------------------------------------------
// Desenhada com primitivas e nao guardada como imagem: assim ela exit em
// qualquer tamanho sem serrilhar, nao ocupa flash, e o mesmo code serve para
// o icon of 190px of screen inicial e para o of 48px of trilho.
//
// A geometria exited of SVG oficial (meshtastic/design, logo/logo.svg): fundo
// verde rgb(103,234,148) e duas cristas em rgb(44,45,60) - a crista cheia a
// right e uma subida released a left, que e a perna of one segundo pico atras
// dela. O drawing e inspirado nos chirps of LoRa e le como montanha, tenda e a
// letra M on mesmo time; a ambiguidade e proposital no original.
//
// Coordenadas em FRACAO of lado, nao em pixels. E o que permite o mesmo traco
// nos dois tamanhos sem uma segunda tabela para manter em dia.
static void marca_mesh(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t verde  = d.color565(103, 234, 148);
  const uint16_t escuro = d.color565(44, 45, 60);

  d.fillRoundRect(x, y, lado, lado, lado / 6, verde);

  // ESPESSURA MEDIDA NO SVG, nao estimada.
  //
  // A primeira versao usava 9% of lado e exited gorda. No original o traco tem
  // ~14,7 unidades e a crista ~140 of width: 10,5% DA CRISTA. Como a crista
  // ocupa cerca of metade of icon, sobra ~5% of lado - quase metade of
  // primeira estimativa.
  //
  // O piso of 3px continua: abaixo disso a marca some em vez of encolher.
  int grosso = (lado * 5) / 100;
  if (grosso < 3) { grosso = 3; }

  #define MX(f) (x + (int)((f) * lado))
  #define MY(f) (y + (int)((f) * lado))

  // Crista principal. Proporcao of original: ~0,49 of width por ~0,36 of
  // height - mais achatada of que a primeira versao.
  tab5_line_grossa(d, MX(0.365f), MY(0.72f), MX(0.610f), MY(0.34f), grosso, escuro);
  tab5_line_grossa(d, MX(0.610f), MY(0.34f), MX(0.855f), MY(0.72f), grosso, escuro);

  // A subida released a left: MESMA BASE, MESMO TOPO e MESMA inclinacao of
  // perna left of crista. Ela e a perna of one segundo pico atras, e one pico
  // atras tem a height of of frente - stopping no middle of caminho, como estava,
  // ela virava one risco released em vez of montanha.
  //
  // Sendo paralelas, as duas nunca se cruzam por construcao: a separacao e
  // constante em toda a height, e nao depende of escolha exata dos
  // pontos.
  tab5_line_grossa(d, MX(0.120f), MY(0.72f), MX(0.365f), MY(0.34f), grosso, escuro);

  #undef MX
  #undef MY
}

// Declaradas aqui, definidas mais abaixo perto of desenhar_msgs() (onde
// nasceram) - trilho e coluna of conversas tambem precisam of
// mesma paleta now, e vem ANTES no arquivo.
static uint16_t background_msg(lgfx::LovyanGFX& d);

static void draw_trilho(lgfx::LovyanGFX& d, const area_t& a)
{
  // Era SIS_FUNDO (a paleta of sistema): as colunas sao
  // MOLDURA, nao content, e tinham que recuar como a barra recua. Trocado
  // pela paleta of proprio Mesh (fundo_msg) - para o trilho, a lista of conversas e o
  // panel of mensagens lerem como UMA SO screen, nao duas paletas coladas.
  tab5_screen_clear(a.x, a.y, TRILHO_L, a.height, background_msg(d));
  // RETANGULO INTEIRO, e nao so a line of right. Com uma borda so, a coluna
  // parecia sangrar para fora of area em top e embaixo; closed, ela vira uma
  // peca.
  //
  // SIS_BORDA e nao branco: o branco puro tinha o maior contraste possivel
  // contra o fundo escuro e a moldura gritava mais que o content dela. Este
  // cinza se ve sem disputar.
  d.drawRect(a.x, a.y, TRILHO_L, a.height, SYSTEM_BORDER);
  const int nao_lidas = tab5_mesh_nao_lidas();
  const int diretas   = tab5_mesh_nao_lidas_diretas();
  const uint16_t FUNDO = background_msg(d);

  for (int i = 0; i < PAG_TOTAL; ++i) {
    const int y = a.y + 16 + i * (ITEM_A + 12);
    const bool sel = ((int)s_pag == i);
    // Fundo of icon NAO selected: era TFT_BLACK puro (achado em
    // "as imagens... com o fundo antigo") e destoava of trilho
    // ja recolorido on redor dele. Agora acompanha a mesma paleta.
    caixa_glifo(d, a.x + 12, y, TRILHO_L - 24, ITEM_A, (glifo_t)i,
                sel ? TFT_SKYBLUE : FUNDO, sel ? TFT_BLACK : TFT_WHITE);

    // PONTO, NAO NUMERO, e a diferenca e of proposito.
    //
    // O numero respondia "quantas" - uma pergunta que a coluna of conversas ja
    // responde melhor, conversa por conversa. Aqui, num icon of 66px, ele so
    // competia por espaco com a letra e ainda precisava erase o proprio fundo.
    //
    // O que o trilho precisa dizer e "tem coisa nova ali", e para isso one ponto
    // basta - o olho pega of relance, sem read. Vermelho when ha message
    // DIRETA: endereçada a voce nao e a mesma coisa que trafego of canal.
    if (i == PAG_MSGS && nao_lidas > 0) {
      const int px = a.x + TRILHO_L - 24;
      const int py = y + 12;
      d.fillCircle(px, py, 7, diretas > 0 ? TFT_RED : TFT_SKYBLUE);
      d.drawCircle(px, py, 7, TFT_BLACK);      // contorno: destaca no fundo claro
    }
  }
  // Link state at the foot of the rail: bright green, dull red. It's the
  // answer to "is it connected?" without leaving whatever page you're on.
  const bool ok = tab5_mesh_no_ar();

  // MARK BELOW, DOT ABOVE. Together they answer "which network is this" and
  // "is it alive" without taking up a single line of text - but the mark is
  // the app's identity and closes off the rail's foot better; the dot is
  // state, and state belongs up top.
  const int ml    = TRILHO_L - 36;
  const int marca_y = a.y + a.height - 18 - ml;   // mesma margem of pe of antes
  const int led_cy  = marca_y - 22 - 12;          // mesmo respiro of 22px

  d.fillCircle(a.x + TRILHO_L / 2, led_cy, 12, ok ? TFT_GREENYELLOW : TFT_RED);
  marca_mesh(d, a.x + (TRILHO_L - ml) / 2, marca_y, ml);
}

// A COLUNA DE CONVERSAS E UMA kernel/list. Item of height fixa;
// a lista rola, recorta, desenha a faixa exposta, a barra e responde on touch.
// A coluna so e repintada por inteiro em pass complete (a unica hora em que
// nomes, selecao e nao-lidas mudam); fora disso, so a rolagem.
static const int CONV_PASSO = ITEM_A + 8;

// O TRACO DA INTERNET: antes of primeira conversa of rede publica (que a order
// of servico poe por ultimo), uma line com "INTERNET". Faz parte of proprio item -
// a lista so conhece alturas of item - e o touch desconta a height dele.
static const int CONV_DIV_A = 34;
static int s_conv_div = -1;   // indice of item que leva o traco; -1 = nenhum

static int conv_height(int i, void*) { return CONV_PASSO + (i == s_conv_div ? CONV_DIV_A : 0); }

static int achar_conv_div(void)
{
  const int n = tab5_mesh_num_conversas();
  for (int i = 0; i < n; ++i) {
    mesh_conversa_t c;
    if (tab5_mesh_conversa(i, &c) && c.channel && mesh_canal_internet(c.idx)) { return i; }
  }
  return -1;
}

// O que cada item mostra now (name e nao-lidas), para a coluna perceber
// sozinha o que changed - antes, o contador of uma conversa que nao era a open
// so aparecia in next pass complete.
typedef struct { char name[MESH_NOME_CONVERSA]; char sub[MESH_NOME_LONGO]; int nao_lidas; } conv_visto_t;
static conv_visto_t s_conv_visto[MESH_MAX_CONVERSAS];

static void conv_draw(lgfx::LovyanGFX& d, int i, int x, int y, int width, void*)
{
  mesh_conversa_t c;
  if (!tab5_mesh_conversa(i, &c)) { return; }
  if (i < MESH_MAX_CONVERSAS) {
    snprintf(s_conv_visto[i].name, sizeof(s_conv_visto[i].name), "%s", c.name);
    snprintf(s_conv_visto[i].sub, sizeof(s_conv_visto[i].sub), "%s", c.sub);
    s_conv_visto[i].nao_lidas = c.nao_lidas;
  }
  const uint16_t FUNDO = background_msg(d);
  if (i == s_conv_div) {
    const int ly = y + CONV_DIV_A / 2;
    tab5_text(d, 1.9f);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_SKYBLUE, FUNDO);
    d.drawString("INTERNET", x + 12, ly);
    const int tx = x + 12 + (int)d.textWidth("INTERNET") + 10;
    d.drawFastHLine(tx, ly, width - (tx - x) - 10, TFT_SKYBLUE);
    d.setTextDatum(top_left);
    y += CONV_DIV_A;
  }
  // O item comeca pelo respiro of 8px: o contador of nao-lidas sobe 6px acima
  // of icon e precisa estar DENTRO of proprio item - a lista so repinta o
  // item que cruza a faixa exposta, e one pedaco fora dele ficaria sem dono.
  y += 8;
  const bool sel = (i == s_conv);

  // O TIPO DA CONVERSA, e nao a inicial of name. A inicial em tamanho 4 era a
  // fonte embutida ampliada, serrilhada, e dizia menos of que parecia: o name
  // inteiro esta escrito on lado, entao a inicial repetia informacao. O que a
  // coluna nao dizia era se aquilo e one CANAL (todo mundo ouve) ou alguem
  // especifico - e e isso que muda como voce escreve.
  // Cor of icon of TIPO (canal/direto) mantida saturada of proposito - e one
  // indicador funcional (distinguir os dois tipos of conversa of relance),
  // nao "a color of bubble"; deixei fora of recolorimento por
  // falta of referencia clara no que foi mandado (a captura of screen nao
  // mostra esta screen). Os avatares coloridos por remetente (cor_do_no), que
  // SAO equivalentes on que a captura mostra, ja seguem a paleta nova.
  caixa_glifo(d, x + 10, y, ITEM_A, ITEM_A, c.channel ? GL_DIFUSAO : GL_DIRETO,
              sel ? TFT_SKYBLUE : (c.channel ? TFT_NAVY : TFT_DARKGREEN),
              sel ? TFT_BLACK : TFT_WHITE);

  // NOME COM LARGURA QUE CABE, measured of verdade: o text comeca em
  // (10 + ITEM_A + 8) = 90px dentro of coluna, o divisor esta em CONV_L-1 =
  // 199. Antes disto era "12px por caractere" (a width fixa of Font0);
  // a Font4 e proporcional, entao o corte now mede com d.textWidth() e
  // tira uma letra of cada vez ate caber - a mesma tecnica usada no resto
  // of sistema since a troca of fonte. O acolchoamento com
  // espacos a right exited: a lista limpa a faixa of item antes of
  // desenha-lo, entao nao sobra resto of name antigo para erase.
  const int name_x = x + 10 + ITEM_A + 8;
  const int name_max_l = width - (name_x - x);
  // +60%. RISCO
  // ACEITO: nome_max_l nao changed (a coluna continua com 200px), entao one
  // name que cabia inteiro antes pode passar a truncar uma letra a mais -
  // o corte por d.textWidth() logo abaixo ja protege contra estourar a
  // coluna, so mostra menos caracteres se precisar.
  tab5_text(d, 3.84f);   // +60%
  d.setTextDatum(top_left);
  d.setTextColor(sel ? TFT_SKYBLUE : TFT_WHITE, FUNDO);
  char name[24];
  snprintf(name, sizeof(name), "%s", c.name);
  tab5_cortar_on_caber(d, name, name_max_l);   // por caractere: o name pode ter acento
  d.drawString(name, name_x, y + 12);

  // SEGUNDA LINHA: o name longo of quem e a direta (ou "canal N"). E o que
  // responde "quem e" when o curto e uma sigla - sem recorrer on id. Na
  // conversa open ela para antes of botao of erase, que mora nesta height.
  if (c.sub[0]) {
    tab5_text(d, 2.4f);
    d.setTextColor(TFT_DARKGREY, FUNDO);
    const int sub_max_l = sel ? (erase_dx() - (name_x - x) - 6) : name_max_l;
    char sub[MESH_NOME_LONGO];
    snprintf(sub, sizeof(sub), "%s", c.sub);
    tab5_cortar_on_caber(d, sub, sub_max_l);   // por caractere: o longo pode ter acento
    d.drawString(sub, name_x, y + 46);
  }

  // Contador sobre o CANTO of icon, como em aplicativo of message. Antes
  // ele ficava a right of name e disputava espaco com ele - e o name sempre
  // ganhava, porque e drawn depois.
  if (c.nao_lidas > 0) {
    char nl[8]; snprintf(nl, sizeof(nl), "%d", c.nao_lidas);
    caixa(d, x + 10 + ITEM_A - 24, y - 6, 30, 26, nl, TFT_RED, TFT_WHITE, 2);
  }

  // APAGAR: so in conversa ABERTA - canal TAMBEM.
  //
  // So in open para nao encher a coluna of botoes vermelhos - voce apaga a
  // que esta olhando, que e a que acabou of perceber que nao queria. Ate
  // canal nao ganhava este botao (one canal apagado voltava sozinho) - deixou of ser verdade when canal passou a entrar in
  // mesma flag of apagados que ja existia para no (ver o aviso grande em
  // tab5_mesh.cpp, junto dela). Reabrir depois: pela secao "CANAIS
  // DISPONIVEIS" in pagina of Nos, ou so esperando message nova chegar.
  if (sel) {
    caixa(d, x + erase_dx(), y + APAGAR_DY, APAGAR_L, APAGAR_A,
          "x", TFT_MAROON, TFT_WHITE, 2);
  }
}

// A window of coluna: por DENTRO of moldura (1px) e acima of keyboard, que
// cobre a width inteira when open. O que o keyboard cobre nao rola nem
// responde - a lista so conhece o que desenhou.
// A width of coluna exit daqui e stays gravada em s_col_l: o item (o "x" of
// erase) e o touch leem o mesmo numero que a moldura usou.
static area_t window_conversas(const area_t& a)
{
  s_col_l = em_pe(a) ? a.width - TRILHO_L : CONV_L;
  const int alt = a.height - (conversation_keyboard_open(&s_c) ? virtual_keyboard_height(a) : 0);
  return area_t{ a.x + TRILHO_L + 1, a.y + 1, s_col_l - 2, alt - 2 };
}

static void draw_conversas(lgfx::LovyanGFX& d, const area_t& a)
{
  const uint16_t FUNDO = background_msg(d);
  const area_t j = window_conversas(a);
  const int n = tab5_mesh_num_conversas();
  const int div = achar_conv_div();
  const bool div_changed = (div != s_conv_div);
  s_conv_div = div;

  if (TUDO || n != s_l_conv.n || div_changed) {
    list_items_changed(&s_l_conv, n);
  } else {
    for (int i = 0; i < n && i < MESH_MAX_CONVERSAS; ++i) {
      mesh_conversa_t c;
      if (!tab5_mesh_conversa(i, &c)) { continue; }
      if (c.nao_lidas != s_conv_visto[i].nao_lidas || strcmp(c.name, s_conv_visto[i].name) != 0
       || strcmp(c.sub, s_conv_visto[i].sub) != 0) {
        list_item_changed(&s_l_conv, i);
      }
    }
  }
  const bool pintou = list_draw(&s_l_conv, d, j, FUNDO, TUDO);

  // Lista vazia repintada (pass complete, ou a ultima conversa apagada):
  // o aviso vai por top of fundo que a lista acabou of paint.
  if (n == 0 && pintou) {
    tab5_text(d, 2.4f);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_LIGHTGREY, FUNDO);
    d.drawString("no conversations", j.x + 11, j.y + 19);
  }
  // Moldura FORA of window of lista: nada que a lista pinte a alcanca, e
  // basta desenha-la in pass complete. No cinza of moldura of sistema.
  if (TUDO) { d.drawRect(a.x + TRILHO_L, a.y, s_col_l, j.height + 2, SYSTEM_BORDER); }
}

// "HA QUANTO TEMPO", curto.
//
// E a pergunta que o app of celular responde e a nossa lista nao respondia:
// SNR alto num no que ninguem ouve ha dois dias nao quer dizer nada. Formato
// curto of proposito - a coluna e estreita e o que importa e a order of
// grandeza, nao o segundo exato.
static void idade_text(int32_t s, char* saida, size_t size)
{
  if      (s < 0)     { snprintf(saida, size, "?");     }
  else if (s < 90)    { snprintf(saida, size, "now"); }
  else if (s < 3600)  { snprintf(saida, size, "%dm", (int)(s / 60)); }
  else if (s < 86400) { snprintf(saida, size, "%dh%02d",
                                 (int)(s / 3600), (int)((s % 3600) / 60)); }
  else                { snprintf(saida, size, "%dd", (int)(s / 86400)); }
}

// Verde ate 15 min, amarelo ate 2h, cinza depois. As 2h sao a fronteira que o
// proprio Meshtastic usa para chamar one no of "online" - vale copiar, para a
// lista aqui e a of celular contarem a mesma historia.
static uint16_t idade_color(int32_t s)
{
  if (s < 0)    { return TFT_DARKGREY; }
  if (s <= 900) { return TFT_GREENYELLOW; }
  if (s <= 7200){ return TFT_YELLOW; }
  return TFT_DARKGREY;
}

// ---------------------------------------------------------------------------
// A GRADE DAS COLUNAS
// ---------------------------------------------------------------------------
// Cabecalho e lines sao montados a partir DESTA tabela, e so dela. E a unica
// forma of os dois nao se descolarem - e eles tinham se descolado.
//
// Antes a line era one formato unico com os vaos contados a mao:
//
//   "%-22.22s  %-6.6s %5.1f %3u%%   %u   "
//
// Repare nos espacos: 2, 1, 1, 3, 3. Cada coluna com one respiro diferente, e o
// cabecalho era OUTRA string, com a sua propria contagem - entao "snr" caia one
// caractere fora dos valores e "visto" encostava em "saltos". Mexer numa das
// duas sem mexer in outra desalinhava everything of novo.
//
// Regra unica now: vao of 2 caracteres entre colunas, e o titulo alinha of
// mesmo jeito que o valor - numero a right, text a left.
typedef struct {
  int         col;      // caractere onde a coluna comeca
  int         larg;
  bool        right;  // alinhamento of valor E of titulo
  const char* titulo;
} coluna_t;

enum { C_NOME = 0, C_CURTO, C_ID, C_HW, C_SNR, C_BAT, C_SALTOS, C_VISTO, C_TOTAL };

// IDENTIDADE E ETIQUETA, LADO A LADO.
//
// "name" e "curto" sao ETIQUETAS: quem renomeia o no no celular troca as duas.
// Nao dizem com que aparelho voce esta falando, e era so isso que a tabela
// mostrava. As duas colunas novas dizem:
//
//   id  - "!a1b2c3d4", derivado of MAC, acompanha o radio para sempre. E o
//         identificador que o Meshtastic usa em todo lugar: log, mapa, celular.
//   hw  - qual placa e (T-Beam, Heltec V3...). Explica sozinho por que one no
//         tem battery e outro nao, por que one anda e outro nao.
//
// A screen e of 1280px em paisagem. A ultima coluna termina no caractere 85
// (com os vaos of 2 caracteres corrigidos), que of 1126px com a margem e o
// x0 of 90px of trilho - cabe com ~150px of folga para o indicador of
// rolagem e ainda sobra chao para uma coluna futura.
// OS VAOS ERAM DE 1 CARACTERE, NAO DE 2 - o comentario acima descrevia a
// regra certa, mas os numeros abaixo nao a seguiam (name termina em 20,
// "curto" comecava em 21 - so 1 of vao, nao 2). Numa tabela of width fixa,
// field direita-alinhado colado a 12px of next field esquerda-alinhado e
// exatamente o que le como "torto": o valor of uma coluna quase encosta no
// text of vizinha, sem o respiro visual que separa "isto e uma coluna" of
// "isto e a next". Refeita com os 2 caracteres que a regra sempre pediu.
static const coluna_t COLUNA[C_TOTAL] = {
  {  0, 20, false, "name"   },
  { 22,  5, false, "curto"  },
  { 29, 10, false, "id"     },
  { 41, 15, false, "modelo" },
  { 58,  5, true,  "snr"    },
  { 65,  4, true,  "bat"    },
  { 71,  6, true,  "saltos" },
  { 79,  6, false, "visto"  },
};

// A MESMA GRADE EM PE. Em 720 px, tirando o trilho, cabem ~48 caracteres -
// nao os 85 of top. Ficam as colunas que respondem "of para falar com ele
// now?": name, sinal, battery, saltos e ha quanto time foi ouvido. Saem
// curto, id e modelo (width 0: field() nao escreve nada), que continuam a
// one touch, no cartao of no. Mesma regra of vao of 2 caracteres.
static const coluna_t COLUNA_PE[C_TOTAL] = {
  {  0, 16, false, "name"   },
  {  0,  0, false, "curto"  },
  {  0,  0, false, "id"     },
  {  0,  0, false, "modelo" },
  { 18,  5, true,  "snr"    },
  { 25,  4, true,  "bat"    },
  { 31,  6, true,  "saltos" },
  { 39,  6, false, "visto"  },
};

// A grade em uso: escolhida por desenhar_nos() a cada pass, pela area.
static const coluna_t* s_grade = COLUNA;

// Fonte 6x8 no tamanho 2 = 12px por caractere.
static const int NOS_CH = 12;
// A line principal vai ate o end of "saltos"; "visto" e drawn a parte,
// porque tem color propria. NOS_LINHA_CH e o MAIOR caso (deitado) - o tamanho
// dos buffers; o comprimento of now vem of grade.
#define NOS_LINHA_CH  77        // COLUNA[C_SALTOS].col + larg
static int line_ch(void) { return s_grade[C_SALTOS].col + s_grade[C_SALTOS].larg; }
// Margem left of tabela: a coluna of estrela (favorito) mora nela, e com
// 16 px a estrela encostava in borda of area.
static const int NOS_MARGEM  = 34;
static const int NOS_ESTRELA_X = 16;              // centro of estrela, dentro of margem
static int visto_x(void) { return NOS_MARGEM + s_grade[C_VISTO].col * NOS_CH; }

// OS CANAIS NO TOPO DA PAGINA DE NOS - todos os ativos of no, como botoes.
//
// Antes era uma lista of "canais disponiveis": so os SEM conversa in coluna,
// e so os que a configuracao of radio tinha marcado como ativos - e o canal 0
// nunca chegava a ser marcado (ver o leitor of canais em tab5_mesh.cpp). Um
// LongFast apagado of coluna nao tinha onde ser achado. Agora todo canal active
// esta sempre aqui; o que nao esta in coluna aparece apagado, com "+".
//
// A geometria e GRAVADA no drawing e LIDA no touch (s_chips): os dois nunca
// discordam, nem com nomes of larguras diferentes.
typedef struct { int x, y, l, a; uint8_t idx; } chip_t;   // coordenadas LOCAIS of area
static chip_t s_chips[MESH_MAX_CANAIS + 1];
static int    s_n_chips = 0;
static int    s_chips_height = 0;          // height of faixa dos canais, a partir of topo
static char   s_chips_assin[(MESH_MAX_CANAIS + 1) * (MESH_NOME_CONVERSA + 4) + 48];

// QUAL REDE A TABELA MOSTRA: no ou internet (botoes no topo of pagina). Na
// vista INTERNET, o botao of enable/disable a rede publica e o canal of la.
static uint8_t s_rede_vista = MESH_REDE_NO;
static chip_t  s_chip_rede[2];
static chip_t  s_chip_internet_liga;
static const int CHIP_A = 36;

// NO ESCOLHIDO NA TABELA: a pagina mostra o cartao dele em vez of tabela.
// Pelo NUMERO, nao pelo indice - a tabela se reordena por "visto".
static uint32_t s_no_sel = 0;

// Escreve one field in sua coluna dentro of 'line', que ja chega cheia of
// espacos. memcpy e nao snprintf: one NUL no middle cortaria o resto of line.
// O strnlen limitado a width tambem e o que impede one name longo of invadir
// a coluna seguinte.
static void field(char* line, const coluna_t& c, const char* txt)
{
  const int n = (int)strnlen(txt, (size_t)c.larg);
  memcpy(line + c.col + (c.right ? c.larg - n : 0), txt, (size_t)n);
}

static void espacos(char* line, size_t size)
{
  memset(line, ' ', size - 1);
  line[size - 1] = 0;
}

// Troca cada sequencia UTF-8 of varios bytes (acento, emoji, qualquer coisa
// fora of ASCII puro) por UM UNICO '?'. Existe porque field()/montar_linha()
// constroem a line como uma GRADE DE LARGURA FIXA POR BYTE (1 byte = 1
// coluna of screen) - one name com emoji (4 bytes UTF-8 desenhando UM icon so)
// ou so one acento (2 bytes por 1 letra, "Jose" -> "José") descola a conta:
// os bytes "extras" empurram os campos seguintes DAQUELA LINHA para a
// right, sem empurrar as outras - e era exatamente o desalinhamento
// observado in tabela of nos.
//
// Nome of no vem of OUTRO radio, sem controle nosso, entao a defesa stays
// aqui, in entrada of tabela - nao adianta request para quem manda a message
// n usar emoji. Nas CONVERSAS, onde o text corre livre (sem grade of
// coluna), o emoji continua aparecendo of verdade - ver tab5_emoji_preparar().
static void sanitizar_ascii(const char* in, char* out, size_t out_size)
{
  if (out_size == 0) { return; }
  size_t o = 0;
  const unsigned char* p = (const unsigned char*)in;
  while (*p && o + 1 < out_size) {
    if (*p < 0x80) { out[o++] = (char)*p; ++p; continue; }
    // Lead byte of sequencia multi-byte: pula ela INTEIRA of uma vez, senao
    // cada byte of continuacao viraria one '?' proprio - one emoji so ja vira
    // 4 pontos of interrogacao em vez of 1.
    const int n = ((*p & 0xE0) == 0xC0) ? 2
                : ((*p & 0xF0) == 0xE0) ? 3
                : ((*p & 0xF8) == 0xF0) ? 4
                : 1;    // byte of continuacao released (UTF-8 invalido): 1 so
    out[o++] = '?';
    p += n;
  }
  out[o] = 0;
}

static void montar_line(char* line, size_t size, const mesh_no_t& no)
{
  char v[24];
  char name_limpo[MESH_NOME_LONGO];
  char curto_limpo[MESH_NOME_CURTO];
  espacos(line, size);

  // LONG: same logic as the short one just below - prefers the LIVE entry
  // (this session's NodeInfo); without it, falls back to the persistent
  // cache (tab5_mesh_nome_longo_cache(), nos.txt). This column used to
  // only look at no.longo - the same class of bug the short one had
  // (see its warning just below), except never fixed here: most nodes are
  // born from telemetry/position/text, not from NodeInfo (the ONLY source
  // of the long name), so the table showed "(no name)" almost always.
  const char* longo_fonte = no.longo;
  char longo_cache[MESH_NOME_LONGO];
  if (!no.longo[0] && tab5_mesh_name_longo_cache(no.num, longo_cache, sizeof(longo_cache))) {
    longo_fonte = longo_cache;
  }
  sanitizar_ascii(longo_fonte[0] ? longo_fonte : "(no name)", name_limpo, sizeof(name_limpo));

  // CURTO: prefere o que ESTE no informou on vivo (NodeInfo desta sessao).
  // Sem isso, cai para o cache persistente (nos.txt) - o mesmo cache que a
  // conversa ja consulta via tab5_mesh_nome_exibicao(), mas aqui SEM o
  // fallback of hex dela (ver tab5_mesh_nome_cache() e o aviso complete no
  // .hpp): a coluna ID logo abaixo ja mostra o hex when falta name, entao
  // repeti-lo aqui in coluna of name curto seria redundante.
  //
  // ACHADO (a maioria das lines saia sem name longo nem curto): esta tabela so olhava a entrada AO VIVO (no.curto). A maior
  // parte das entries nasce of achar_no() sendo chamado por one pacote of
  // TELEMETRIA/POSICAO/TEXTO (ver ler_packet()/ler_data() no servico), nao
  // por one NodeInfo - e NodeInfo (o UNICO tipo que traz name) e bem mais
  // raro in malha que os outros. Um no com quem ja se conversou antes (name
  // gravado no cache ha muito time) aparecia em branco of novo, so porque a
  // entrada on vivo of AGORA ainda nao recebeu one NodeInfo novo - o mesmo
  // problema que a conversa ja tinha resolvido, so que a tabela nunca ganhou
  // a mesma correcao.
  const char* curto_fonte = no.curto;
  char curto_cache[MESH_NOME_CURTO];
  if (!no.curto[0] && tab5_mesh_name_cache(no.num, curto_cache, sizeof(curto_cache))) {
    curto_fonte = curto_cache;
  }
  sanitizar_ascii(curto_fonte, curto_limpo, sizeof(curto_limpo));

  field(line, s_grade[C_NOME],  name_limpo);
  field(line, s_grade[C_CURTO], curto_limpo);

  // O id vem of proprio radio como "!a1b2c3d4". Quando ainda nao veio (o
  // node_info dele nao chegou), montamos a partir of numero - e a MESMA conta
  // que o Meshtastic faz, entao o valor confere.
  if (no.id[0]) { field(line, s_grade[C_ID], no.id); }
  else {
    snprintf(v, sizeof(v), "!%08lx", (unsigned long)no.num);
    field(line, s_grade[C_ID], v);
  }

  // Modelo conhecido pelo name; desconhecido pelo numero, que of para procurar.
  // Inventar one name para one valor que a tabela nao tem seria pior que o numero.
  const char* hw = mesh_hw_name(no.hw);
  if (hw) { field(line, s_grade[C_HW], hw); }
  else    { snprintf(v, sizeof(v), "modelo %u", (unsigned)no.hw);
            field(line, s_grade[C_HW], v); }

  // O PONTO DECIMAL TEM QUE CAIR SEMPRE NA MESMA COLUNA - foi essa a raiz of
  // "torto" observado.
  //
  // field() ja alinha a DIREITA corretamente (a string sempre termina rente a
  // borda right of field); o problema era o "%.1f" nao ter width fixa.
  // "-18.5" tem 5 caracteres, "9.0" tem 3 - as duas terminam no mesmo lugar
  // (alinhadas a right, of jeito certo), mas como comecam em colunas
  // DIFERENTES, o ponto decimal of uma line nao stays embaixo of ponto
  // decimal of line seguinte. Numa coluna of numeros, e o ponto decimal -
  // nao a borda - que o olho usa para saber que esta everything alinhado.
  //
  // "%+05.1f" fixa sinal, dois digitos inteiros (com zero a left when
  // faltar) e uma casa decimal - sempre 5 caracteres, sempre no mesmo formato:
  // "-18.5", "+09.0", "-05.5", "+00.0". O ponto decimal cai sempre in mesma
  // position, para qualquer leitura entre -99.9 e +99.9 (a faixa real of SNR
  // of Meshtastic nunca chega perto disso).
  snprintf(v, sizeof(v), "%+05.1f", (double)no.snr);
  field(line, s_grade[C_SNR], (no.snr <= -128.0f) ? "-" : v);

  // 101 e o code of Meshtastic para "in tomada", nao uma battery of 101%.
  // Mostrava "101%" antes. E 0 significa que a telemetria dele ainda nao veio.
  //
  // ZERO A ESQUERDA, pela mesma razao of SNR acima: "5%" e "100%" tem
  // larguras diferentes, e sem width fixa o digito das dezenas of uma line
  // nao cai embaixo of digito das dezenas of line seguinte. "%03u%%" fixa
  // 4 caracteres sempre - "005%", "050%", "100%" - e o '%' e os digitos ficam
  // in mesma coluna em toda line que tiver battery of verdade.
  if      (no.battery == 0)   { snprintf(v, sizeof(v), "-");  }
  else if (no.battery > 100)  { snprintf(v, sizeof(v), "AC"); }
  else                        { snprintf(v, sizeof(v), "%03u%%", (unsigned)no.battery); }
  field(line, s_grade[C_BAT], v);

  // "mqtt" no lugar of numero: no que chega pela internet nao tem salto of
  // radio nenhum, e mostrar "0" ali diria que ele e vizinho direto - o oposto
  // of verdade. E a informacao que decide se voce alcanca esse no com a
  // internet fora.
  //
  // "-" PARA DESCONHECIDO (nos apareciam sem name e sem saltos): one no so criado por achar_no() on ver one pacote
  // (sem NodeInfo ainda) tinha saltos=0 pelo memset, indistinguivel of one
  // vizinho direto of verdade. Agora o sentinela (MESH_SALTOS_DESCONHECIDO)
  // e explicito aqui, of jeito que o SNR (-128) ja era.
  if (no.via_mqtt) { snprintf(v, sizeof(v), "mqtt"); }
  else if (no.saltos == MESH_SALTOS_DESCONHECIDO) { snprintf(v, sizeof(v), "-"); }
  else             { snprintf(v, sizeof(v), "%u", (unsigned)no.saltos); }
  field(line, s_grade[C_SALTOS], v);
  // Em pe a line e mais curta: termina aqui, senao os espacos of resto
  // passariam of borda of screen.
  if ((int)size > line_ch()) { line[line_ch()] = 0; }
}

// O que ja esta escrito em cada line. Existe para NAO reescrever 32 lines of
// text por segundo: com o periodo of 1Hz e a idade correndo, quase toda
// pass mudava one punhado of lines e redesenhava todas. Em paisagem isso
// custa caro - o M5GFX transpoe as coordenadas e cada line of text vira
// escrita em coluna in memoria of panel.
EXT_RAM_BSS_ATTR static char s_nos_ant[MESH_MAX_NOS][NOS_LINHA_CH + 1];
EXT_RAM_BSS_ATTR static char s_nos_visto_ant[MESH_MAX_NOS][8];
EXT_RAM_BSS_ATTR static bool s_nos_fav_ant[MESH_MAX_NOS];   // a estrela que esta desenhada

// 44 e nao 32: com rolagem, height nao falta, e lines coladas cansavam o
// olho. O text (16 px) vai centrado e one fio escuro separa uma of outra.
static const int NOS_LINHA_A = 44;
static const int NOS_TEXTO_DY = (NOS_LINHA_A - 16) / 2;

static int nos_height(int, void*) { return NOS_LINHA_A; }

// Uma line of tabela. Guarda o que escreveu (s_nos_ant/s_nos_visto_ant): e a
// referencia com que desenhar_nos() decide, in pass seguinte, se a line
// changed. Font0 ja esta active (ver a excecao em desenhar_nos).
// ESTRELA of cinco pontas, em primitivas: none fonte of sistema tem o
// caractere, e desenhada ela exit igual em qualquer tamanho. Cheia = favorito;
// vazia (so o contorno) = o botao of marcar.
static void estrela(lgfx::LovyanGFX& d, int cx, int cy, int r, uint16_t color, bool cheia)
{
  float px[10], py[10];
  for (int k = 0; k < 10; ++k) {
    const float ang = (float)M_PI * (-0.5f + k * 0.2f);
    const float rr = (k & 1) ? r * 0.42f : (float)r;
    px[k] = cx + rr * cosf(ang);
    py[k] = cy + rr * sinf(ang);
  }
  for (int k = 0; k < 10; ++k) {
    const int j = (k + 1) % 10;
    if (cheia) { d.fillTriangle(cx, cy, (int)px[k], (int)py[k], (int)px[j], (int)py[j], color); }
    else       { d.drawLine((int)px[k], (int)py[k], (int)px[j], (int)py[j], color); }
  }
}

static void nos_draw(lgfx::LovyanGFX& d, int i, int x, int y, int, void*)
{
  mesh_no_t no;
  if (!tab5_mesh_no_rede(s_rede_vista, i, &no) || i >= MESH_MAX_NOS) { return; }

  // BRANCO = o nosso no. AMARELO = ouvido pelo ar (tem SNR): fixos no topo,
  // os que of para alcancar com a internet fora. Pela internet, o azul of
  // MQTT das conversas; sem SNR, cinza.
  const bool meu = (no.num == tab5_mesh_meu_numero());
  const uint16_t color = meu ? TFT_WHITE
                     : (no.fixado || tab5_mesh_no_pelo_ar(&no)) ? TFT_YELLOW
                     : no.via_mqtt ? TFT_SKYBLUE : TFT_DARKGREY;
  d.setTextSize(2);
  d.setTextDatum(top_left);
  montar_line(s_nos_ant[i], sizeof(s_nos_ant[i]), no);
  d.setTextColor(color, TFT_BLACK);
  d.drawString(s_nos_ant[i], x + NOS_MARGEM, y + NOS_TEXTO_DY);
  // Favorito: estrela in margem, antes of name (os 16 px que ja sobravam).
  if (no.favorito) { estrela(d, x + NOS_ESTRELA_X, y + NOS_LINHA_A / 2, 8, TFT_GOLD, true); }
  // Divisao fraca, of pouco contraste: separa sem disputar com o text.
  d.drawFastHLine(x + 6, y + NOS_LINHA_A - 1, visto_x() + 6 * NOS_CH - 6, d.color565(38, 42, 50));

  // Coluna separada porque tem color propria: a idade responde "of para falar
  // com ele AGORA?", que e outra pergunta of qualidade of sinal.
  char curto[8];
  idade_text(no.idade_s, curto, sizeof(curto));
  snprintf(s_nos_visto_ant[i], sizeof(s_nos_visto_ant[i]), "%-6.6s", curto);
  s_nos_fav_ant[i] = no.favorito;
  d.setTextColor(idade_color(no.idade_s), TFT_BLACK);
  d.drawString(s_nos_visto_ant[i], x + visto_x(), y + NOS_TEXTO_DY);
}

// Assinatura dos canais oferecidos: muda when one canal liga, desliga, troca
// of name ou entra/exit of coluna - e ai a faixa e repintada inteira.
// O estado of rede of internet, so pelo que muda o drawing (nao pelo contador of
// pacotes, que correria a cada segundo).
static int internet_fase(void)
{
  return !settings_mesh_public_network() ? 0 : mesh_mqtt_internet_conectado() ? 2 : 1;
}

static void canais_assinatura(char* out, size_t size)
{
  snprintf(out, size, "r%u%d|", (unsigned)s_rede_vista, internet_fase());
  const int n = tab5_mesh_num_canais();
  for (int i = 0; i < n; ++i) {
    uint8_t idx; char name[MESH_NOME_CONVERSA]; bool vis = false;
    if (!tab5_mesh_canal(i, &idx, name, sizeof(name), &vis)) { continue; }
    const size_t k = strlen(out);
    snprintf(out + k, size - k, "%u%c%s|", (unsigned)idx, vis ? '+' : '-', name);
  }
}

// Desenha a faixa dos canais e grava a geometria. Devolve a height usada.
//
// NA FONTE DA TABELA (Font0 em tamanho 2, 12 px por caractere), e nao in
// suave of tab5_botao: a faixa e a tabela sao a mesma pagina, e duas fontes
// coladas liam como duas telas. Cada botao leva o numero of canal ("ch0") -
// e o que o Meshtastic usa para dizer qual e o primario, e o que distingue
// dois canais of name parecido.
// Um botao of faixa, in fonte of tabela. Devolve a width.
static int chip(lgfx::LovyanGFX& d, int x, int y, const char* rot, uint16_t fundo, uint16_t frente)
{
  const int l = (int)strlen(rot) * NOS_CH + 28;
  d.fillRoundRect(x, y, l, CHIP_A, 10, fundo);
  d.setTextColor(frente, fundo);
  d.drawString(rot, x + 14, y + CHIP_A / 2);
  return l;
}

static int draw_canais(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  d.setFont(&fonts::Font0);
  d.setTextSize(2);
  const int end_x = a.x + a.width - 16;
  const int ini_x = x0 + NOS_MARGEM + 8 * NOS_CH;   // depois of rotulo "canais"
  int x = ini_x, y = a.y + 8;
  d.setTextDatum(middle_left);

  // REDE: NODE | INTERNET, e in vista INTERNET o liga/desliga e o estado dela.
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString("network", x0 + NOS_MARGEM, y + CHIP_A / 2);
  static const char* const ROT_REDE[2] = { "NODE", "INTERNET" };
  for (int r = 0; r < 2; ++r) {
    const bool sel = (s_rede_vista == r);
    const int l = chip(d, x, y, ROT_REDE[r], sel ? TFT_SKYBLUE : d.color565(50, 55, 65),
                       sel ? TFT_BLACK : TFT_LIGHTGREY);
    s_chip_rede[r] = chip_t{ x - a.x, y - a.y, l, CHIP_A, (uint8_t)r };
    x += l + 10;
  }
  s_chip_internet_liga = chip_t{ 0, 0, 0, 0, 0 };
  if (s_rede_vista == MESH_REDE_INTERNET) {
    x += 14;
    const bool lig = settings_mesh_public_network();
    const int l = chip(d, x, y, lig ? "TURN OFF" : "TURN ON", lig ? TFT_MAROON : TFT_DARKGREEN, TFT_WHITE);
    s_chip_internet_liga = chip_t{ x - a.x, y - a.y, l, CHIP_A, 0 };
    x += l + 14;
    const int fase = internet_fase();
    char est[40];
    sanitizar_ascii(fase == 0 ? "desligada" : fase == 1 ? "conectando..." : "conectada", est, sizeof(est));
    d.setTextColor(fase == 2 ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
    if (x + (int)strlen(est) * NOS_CH < end_x) { d.drawString(est, x, y + CHIP_A / 2); }
  }
  x = ini_x;
  y += CHIP_A + 8;
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString("canais", x0 + NOS_MARGEM, y + CHIP_A / 2);
  s_n_chips = 0;
  const int n = tab5_mesh_num_canais();
  for (int i = 0; i < n && s_n_chips < MESH_MAX_CANAIS + 1; ++i) {
    uint8_t idx; char name[MESH_NOME_CONVERSA]; bool vis = false;
    if (!tab5_mesh_canal(i, &idx, name, sizeof(name), &vis)) { continue; }
    if (mesh_canal_internet(idx) != (s_rede_vista == MESH_REDE_INTERNET)) { continue; }   // so os of rede vista
    char limpo[MESH_NOME_CONVERSA];
    sanitizar_ascii(name, limpo, sizeof(limpo));   // Font0: so ASCII (ver sanitizar_ascii)
    char rot[MESH_NOME_CONVERSA + 12];
    if (mesh_canal_internet(idx)) { snprintf(rot, sizeof(rot), "%s%s", vis ? "" : "+ ", limpo); }
    else { snprintf(rot, sizeof(rot), "%sch%u %s", vis ? "" : "+ ", (unsigned)idx, limpo); }
    const int l = (int)strlen(rot) * NOS_CH + 28;
    if (x + l > end_x && x > ini_x) { x = ini_x; y += CHIP_A + 8; }
    const uint16_t fundo = vis ? TFT_NAVY : d.color565(50, 55, 65);
    d.fillRoundRect(x, y, l, CHIP_A, 10, fundo);
    d.setTextColor(vis ? TFT_WHITE : TFT_LIGHTGREY, fundo);
    d.drawString(rot, x + 14, y + CHIP_A / 2);
    s_chips[s_n_chips++] = chip_t{ x - a.x, y - a.y, l, CHIP_A, idx };
    x += l + 10;
  }
  d.setTextDatum(top_left);
  return (y - a.y) + CHIP_A + 8;
}

static bool no_por_num(uint32_t num, mesh_no_t* out);
static bool no_internet(uint32_t num) { mesh_no_t no; return no_por_num(num, &no) && no.rede == MESH_REDE_INTERNET; }

static bool no_por_num(uint32_t num, mesh_no_t* out)
{
  const int n = tab5_mesh_num_nos();
  for (int i = 0; i < n; ++i) {
    if (tab5_mesh_no(i, out) && out->num == num) { return true; }
  }
  return false;
}

static void distancia_direcao(float lat1, float lon1, float lat2, float lon2,
                              float* dist_km, const char** direcao);

// CARTAO DO NO - everything o que se sabe dele, como o cliente web mostra. Cabe o
// que a tabela nao tem width para mostrar (MAC, tensao, position) sem
// apertar mais uma coluna nela.
static const int DET_LIN   = 50;
static const int DET_BTN_A = 64;
static const int DET_BTN_L = 300;
static const int DET_BTN_Y_DA_BASE = 84;    // of bottom para top, ver ao_tocar
// Botao of ESTRELA, no canto of top a right of cartao (coordenadas a partir
// of right of area, para valer em qualquer width).
static const int DET_FAV_L = 260, DET_FAV_A = 56, DET_FAV_Y = 14, DET_FAV_DIR = 24;
// EM PE as medidas fixas passam of borda: dois botoes of 300 lado a lado nao
// cabem nos ~630 px que sobram of trilho. Desenho e touch passam pelas mesmas
// funcoes, entao concordam nas duas orientacoes. 'larg' = of coluna x0 ate o
// end of area.
static int det_btn_l(const area_t& a)
{
  const int larg = a.width - TRILHO_L;
  const int cabe = (larg - 3 * 24) / 2;
  return cabe < DET_BTN_L ? cabe : DET_BTN_L;
}
static int det_fav_l(const area_t& a) { return em_pe(a) ? 210 : DET_FAV_L; }
static int det_val_dx(const area_t& a) { return em_pe(a) ? 170 : 220; }   // coluna dos valores
static char s_det_assin[512];

static void draw_no_detalhe(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  mesh_no_t no;
  const bool achou = no_por_num(s_no_sel, &no);
  const bool meu = (s_no_sel == tab5_mesh_meu_numero());

  enum { R_ID = 0, R_MAC, R_HW, R_SINAL, R_SALTOS, R_BAT, R_VISTO, R_POS, R_TOTAL };
  static const char* const ROT[R_TOTAL] = { "id", "MAC", "model", "signal", "hops", "battery", "seen", "position" };
  char val[R_TOTAL][64];
  char titulo[MESH_NOME_LONGO + 16], curto[MESH_NOME_CURTO * 2] = "";
  for (int k = 0; k < R_TOTAL; ++k) { snprintf(val[k], sizeof(val[k]), "-"); }

  if (!achou) {
    tab5_mesh_name_exibicao(s_no_sel, titulo, sizeof(titulo));
    snprintf(val[R_ID], sizeof(val[R_ID]), "!%08lx  (saiu da tabela ao vivo)", (unsigned long)s_no_sel);
  } else {
    // Titulo: o longo; sem ele, a regra of exibicao (curto, ou o id).
    char longo[MESH_NOME_LONGO] = "";
    if (no.longo[0]) { snprintf(longo, sizeof(longo), "%s", no.longo); }
    else             { tab5_mesh_name_longo_cache(no.num, longo, sizeof(longo)); }
    if (longo[0]) { snprintf(titulo, sizeof(titulo), "%s", longo); }
    else          { tab5_mesh_name_exibicao(no.num, titulo, sizeof(titulo)); }
    if (no.curto[0]) { snprintf(curto, sizeof(curto), "%s", no.curto); }
    else             { tab5_mesh_name_cache(no.num, curto, sizeof(curto)); }

    snprintf(val[R_ID], sizeof(val[R_ID]), "%s   nº %lu",
             no.id[0] ? no.id : "", (unsigned long)no.num);
    if (!no.id[0]) { snprintf(val[R_ID], sizeof(val[R_ID]), "!%08lx   nº %lu", (unsigned long)no.num, (unsigned long)no.num); }

    // O MAC so when o no o mandou (User.macaddr). O id e derivado dos 4
    // ultimos bytes dele, mas completar os 2 primeiros seria inventar.
    if (no.tem_mac) {
      snprintf(val[R_MAC], sizeof(val[R_MAC]), "%02X:%02X:%02X:%02X:%02X:%02X",
               no.mac[0], no.mac[1], no.mac[2], no.mac[3], no.mac[4], no.mac[5]);
    } else {
      snprintf(val[R_MAC], sizeof(val[R_MAC]), "not sent by the node");
    }

    const char* hw = mesh_hw_name(no.hw);
    if (no.hw == 0)  { snprintf(val[R_HW], sizeof(val[R_HW]), "not reported"); }
    else if (hw)     { snprintf(val[R_HW], sizeof(val[R_HW]), "%s", hw); }
    else             { snprintf(val[R_HW], sizeof(val[R_HW]), "modelo %u", (unsigned)no.hw); }

    if (no.snr > -128.0f) { snprintf(val[R_SINAL], sizeof(val[R_SINAL]), "SNR %+.1f dB%s", (double)no.snr, no.via_mqtt ? "  (do gateway)" : ""); }

    if (no.via_mqtt)                                   { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), "over the internet (MQTT)"); }
    else if (no.saltos == 0)                           { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), "0 - vizinho direto"); }
    else if (no.saltos != MESH_SALTOS_DESCONHECIDO)    { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), "%u", (unsigned)no.saltos); }

    char bat[32] = "";
    if (no.battery > 100)    { snprintf(bat, sizeof(bat), "na tomada"); }
    else if (no.battery > 0) { snprintf(bat, sizeof(bat), "%u %%", (unsigned)no.battery); }
    if (no.tensao > 0.1f) {
      const size_t k = strlen(bat);
      snprintf(bat + k, sizeof(bat) - k, "%s%.2f V", k ? "   " : "", (double)no.tensao);
    }
    if (bat[0]) { snprintf(val[R_BAT], sizeof(val[R_BAT]), "%s", bat); }

    char idade[12];
    idade_text(no.idade_s, idade, sizeof(idade));
    if (no.idade_s < 0)       { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), "not heard this session"); }
    else if (no.idade_s < 90) { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), "now"); }
    else                      { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), "%s ago", idade); }

    if (no.tem_pos) {
      const float lat = no.lat_i / 1e7f, lon = no.lon_i / 1e7f;
      float rlat, rlon; bool ref = false;
      const nmea_position_t fix = tab5_gps_position();
      if (fix.valid && fix.qualidade > 0) { rlat = (float)fix.lat; rlon = (float)fix.lon; ref = true; }
      else { ref = settings_home(&rlat, &rlon); }
      if (ref && !meu) {
        float km; const char* dir;
        distancia_direcao(rlat, rlon, lat, lon, &km, &dir);
        snprintf(val[R_POS], sizeof(val[R_POS]), "%.5f, %.5f   %.1f km %s", (double)lat, (double)lon, (double)km, dir);
      } else {
        snprintf(val[R_POS], sizeof(val[R_POS]), "%.5f, %.5f", (double)lat, (double)lon);
      }
    }
  }

  const bool fav = achou ? no.favorito : tab5_mesh_favorito(s_no_sel);

  // So repinta when algum text changed: a pagina roda a 1 Hz.
  char assin[sizeof(s_det_assin)];
  snprintf(assin, sizeof(assin), "%s|%s|%d|", titulo, curto, (int)fav);
  for (int k = 0; k < R_TOTAL; ++k) {
    const size_t n = strlen(assin);
    snprintf(assin + n, sizeof(assin) - n, "%s|", val[k]);
  }
  if (!paint_changed_s(&s_p, s_det_assin, sizeof(s_det_assin), assin)) { return; }

  tab5_screen_clear(x0, a.y, a.x + a.width - x0, a.height, TFT_BLACK);
  d.setTextDatum(top_left);
  tab5_text(d, 4.2f);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  // O titulo para antes of estrela (e of chip of curto).
  const int fav_l = det_fav_l(a);
  const int fav_x = a.x + a.width - DET_FAV_DIR - fav_l;
  const int tit_max = fav_x - (x0 + 24) - 160;
  tab5_cortar_on_caber(d, titulo, tit_max);
  d.drawString(titulo, x0 + 24, a.y + 18);
  if (!meu) {
    const uint16_t fundo = fav ? d.color565(70, 60, 10) : d.color565(50, 55, 65);
    d.fillRoundRect(fav_x, a.y + DET_FAV_Y, fav_l, DET_FAV_A, 12, fundo);
    estrela(d, fav_x + 34, a.y + DET_FAV_Y + DET_FAV_A / 2, 16, fav ? TFT_GOLD : TFT_LIGHTGREY, fav);
    d.setFont(tab5_fonte_suave(true, 26));
    d.setTextDatum(middle_left);
    d.setTextColor(fav ? TFT_GOLD : TFT_LIGHTGREY, fundo);
    d.drawString(fav ? "FAVORITO" : "FAVORITAR", fav_x + 62, a.y + DET_FAV_Y + DET_FAV_A / 2);
    d.setTextDatum(top_left);
    tab5_text(d, 4.2f);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  if (curto[0]) {
    const int tx = x0 + 24 + (int)d.textWidth(titulo) + 20;
    tab5_botao(d, tx, a.y + 16, 120, 44, curto, TFT_NAVY, TFT_WHITE);
  }
  if (meu) {
    tab5_text(d, 2.4f);
    d.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    d.drawString("this is your node", x0 + 24, a.y + 62);
  }

  for (int k = 0; k < R_TOTAL; ++k) {
    const int y = a.y + 96 + k * DET_LIN;
    tab5_text(d, 2.8f);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString(ROT[k], x0 + 24, y + 4);
    // Valor longo (a position com distancia, sobretudo em pe): encolhe ate
    // caber e, no limit, corta - nunca passa of borda.
    const int vx = x0 + det_val_dx(a);
    text_que_cabe(d, val[k], a.x + a.width - 16 - vx, 3.4f, 2.6f);
    d.setTextColor(k == R_MAC && !(achou && no.tem_mac) ? TFT_DARKGREY : TFT_WHITE, TFT_BLACK);
    d.drawString(val[k], vx, y);
  }

  const int by = a.y + a.height - DET_BTN_Y_DA_BASE;
  const int bl = det_btn_l(a);
  tab5_botao(d, x0 + 24, by, bl, DET_BTN_A, "BACK", d.color565(50, 55, 65), TFT_WHITE);
  // No of INTERNET nao tem CONVERSAR: a direta sairia pelo nosso radio (ou pela
  // internet, que nao leva direta) e nunca chegaria la. Fala-se com ele no
  // LongFast Internet.
  if (!meu && !(achou && no.rede == MESH_REDE_INTERNET)) {
    tab5_botao(d, x0 + 24 + bl + 24, by, bl, DET_BTN_A, "CONVERSAR", TFT_GREENYELLOW, TFT_BLACK);
  }
}

static void draw_nos(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  char line[NOS_LINHA_CH + 1];
  char visto[8];
  // A grade pela orientation of now. Trocar of grade so acontece num giro,
  // e o giro ja vem com pass complete: everything e remontado abaixo.
  s_grade = em_pe(a) ? COLUNA_PE : COLUNA;

  if (s_no_sel) {
    list_hide(&s_l_nos);   // a tabela nao esta in screen: nao rola nem responde
    draw_no_detalhe(d, a, x0);
    tab5_fonte_body(d);
    return;
  }

  // Os canais changed (config nova, conversa apagada/reaberta): a faixa muda
  // of height e a tabela muda of lugar - pass complete.
  char assin[sizeof(s_chips_assin)];
  canais_assinatura(assin, sizeof(assin));
  if (strcmp(assin, s_chips_assin) != 0) {
    snprintf(s_chips_assin, sizeof(s_chips_assin), "%s", assin);
    if (!TUDO) { paint_request(&s_p); system_request_redraw_complete(); return; }
  }

  if (TUDO) {
    tab5_screen_clear(x0, a.y, a.x + a.width - x0, a.height, TFT_BLACK);
    s_chips_height = draw_canais(d, a, x0);
  }

  // EXCECAO A FONT4, DE PROPOSITO: esta tabela monta UMA STRING SO por line
  // (field() escreve cada valor num offset of CARACTERE fixo dentro of
  // 'line', ver COLUNA[].col acima) e desenha a line inteira of uma vez. Isso
  // SO alinha em colunas se cada caractere tiver a MESMA width em pixels -
  // exatamente o que a Font0 garante (6px, sempre) e a Font4 NAO (e
  // proporcional). Por isso ela stays in Font0 e so ela neste app.
  d.setFont(&fonts::Font0);

  if (TUDO) {
    // Cabecalho pela MESMA tabela das lines - e o que garante que ele nunca
    // mais saia of lugar sozinho.
    espacos(line, sizeof(line));
    for (int i = 0; i < C_VISTO; ++i) { field(line, s_grade[i], s_grade[i].titulo); }
    line[line_ch()] = 0;
    d.setTextSize(2);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(line, x0 + NOS_MARGEM, a.y + s_chips_height + 8);
    d.drawString(s_grade[C_VISTO].titulo, x0 + visto_x(), a.y + s_chips_height + 8);
  }
  d.setTextSize(2);
  d.setTextDatum(top_left);

  // CABECALHO FIXO ("sticky"): ele nao rola, so as lines.
  const int cabecalho_a = s_chips_height + 36;
  const area_t window_nos = { x0, a.y + cabecalho_a, a.x + a.width - x0, a.height - cabecalho_a };

  // O QUE MUDOU DE VALOR (a idade corre a cada segundo, o SNR muda com o
  // next pacote, a order muda when alguem fala): compara com o que esta
  // escrito e marca SO essas lines. Mudou a CONTAGEM: a lista inteira.
  const int n = tab5_mesh_num_nos_rede(s_rede_vista);
  if (TUDO || n != s_l_nos.n) {
    list_items_changed(&s_l_nos, n);
  } else {
    for (int i = 0; i < n; ++i) {
      // Linha nunca desenhada (fora of window): a rolagem a desenha when
      // ela aparecer. Marca-la por ter o text vazio arrastaria as visiveis
      // junto (a marca e one intervalo) - ver s_cfg_pintada in pagina "No".
      if (i < MESH_MAX_NOS && !s_nos_ant[i][0]) { continue; }
      mesh_no_t no;
      if (!tab5_mesh_no_rede(s_rede_vista, i, &no)) { continue; }
      montar_line(line, sizeof(line), no);
      char curto[8];
      idade_text(no.idade_s, curto, sizeof(curto));
      snprintf(visto, sizeof(visto), "%-6.6s", curto);
      if (strcmp(line, s_nos_ant[i]) != 0 || strcmp(visto, s_nos_visto_ant[i]) != 0
       || no.favorito != s_nos_fav_ant[i]) {
        list_item_changed(&s_l_nos, i);
      }
    }
  }
  list_draw(&s_l_nos, d, window_nos, TFT_BLACK, TUDO);

  tab5_fonte_body(d);   // devolve a Font4 - ver a excecao acima
}

static void draw_malha(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  if (TUDO) { tab5_screen_clear(x0, a.y, a.x + a.width - x0, a.height, TFT_BLACK); }
  mesh_saude_t s; tab5_mesh_saude(&s);
  char buf[160];

  // "-" WHEN IT DIDN'T COME IN. The health numbers only exist with OUR
  // node's LocalStats / DeviceMetrics; in internet mode (no node) or before
  // the first telemetry they're zero - and "0.0 %" channel usage is a
  // plausible value that used to lie "empty mesh" when the truth was
  // "don't know".
  //
  // ONLINE NODES without LocalStats: counted here, with the same rule as
  // Meshtastic (heard in the last 2 h) - marked as such.
  struct { const char* rot; char val[48]; uint16_t color; } campos[10];
  if (s.tem_local) {
    snprintf(campos[0].val, sizeof(campos[0].val), "%u / %u", (unsigned)s.nos_online, (unsigned)s.nos_total);
    campos[0].rot = "online nodes (from the radio)";
  } else {
    int on = 0;
    const int n = tab5_mesh_num_nos_rede(MESH_REDE_NO);   // a saude e of malha daqui
    for (int i = 0; i < n; ++i) {
      mesh_no_t no;
      if (tab5_mesh_no_rede(MESH_REDE_NO, i, &no) && no.idade_s >= 0 && no.idade_s <= 7200) { ++on; }
    }
    snprintf(campos[0].val, sizeof(campos[0].val), "%d / %d", on, n);
    campos[0].rot = "online nodes (counted here, 2 h)";
  }
  campos[0].color = TFT_GREENYELLOW;
  const bool tem_uso = s.tem_local || s.tem_metricas;
  if (tem_uso) { snprintf(campos[1].val, sizeof(campos[1].val), "%.1f %%", (double)s.uso_canal); }
  else         { snprintf(campos[1].val, sizeof(campos[1].val), "-"); }
  campos[1].rot = "channel usage"; campos[1].color = TFT_SKYBLUE;
  if (tem_uso) { snprintf(campos[2].val, sizeof(campos[2].val), "%.2f %%", (double)s.airtime_tx); }
  else         { snprintf(campos[2].val, sizeof(campos[2].val), "-"); }
  campos[2].rot = "airtime tx"; campos[2].color = TFT_SKYBLUE;
  if (tem_uso) {
    snprintf(campos[3].val, sizeof(campos[3].val), "%lu h %lu min",
             (unsigned long)(s.uptime_s / 3600), (unsigned long)((s.uptime_s % 3600) / 60));
  } else {
    snprintf(campos[3].val, sizeof(campos[3].val), "-");
  }
  campos[3].rot = "node uptime"; campos[3].color = TFT_WHITE;
  // LINK WITH TRANSPORT: "on air" alone didn't say over what.
  const uint8_t via = tab5_mesh_via();
  const char* name_via = !tab5_mesh_servico_active() ? "stopped"
                       : via == SETTINGS_MESH_TRANSPORT_BLUETOOTH ? "Bluetooth"
                       : via == SETTINGS_MESH_TRANSPORT_INTERNET  ? "Internet" : "Grove";
  snprintf(campos[4].val, sizeof(campos[4].val), "%s · %s", name_via, tab5_mesh_no_ar() ? "on air" : "no frame");
  campos[4].rot = "link"; campos[4].color = tab5_mesh_no_ar() ? TFT_GREENYELLOW : TFT_RED;

  // SECOND COLUMN: what the radio heard and its immediate surroundings. The
  // packet counters only exist with LocalStats ("-" without it, as above).
  if (s.tem_local) {
    snprintf(campos[5].val, sizeof(campos[5].val), "%lu / %lu", (unsigned long)s.pkt_rx, (unsigned long)s.pkt_tx);
    snprintf(campos[6].val, sizeof(campos[6].val), "%lu · %lu", (unsigned long)s.pkt_ruins, (unsigned long)s.pkt_repetidos);
  } else {
    snprintf(campos[5].val, sizeof(campos[5].val), "-");
    snprintf(campos[6].val, sizeof(campos[6].val), "-");
  }
  campos[5].rot = "pacotes recebidos / enviados"; campos[5].color = TFT_SKYBLUE;
  campos[6].rot = "pacotes ruins · repetidos";    campos[6].color = TFT_SKYBLUE;
  {
    const uint32_t meu_no = tab5_mesh_meu_numero();
    int vizinhos = 0, mais_recente = -1;
    int bat = -1;
    const int nn = tab5_mesh_num_nos_rede(MESH_REDE_NO);
    for (int i = 0; i < nn; ++i) {
      mesh_no_t no;
      if (!tab5_mesh_no_rede(MESH_REDE_NO, i, &no)) { continue; }
      if (meu_no && no.num == meu_no) { bat = no.battery; continue; }
      if (no.idade_s < 0) { continue; }
      if (mais_recente < 0 || no.idade_s < mais_recente) { mais_recente = no.idade_s; }
      if (no.saltos == 0 && !no.via_mqtt && no.idade_s <= 7200) { ++vizinhos; }
    }
    snprintf(campos[7].val, sizeof(campos[7].val), "%d", vizinhos);
    campos[7].rot = "vizinhos diretos (2 h)"; campos[7].color = TFT_GREENYELLOW;
    if (mais_recente >= 0) {
      char t[8]; idade_text(mais_recente, t, sizeof(t));
      snprintf(campos[8].val, sizeof(campos[8].val), "%s ago", t);
    } else {
      snprintf(campos[8].val, sizeof(campos[8].val), "-");
    }
    campos[8].rot = "last node heard"; campos[8].color = TFT_WHITE;
    if (bat < 0)        { snprintf(campos[9].val, sizeof(campos[9].val), "-"); }
    else if (bat > 100) { snprintf(campos[9].val, sizeof(campos[9].val), "na tomada"); }
    else                { snprintf(campos[9].val, sizeof(campos[9].val), "%d %%", bat); }
    campos[9].rot = "my node's battery"; campos[9].color = TFT_WHITE;
  }

  // DUAS COLUNAS, passo vertical repartido pela height que sobra (nunca menor
  // que antes). A width of cada valor e measurement: o que nao couber in coluna
  // cai para a fonte menor em vez of invadir a vizinha.
  //
  // EM PE, UMA COLUNA SO: a metade of ~630 px nao cabe nem o rotulo mais
  // longo, e a height que sobra of para as dez lines uma embaixo of outra.
  const int colunas = em_pe(a) ? 1 : 2;
  const int por_col = 10 / colunas;
  const int col_l = (a.x + a.width - x0 - 48) / colunas;
  int passo = (a.height - 24 - 40) / por_col;
  if (passo < 88) { passo = 88; }
  if (passo > 120) { passo = 120; }

  // Repinta so o valor que changed: a pagina roda a 1 Hz.
  static char s_val_ant[10][48];
  for (int i = 0; i < 10; ++i) {
    const int x = x0 + 24 + (i / por_col) * col_l;
    const int y = a.y + 24 + (i % por_col) * passo;
    if (!paint_changed_s(&s_p, s_val_ant[i], sizeof(s_val_ant[i]), campos[i].val)) { continue; }
    if (!TUDO) { tab5_screen_clear(x, y, col_l - 8, passo - 2, TFT_BLACK); }
    tab5_text(d, 3.12f);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(campos[i].rot, x, y);
    tab5_text(d, 5.85f);
    if (d.textWidth(campos[i].val) > col_l - 16) { tab5_text(d, 4.2f); }
    d.setTextColor(campos[i].color, TFT_BLACK);
    d.drawString(campos[i].val, x, y + 26);
  }

  // O NOSSO NO PELO NOME, com o id on lado - era so o numero em hexa.
  const uint32_t meu = tab5_mesh_meu_numero();
  char curto[MESH_NOME_CURTO * 2] = "", longo[MESH_NOME_LONGO] = "";
  tab5_mesh_name_cache(meu, curto, sizeof(curto));
  tab5_mesh_name_longo_cache(meu, longo, sizeof(longo));
  if (!meu) { snprintf(buf, sizeof(buf), "my node: not announced yet"); }
  else {
    snprintf(buf, sizeof(buf), "my node: %s%s%s%s  !%08lx   firmware %s",
             longo, (longo[0] && curto[0]) ? " (" : "", curto, (longo[0] && curto[0]) ? ")" : "",
             (unsigned long)meu, tab5_mesh_firmware()[0] ? tab5_mesh_firmware() : "-");
  }
  static char s_rodape_ant[sizeof(buf)];
  if (paint_changed_s(&s_p, s_rodape_ant, sizeof(s_rodape_ant), buf)) {
    if (!TUDO) { tab5_screen_clear(x0, a.y + a.height - 34, a.x + a.width - x0, 34, TFT_BLACK); }
    text_que_cabe(d, buf, a.x + a.width - 16 - (x0 + 24), 3.12f, 2.4f);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString(buf, x0 + 24, a.y + a.height - 30);
  }
}


// ROTULO DE REMETENTE, DENTRO of bubble (era ACIMA, fora dele
// changed) - so em conversa of CANAL, onde varias pessoas diferentes
// escrevem no mesmo lugar. Numa conversa direta o lado e a color of bubble ja
// dizem quem mandou; one rotulo ali so repetiria informacao. A ALTURA
// reservada para ele (NOME_A) e measurement of verdade em desenhar_msgs() - ver o
// aviso la - nao mais one numero fixo aqui.

// Nome of remetente para o rotulo - ver tab5_mesh_nome_exibicao() no servico:
// shortname first, ID em hexa se o shortname nao prestar (vazio, "?" ou
// emoji) ou o no ainda nao se apresentou. So one wrapper fino aqui; a regra em
// si mora no servico porque a conversa direta (tab5_mesh.cpp) precisa of
// MESMA regra, e duas copias dela e exatamente o tipo of duplicacao que
// tab5_ui.hpp existe para evitar (ver o aviso no topo daquele arquivo).
static void name_of_no(uint32_t num, char* out, size_t out_size)
{
  tab5_mesh_name_exibicao(num, out, out_size);
}

// COR POR REMETENTE, so para o rotulo of name em conversa of CANAL - request
// depois of ver o app oficial of Meshtastic no celular, que
// pinta cada participante numa color propria em vez of cinza uniforme igual
// para todo mundo. Hash simples (o proprio numero of no, modulo o tamanho of
// paleta): determinismo e everything que se precisa aqui - a mesma pessoa sempre
// cai in mesma color NESTA sessao, e isso basta para o olho aprender a
// distinguir quem escreveu o que numa window cheia of gente. Nao precisa
// sobreviver a reinicio, e so identificacao visual, nao dado gravado.
static uint16_t color_of_no(uint32_t num)
{
  static const uint16_t PALETA[] = {
    TFT_SKYBLUE, TFT_ORANGE, TFT_PINK, TFT_YELLOW,
    TFT_CYAN, TFT_MAGENTA, TFT_GREENYELLOW, TFT_PURPLE,
  };
  return PALETA[num % (sizeof(PALETA) / sizeof(PALETA[0]))];
}

// -----------------------------------------------------------------------
// LOCALIZACAO DENTRO DE UMA MENSAGEM
// -----------------------------------------------------------------------
// Quem compartilha localizacao pelo app of Meshtastic no celular quase
// sempre manda uma message of TEXTO normal com as coordenadas embutidas
// (one link tipo "https://maps.google.com/?q=-12.97,-38.50", ou so o par
// cru) - nao existe one tipo of pacote separado que a conversa precise
// entender. Entao a deteccao e sobre o TEXTO, nao sobre o protocolo.
//
// achar_coord() procura, em QUALQUER lugar of string, one par "numero,
// numero" (com o first cabendo em latitude e o segundo em longitude) -
// serve tanto para o link quanto para o par cru, sem se importar com o
// resto of text on redor (URL, legenda, o que vier).
static bool achar_coord(const char* txt, float* lat, float* lon)
{
  for (const char* p = txt; *p; ++p) {
    if (*p != '-' && (*p < '0' || *p > '9')) { continue; }

    char* fim1 = nullptr;
    const double a = strtod(p, &fim1);
    if (fim1 == p) { continue; }

    const char* q = fim1;
    while (*q == ' ') { ++q; }
    if (*q != ',') { continue; }
    ++q;
    while (*q == ' ') { ++q; }

    char* fim2 = nullptr;
    const double b = strtod(q, &fim2);
    if (fim2 == q) { continue; }

    // O PRIMEIRO numero tem que caber em latitude e o SEGUNDO em longitude -
    // e o que distingue one par of coordenadas of dois numeros quaisquer
    // separados por virgula que por acaso apareçam num text.
    if (a < -90.0 || a > 90.0)   { continue; }
    if (b < -180.0 || b > 180.0) { continue; }

    *lat = (float)a;
    *lon = (float)b;
    return true;
  }
  return false;
}

// Distancia (formula of haversine, correta em qualquer escala) e direcao
// (rumo inicial, arredondado para os 8 pontos cardeais) of position of
// referencia ("casa", settings.h) ate a localizacao recebida.
static void distancia_direcao(float lat1, float lon1, float lat2, float lon2,
                              float* dist_km, const char** direcao)
{
  const double GRAU = M_PI / 180.0;
  const double R_TERRA = 6371.0;   // km

  const double dlat = (lat2 - lat1) * GRAU;
  const double dlon = (lon2 - lon1) * GRAU;
  const double s = sin(dlat / 2) * sin(dlat / 2)
                 + cos(lat1 * GRAU) * cos(lat2 * GRAU) * sin(dlon / 2) * sin(dlon / 2);
  *dist_km = (float)(2.0 * R_TERRA * atan2(sqrt(s), sqrt(1.0 - s)));

  const double y = sin(dlon) * cos(lat2 * GRAU);
  const double x = cos(lat1 * GRAU) * sin(lat2 * GRAU)
                  - sin(lat1 * GRAU) * cos(lat2 * GRAU) * cos(dlon);
  double rumo = atan2(y, x) * 180.0 / M_PI;
  if (rumo < 0.0) { rumo += 360.0; }

  static const char* NOMES[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  *direcao = NOMES[((int)((rumo + 22.5) / 45.0)) % 8];
}

// ---------------------------------------------------------------------------
// A CONVERSA - o componente of sistema (kernel/conversation),
// ---------------------------------------------------------------------------
// Baloes, barra of escrita, keyboard virtual e rolagem eram uma copia inteira
// aqui (~700 lines). O componente nasceu DELES, para o ESP-NOW; now o Mesh
// iteration a usar o original, e uma correcao no keyboard ou no bubble vale para os
// dois. O que e so of Mesh mora nas tres respostas abaixo: rotulo of remetente
// no canal, "N saltos"/MQTT, coordenada virando distancia e rumo, e as quatro
// marcas of send_state.
// Fundo of area of mensagens (o mesmo of componente - trilho e colunas usam).
static uint16_t background_msg(lgfx::LovyanGFX& d) { return d.color565(13, 17, 23); }

static float s_ref_lat = 0, s_ref_lon = 0;   // referencia of distancia (GPS ou casa)
static bool  s_tem_ref = false;

static void atualizar_eh_canal(void)
{
  s_eh_canal = s_ab.ok && s_ab.channel;
}

static int conv_count(void*) { return (s_hist && s_ab.ok && chave_igual(s_hist_chave, s_ab)) ? s_n_hist : 0; }

static bool conv_read(int i, conversation_msg_t* o, void*)
{
  if (!s_hist || i < 0 || i >= s_n_hist) { return false; }
  const mesh_msg_t* m = &s_hist[i];

  // O componente le em order, of 0 em diante: a referencia of distancia vale
  // para a pass inteira (uma leitura of GPS, nao duzentas).
  if (i == 0) {
    const nmea_position_t fix = tab5_gps_position();
    s_tem_ref = false;
    if (fix.valid && fix.qualidade > 0) { s_ref_lat = (float)fix.lat; s_ref_lon = (float)fix.lon; s_tem_ref = true; }
    else if (settings_home(&s_ref_lat, &s_ref_lon)) { s_tem_ref = true; }
  }

  o->minha  = (m->of == tab5_mesh_meu_numero());
  o->when = m->when;

  // Coordenada no text vira "alfinete, lat, lon, distancia e rumo".
  float lat, lon;
  if (achar_coord(m->text, &lat, &lon)) {
    if (s_tem_ref) {
      float km; const char* dir;
      distancia_direcao(s_ref_lat, s_ref_lon, lat, lon, &km, &dir);
      snprintf(o->text, sizeof(o->text), "\xF0\x9F\x93\x8D %.4f, %.4f  %.1fkm %s",
               (double)lat, (double)lon, (double)km, dir);
    } else {
      snprintf(o->text, sizeof(o->text), "\xF0\x9F\x93\x8D %.4f, %.4f", (double)lat, (double)lon);
    }
  } else {
    snprintf(o->text, sizeof(o->text), "%s", m->text);
  }

  // Rotulo so no CANAL (varias pessoas no mesmo lugar), e so in primeira of
  // uma sequencia of mesmo remetente.
  if (s_eh_canal && !o->minha && (i == 0 || s_hist[i - 1].of != m->of)) {
    name_of_no(m->of, o->name, sizeof(o->name));
    o->color_name = color_of_no(m->of);
  }

  if (!o->minha) {
    // Por onde chegou: MQTT (internet) ou quantos saltos of radio. Sem o dado
    // no pacote, o que a tabela of nos sabe of remetente.
    bool via_mqtt = m->via_mqtt;
    uint8_t saltos = m->saltos;
    if (!via_mqtt && saltos == MESH_SALTOS_DESCONHECIDO) {
      bool nm = false; uint8_t ns = MESH_SALTOS_DESCONHECIDO;
      if (tab5_mesh_no_enlace(m->of, &nm, &ns)) { via_mqtt = nm; saltos = ns; }
    }
    if (via_mqtt)                                { snprintf(o->meta, sizeof(o->meta), "MQTT"); }
    else if (saltos != MESH_SALTOS_DESCONHECIDO) { snprintf(o->meta, sizeof(o->meta), "%u salto%s", (unsigned)saltos, saltos == 1 ? "" : "s"); }
    else                                         { snprintf(o->meta, sizeof(o->meta), "--"); }
    o->color_meta = via_mqtt ? (uint16_t)TFT_SKYBLUE
                : (saltos == MESH_SALTOS_DESCONHECIDO) ? (uint16_t)TFT_DARKGREY
                : (saltos <= 1) ? (uint16_t)TFT_GREENYELLOW
                : (saltos <= 3) ? lgfx::color565(140, 235, 140) : (uint16_t)TFT_ORANGE;
  } else if (m->id != 0) {
    // O estado vivo of send_state (ACK chega depois), ou o que became gravado.
    uint8_t erro = 0;
    uint8_t est = tab5_mesh_estado_send_state(m->id, &erro);
    if (est == MESH_ENVIO_NA) { est = m->send_state; }
    o->send_state = (est == MESH_ENVIANDO)     ? CONVERSA_ENVIO_PENDENTE
             : (est == MESH_ENTREGUE)     ? CONVERSA_ENVIO_OK
             : (est == MESH_FALHOU)       ? CONVERSA_ENVIO_FALHOU
             : (est == MESH_SEM_RESPOSTA) ? CONVERSA_ENVIO_SEM_RESPOSTA
             : (est == MESH_OUVIDO)       ? CONVERSA_ENVIO_OUVIDO
                                          : CONVERSA_ENVIO_NADA;
  }
  return true;
}

static bool conv_send(const char* text, void*)
{
  // PELA CHAVE: e para quem esta in screen, mesmo que a coluna tenha se
  // reordenado entre o drawing e o touch em ENVIAR.
  if (s_conv < 0 || !s_ab.ok || !tab5_mesh_servico_active()) { return false; }
  const bool ok = tab5_mesh_send(s_ab.channel ? 0xFFFFFFFFu : s_ab.no, s_ab.channel ? s_ab.idx : 0, text);
  s_hist_pronto = false;          // rele incluindo a que acabou of exit
  return ok;
}

// A FAIXA DE CIMA DA CONVERSA EM PE: VOLTAR, o tipo (canal ou direta) e o
// name. Deitado ela nao existe - a coluna on lado ja diz qual conversa e.
// Repintada so when o name muda (ou in pass complete).
static void draw_faixa_frente(lgfx::LovyanGFX& d, const area_t& a, const mesh_conversa_t& c)
{
  static char s_ant[MESH_NOME_CONVERSA + MESH_NOME_LONGO + 4];
  char sig[sizeof(s_ant)];
  snprintf(sig, sizeof(sig), "%s|%s", c.name, c.sub);
  if (!paint_changed_s(&s_p, s_ant, sizeof(s_ant), sig)) { return; }

  const uint16_t FUNDO = background_msg(d);
  tab5_screen_clear(a.x, a.y, a.width, FRENTE_TOPO_A, FUNDO);
  tab5_botao(d, a.x + 16, a.y + FRENTE_VOLTAR_Y, FRENTE_VOLTAR_L, FRENTE_VOLTAR_A,
             "BACK", d.color565(50, 55, 65), TFT_WHITE);
  const int gl = FRENTE_TOPO_A - 24;
  const int gx = a.x + 16 + FRENTE_VOLTAR_L + 20;
  caixa_glifo(d, gx, a.y + 12, gl, gl, c.channel ? GL_DIFUSAO : GL_DIRETO,
              c.channel ? TFT_NAVY : TFT_DARKGREEN, TFT_WHITE);

  // Name and, below, the long one (or "channel N") - clipped character by
  // character to fit, same as in the column.
  const int tx = gx + gl + 16;
  const int max_l = a.x + a.width - 16 - tx;
  char name[MESH_NOME_CONVERSA], sub[MESH_NOME_LONGO];
  snprintf(name, sizeof(name), "%s", c.name);
  snprintf(sub, sizeof(sub), "%s", c.sub);
  d.setTextDatum(top_left);
  tab5_text(d, 3.84f);
  cortar_on_caber(d, name, max_l);
  d.setTextColor(TFT_WHITE, FUNDO);
  d.drawString(name, tx, a.y + (sub[0] ? 10 : 24));
  if (sub[0]) {
    tab5_text(d, 2.4f);
    cortar_on_caber(d, sub, max_l);
    d.setTextColor(TFT_DARKGREY, FUNDO);
    d.drawString(sub, tx, a.y + 50);
  }
  d.drawFastHLine(a.x, a.y + FRENTE_TOPO_A - 1, a.width, SYSTEM_BORDER);
}

static void draw_msgs(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  const int larg = a.x + a.width - x0;
  if (s_conv < 0) {
    conversation_close_keyboard(&s_c);
    list_hide(&s_c.lista);   // sem conversa, nada of baloes para rolar
    if (TUDO) {
      const uint16_t FUNDO = background_msg(d);
      tab5_screen_clear(x0, a.y, larg, a.height, FUNDO);
      tab5_text(d, 3.4f);
      d.setTextDatum(middle_center);
      d.setTextColor(TFT_LIGHTGREY, FUNDO);
      d.drawString("choose a conversation", x0 + larg / 2, a.y + a.height / 2);
      d.setTextDatum(top_left);
    }
    return;
  }
  mesh_conversa_t c = {};
  if (tab5_mesh_conversa(s_conv, &c) && c.nao_lidas > 0) { tab5_mesh_marcar_conversa_lida(s_conv); }
  if (!s_hist_pronto) { carregar_hist(); }

  // Em pe a conversa esta POR CIMA of coluna: a faixa of VOLTAR ocupa o topo.
  int topo = 0;
  if (em_pe(a)) {
    topo = FRENTE_TOPO_A;
    draw_faixa_frente(d, a, c);
  }

  // A conversa e a coluna of right (deitado) ou a area inteira abaixo of
  // faixa (em pe); o keyboard, a width inteira of area nos dois casos.
  const area_t r = { x0, a.y + topo, larg, a.height - topo };
  conversation_area_keyboard(&s_c, a);
  if (c.channel && mesh_canal_internet(c.idx)) {
    static char reason[96];
    snprintf(reason, sizeof(reason), "internet network %s - view only", tab5_mesh_rede_internet_estado());
    conversation_draw(&s_c, d, r, TUDO, tab5_mesh_servico_active() && mesh_mqtt_internet_conectado(), reason);
  } else {
    conversation_draw(&s_c, d, r, TUDO, tab5_mesh_servico_active(),
                      "no Meshtastic node - view only");
  }
}


// ---------------------------------------------------------------------------
// "NODE" PAGE - configure the Meshtastic node from here (services/tab5_mesh_admin)
// ---------------------------------------------------------------------------
// Each row carries an icon for what it does (icone_cfg). Tapping does NOT
// save: it changes the PENDING value (yellow, with a dot). "SAVE TO THE
// NODE" sends everything in a single transaction - the node saves and
// restarts ONCE, no matter how many options changed. Saving on every tap
// would restart the node at every step of a cycle (3, 4, 5...).
//
// After saving, the screen shows whatever the NODE confirmed in the next
// dump, never what we asked for: if it rejects something, the old value
// keeps showing.
//
// Immediate actions (no restart): notification sound and the Tab5's GPS
// feeding the node (both are the Tab5's own), fixed position from the
// Tab5's GPS, setting the node's clock. Restarting asks for confirmation.
//
// EM GRUPOS. A pagina abre in lista dos GRUPOS (Nome e papel, Localizacao,
// Radio, Conexoes, Avisos, Manutencao), cada one com one summary of que tem
// dentro; touch abre o grupo com as lines dele juntas, e VOLTAR sobe. O
// pending vale para a pagina toda: mudar algo em dois grupos e gravar uma vez
// so continua sendo UMA transacao (o rodape aparece nos dois niveis).
enum {
  RC_SOM = 0, RC_LONGO, RC_CURTO, RC_PAPEL, RC_SALTOS, RC_TX,
  RC_LOC_PRECISAO, RC_LOC_TAB5, RC_POS_S, RC_POS_INTELIGENTE, RC_GPS_NO, RC_POS_FIXA,
  RC_NODEINFO_S, RC_TELE_S, RC_MQTT, RC_PROXY, RC_BT, RC_WIFI, RC_HORA, RC_REINICIAR,
  RC_I_FIRMWARE, RC_I_REGIAO, RC_I_NOS, RC_I_REINICIOS, RC_I_AVISO,
  RC_TOTAL
};
static const int RC_EDITAVEIS = RC_I_FIRMWARE;   // daqui para bottom, so leitura
// Linhas que sao of TAB5, nao of no: mudam in hora, com ou sem o no.
static bool rc_of_tab5(int i) { return i == RC_SOM || i == RC_LOC_TAB5; }

enum { G_NOME = 0, G_LOCAL, G_RADIO, G_CONEXOES, G_AVISOS, G_MANUT, G_TOTAL };
typedef struct { const char* name; int8_t icon; int8_t n; int8_t rc[6]; } grupo_cfg_t;
static const grupo_cfg_t GRUPOS[G_TOTAL] = {
  { "Nome e papel",        RC_LONGO,     4, { RC_LONGO, RC_CURTO, RC_PAPEL, RC_NODEINFO_S } },
  { "Location",            RC_POS_S,     6, { RC_LOC_PRECISAO, RC_LOC_TAB5, RC_POS_S, RC_POS_INTELIGENTE,
                                                RC_GPS_NO, RC_POS_FIXA } },
  { "LoRa radio",          RC_TX,        3, { RC_I_REGIAO, RC_SALTOS, RC_TX } },
  { "Connections and internet", RC_WIFI, 4, { RC_BT, RC_WIFI, RC_MQTT, RC_PROXY } },
  { "Avisos e telemetria", RC_SOM,       3, { RC_SOM, RC_TELE_S, RC_I_AVISO } },
  { "Maintenance",         RC_REINICIAR, 5, { RC_HORA, RC_REINICIAR, RC_I_FIRMWARE, RC_I_NOS, RC_I_REINICIOS } },
};
static int s_grupo = -1;   // -1 = a lista dos grupos
static int cfg_n_items(void) { return s_grupo < 0 ? G_TOTAL : GRUPOS[s_grupo].n; }
// Modos of edicao of text of Wi-Fi (s_editando): fora of numeracao das lines.
enum { ED_WIFI_SSID = 100, ED_WIFI_SENHA = 101 };

static const int CFG_LIN   = 68;
static const int CFG_TOPO  = 80;     // cabecalho of pagina (o VOLTAR dos grupos cabe nele)
static const int CFG_RODAPE = 88;    // faixa dos botoes GRAVAR / DESCARTAR
static const int CFG_BTN_L = 360, CFG_BTN_A = 64;
// Largura dos botoes GRAVAR/DESCARTAR: 360 deitado; em pe, o que couber lado
// a lado. Desenho e touch usam esta mesma funcao.
static int cfg_btn_l(const area_t& a)
{
  const int cabe = (a.width - TRILHO_L - 3 * 24) / 2;
  return cabe < CFG_BTN_L ? cabe : CFG_BTN_L;
}

static list_t            s_l_cfg;
static mesh_cfg_t         s_cfg;            // o que o no confirmou (copia por pass)
static mesh_cfg_mudanca_t s_mud;            // o que esta pending
static char               s_cfg_ant[RC_TOTAL][112];
// A line ja foi desenhada ALGUMA vez since a ultima pass complete. Sem
// isto, uma line fora of window tinha assinatura vazia, "mudava" em toda
// pass, e a marca dela (lista_item_mudou marca one INTERVALO, of first
// on ultimo item) arrastava junto todas as lines visiveis entre elas: cada
// sample of arraste redesenhava a window inteira por top of que a rolagem
// acabara of scroll - e o que sobrava disso eram rastros nas bordas.
static bool               s_cfg_pintada[RC_TOTAL];
static int                s_editando = 0;   // 0 = none; RC_LONGO or RC_CURTO
static char               s_edit_txt[66];   // the biggest field: Wi-Fi password (64)
static virtual_keyboard_t          s_kb_cfg;
// REBOOT LOG: full-screen sub-page opened by tapping "node's restarts".
static bool               s_diario = false;
// NODE'S WI-FI NETWORKS: sub-page with the already-used networks, to pick from.
static bool               s_redes_pg = false;
static char               s_redes_msg[112];
static mesh_reinicio_t    s_rei[MESH_DIARIO_MAX];
static int                s_rei_n = 0;
static uint32_t           s_limpar_ms = 0;      // 1st tap on CLEAR: confirms within 4 s
static uint32_t           s_reiniciar_ms = 0;   // 1st tap on RESTART: confirms within 4 s
static char               s_cfg_msg[64];        // short response from the last action


typedef struct { int32_t v; const char* name; } opcao_t;
#define N_OPC(x) ((int)(sizeof(x) / sizeof((x)[0])))

// Papeis oferecidos. REPEATER e ROUTER_CLIENT ficam of fora: obsoletos no
// firmware current. Os outros nao fazem sentido para one no enabled a uma screen.
static const opcao_t OPC_PAPEL[] = {
  { 0, "client" }, { 1, "mute client (does not repeat)" }, { 8, "hidden client" },
  { 5, "rastreador" }, { 6, "sensor" }, { 2, "roteador" }, { 11, "roteador tardio" },
};
static const opcao_t OPC_SALTOS[] = { {1,"1"},{2,"2"},{3,"3"},{4,"4"},{5,"5"},{6,"6"},{7,"7"} };
static const opcao_t OPC_TX[] = {
  { 0, "region maximum" }, { 10, "10 dBm" }, { 14, "14 dBm" }, { 17, "17 dBm" },
  { 20, "20 dBm" }, { 22, "22 dBm" }, { 27, "27 dBm" }, { 30, "30 dBm" },
};
// LOCALIZACAO NA MALHA = precisao of canal primario (ver mesh_cfg_t.precisao).
static const opcao_t OPC_PRECISAO[] = {
  { 0, "do not share" }, { 32, "exact" }, { 18, "approximate (~90 m)" },
  { 16, "approximate (~360 m)" }, { 14, "approximate (~1.5 km)" }, { 12, "city only (~6 km)" },
};
// How many minutes between the Tab5's GPS being pushed to the node (settings.h).
static const opcao_t OPC_TAB5[] = {
  { 0, "do not use" }, { 5, "every 5 min" }, { 15, "every 15 min" }, { 30, "every 30 min" }, { 60, "every 1 h" },
};
static const opcao_t OPC_POS_S[] = {
  { 0, "default (15 min)" }, { 300, "5 min" }, { 900, "15 min" }, { 1800, "30 min" },
  { 3600, "1 h" }, { 21600, "6 h" }, { 43200, "12 h" },
};
static const opcao_t OPC_NODEINFO_S[] = {
  { 0, "default (3 h)" }, { 3600, "1 h" }, { 10800, "3 h" }, { 21600, "6 h" }, { 43200, "12 h" }, { 86400, "24 h" },
};
static const opcao_t OPC_TELE_S[] = {
  { 0, "default (30 min)" }, { 900, "15 min" }, { 1800, "30 min" }, { 3600, "1 h" }, { 7200, "2 h" }, { 21600, "6 h" },
};

static const char* opc_name(const opcao_t* o, int n, int32_t v, char* buf, size_t size, const char* fmt)
{
  for (int i = 0; i < n; ++i) { if (o[i].v == v) { return o[i].name; } }
  snprintf(buf, size, fmt, (long)v);   // valor que nao esta in lista (posto pelo cliente web)
  return buf;
}

static int32_t opc_next(const opcao_t* o, int n, int32_t v)
{
  for (int i = 0; i < n; ++i) { if (o[i].v == v) { return o[(i + 1) % n].v; } }
  return o[0].v;
}

// Precisao fora of lista (posta pelo app of celular): a distancia dela. Cada
// bit a menos dobra a celula; 10 bits ~ 23 km.
static const char* precisao_name(int32_t p, char* buf, size_t size)
{
  for (int i = 0; i < N_OPC(OPC_PRECISAO); ++i) { if (OPC_PRECISAO[i].v == p) { return OPC_PRECISAO[i].name; } }
  if (p < 0)            { return "-"; }
  if (p >= 32 || p < 1) { return p ? "exact" : "do not share"; }
  const uint32_t m = p >= 10 ? (23300u >> (p - 10)) : (23300u << (10 - p));
  if (m >= 1000) { snprintf(buf, size, "aproximada (~%lu km)", (unsigned long)((m + 500) / 1000)); }
  else           { snprintf(buf, size, "aproximada (~%lu m)", (unsigned long)m); }
  return buf;
}

static const char* name_regiao(int r)
{
  static const char* const N[] = {
    "not set", "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW", "RU", "IN",
    "NZ_865", "TH", "LORA_24", "UA_433", "UA_868", "MY_433", "MY_919", "SG_923", "PH_433",
    "PH_868", "PH_915", "ANZ_433", "KZ_433", "KZ_863", "NP_865", "BR_902",
  };
  return (r >= 0 && r < N_OPC(N)) ? N[r] : "outra";
}

static const char* name_preset(int p)
{
  static const char* const N[] = {
    "LongFast", "LongSlow", "VeryLongSlow", "MediumSlow", "MediumFast", "ShortSlow",
    "ShortFast", "LongModerate", "ShortTurbo", "LongTurbo",
  };
  return (p >= 0 && p < N_OPC(N)) ? N[p] : "outro";
}

// Qual field of servico cada line edita (-1 = nenhum).
static int field_of_line(int i)
{
  switch (i) {
    case RC_PAPEL:      return MESH_CFG_PAPEL;
    case RC_SALTOS:     return MESH_CFG_SALTOS;
    case RC_TX:         return MESH_CFG_TX_DBM;
    case RC_POS_S:      return MESH_CFG_POS_S;
    case RC_NODEINFO_S: return MESH_CFG_NODEINFO_S;
    case RC_TELE_S:     return MESH_CFG_TELE_S;
    case RC_MQTT:       return MESH_CFG_MQTT;
    case RC_PROXY:      return MESH_CFG_MQTT_PROXY;
    case RC_BT:         return MESH_CFG_BT;
    case RC_POS_INTELIGENTE: return MESH_CFG_POS_INTELIGENTE;
    case RC_GPS_NO:     return MESH_CFG_GPS_MODO;
    default:            return -1;
  }
}

static int32_t valor_current(int field)
{
  return s_mud.muda[field] ? s_mud.v[field] : s_cfg.v[field];
}

static bool ha_pending(void)
{
  for (int k = 0; k < MESH_CFG_N; ++k) { if (s_mud.muda[k]) { return true; } }
  return s_mud.longo[0] || s_mud.curto[0] || s_mud.wifi_muda || s_mud.wifi_ssid[0] || s_mud.wifi_psk[0]
      || s_mud.precisao_muda;
}

// Rotulo e valor of uma line. 'pend' = valor pending (ainda nao gravado).
static void line_cfg(int i, char* rot, size_t tr, char* val, size_t tv, bool* pend)
{
  char tmp[40];
  *pend = false;
  if (tv) { val[0] = 0; }
  const uint32_t meu = tab5_mesh_meu_numero();
  const int c = field_of_line(i);
  if (c >= 0) {
    *pend = s_mud.muda[c];
    if (!s_cfg.conhecido[c] && !*pend) { snprintf(val, tv, "-"); }
  }
  const int32_t v = (c >= 0) ? valor_current(c) : 0;
  const bool conhecido = (c < 0) || s_cfg.conhecido[c] || *pend;

  switch (i) {
    case RC_SOM:
      snprintf(rot, tr, "notification sound");
      snprintf(val, tv, "%s", tab5_mesh_beep_enabled() ? "on" : "muted");
      break;
    case RC_LONGO: case RC_CURTO: {
      const bool longo = (i == RC_LONGO);
      snprintf(rot, tr, longo ? "long name" : "short name");
      const char* pending = longo ? s_mud.longo : s_mud.curto;
      if (pending[0]) { *pend = true; snprintf(val, tv, "%s", pending); break; }
      char n[MESH_NOME_LONGO] = "";
      if (longo) { tab5_mesh_name_longo_cache(meu, n, sizeof(n)); } else { tab5_mesh_name_cache(meu, n, sizeof(n)); }
      snprintf(val, tv, "%s", n[0] ? n : "-");
      break;
    }
    case RC_PAPEL:
      snprintf(rot, tr, "papel na malha");
      if (conhecido) { snprintf(val, tv, "%s", opc_name(OPC_PAPEL, N_OPC(OPC_PAPEL), v, tmp, sizeof(tmp), "papel %ld")); }
      break;
    case RC_SALTOS:
      snprintf(rot, tr, "saltos (hop limit)");
      if (conhecido) { if (v == 0) { snprintf(val, tv, "3 (default)"); } else { snprintf(val, tv, "%ld", (long)v); } }
      break;
    case RC_TX:
      snprintf(rot, tr, "transmit power");
      if (conhecido) { snprintf(val, tv, "%s", opc_name(OPC_TX, N_OPC(OPC_TX), v, tmp, sizeof(tmp), "%ld dBm")); }
      break;
    case RC_LOC_PRECISAO: {
      // LIGA/DESLIGA a localizacao: e a precisao of canal primario. Com 0 o
      // no nao manda position none - as lines of bottom ficam sem efeito.
      snprintf(rot, tr, "share location");
      *pend = s_mud.precisao_muda;
      snprintf(val, tv, "%s", precisao_name(*pend ? s_mud.precisao : s_cfg.precisao, tmp, sizeof(tmp)));
      break;
    }
    case RC_LOC_TAB5: {
      // DO TAB5 (settings.h), muda in hora. So vai para o no; e o no que
      // difunde, no ritmo of "send a cada" (ver enviar_posicao).
      snprintf(rot, tr, "Tab5's GPS to the node");
      const uint8_t m = settings_mesh_position_interval_min();
      const char* n = opc_name(OPC_TAB5, N_OPC(OPC_TAB5), m, tmp, sizeof(tmp), "a cada %ld min");
      const bool fixa = s_cfg.pos_fixa;
      const bool gps_no = s_cfg.conhecido[MESH_CFG_GPS_MODO] && s_cfg.v[MESH_CFG_GPS_MODO] == 1;
      if (m && (fixa || gps_no)) { snprintf(val, tv, "stopped: the node uses %s", fixa ? "the fixed position" : "its own GPS"); }
      else                       { snprintf(val, tv, "%s", n); }
      break;
    }
    case RC_POS_S:
      snprintf(rot, tr, "send position every");
      if (conhecido) { snprintf(val, tv, "%s", opc_name(OPC_POS_S, N_OPC(OPC_POS_S), v, tmp, sizeof(tmp), "%ld s")); }
      break;
    case RC_POS_INTELIGENTE:
      snprintf(rot, tr, "also send when moving");
      if (conhecido) { snprintf(val, tv, "%s", v ? "on" : "off"); }
      break;
    case RC_GPS_NO:
      snprintf(rot, tr, "the node's own GPS");
      if (conhecido) {
        if (v == 2)      { snprintf(val, tv, "the board has no GPS"); }
        else             { snprintf(val, tv, "%s", v == 1 ? "on" : "off"); }
      }
      break;
    case RC_POS_FIXA: {
      snprintf(rot, tr, "node's fixed position");
      const nmea_position_t g = tab5_gps_position();
      const bool fix = g.valid && g.qualidade > 0;
      if (s_cfg.pos_fixa) { snprintf(val, tv, "fixed - tap to release"); }
      else if (fix)       { snprintf(val, tv, "tap to fix to the Tab5's GPS"); }
      else                { snprintf(val, tv, "the Tab5's GPS has no signal"); }
      break;
    }
    case RC_NODEINFO_S:
      snprintf(rot, tr, "announce the node every");
      if (conhecido) { snprintf(val, tv, "%s", opc_name(OPC_NODEINFO_S, N_OPC(OPC_NODEINFO_S), v, tmp, sizeof(tmp), "%ld s")); }
      break;
    case RC_TELE_S:
      snprintf(rot, tr, "telemetry every");
      if (conhecido) { snprintf(val, tv, "%s", opc_name(OPC_TELE_S, N_OPC(OPC_TELE_S), v, tmp, sizeof(tmp), "%ld s")); }
      break;
    case RC_MQTT:
      snprintf(rot, tr, "node's MQTT");
      if (conhecido) { snprintf(val, tv, "%s", v ? "on" : "off"); }
      break;
    case RC_PROXY:
      // TAB5'S INTERNET FOR THE NODE (MQTT Client Proxy). The field belongs
      // to the node (proxy_to_client_enabled); once it's on and confirmed,
      // the Tab5's service carries what the node publishes to the server.
      // Next to it, the bridge's current state.
      snprintf(rot, tr, "Tab5's internet (proxy)");
      if (conhecido) {
        if (v && !*pend) { snprintf(val, tv, "on  ·  %s", tab5_mesh_proxy_estado()); }
        else             { snprintf(val, tv, "%s", v ? "on" : "off"); }
      }
      break;
    case RC_BT:
      snprintf(rot, tr, "node's Bluetooth");
      if (conhecido) {
        snprintf(val, tv, "%s%s", v ? "on" : "off",
                 tab5_mesh_via() == SETTINGS_MESH_TRANSPORT_BLUETOOTH ? " (in use by the Tab5)" : "");
      }
      break;
    case RC_WIFI: {
      snprintf(rot, tr, "node's Wi-Fi");
      if (s_mud.wifi_ssid[0]) { *pend = true; snprintf(val, tv, "turn on at %s", s_mud.wifi_ssid); }
      else if (s_mud.wifi_muda) { *pend = true; snprintf(val, tv, "%s", s_mud.wifi_liga ? "turn on" : "turn off"); }
      else if (!s_cfg.wifi_conhecido) { snprintf(val, tv, "-"); }
      else if (!s_cfg.wifi_enabled) { snprintf(val, tv, "off"); }
      else { snprintf(val, tv, "on%s%s", s_cfg.wifi_ssid[0] ? "  ·  " : "", s_cfg.wifi_ssid); }
      break;
    }
    case RC_HORA:
      snprintf(rot, tr, "node's clock");
      snprintf(val, tv, "tocar para acertar com o Tab5");
      break;
    case RC_REINICIAR:
      snprintf(rot, tr, "restart the node");
      snprintf(val, tv, (s_reiniciar_ms && (tab5_now_ms() - s_reiniciar_ms) < 4000) ? "toque de novo para confirmar" : "tocar para reiniciar");
      break;
    case RC_I_FIRMWARE: {
      snprintf(rot, tr, "firmware / placa");
      const char* hw = mesh_hw_name(s_cfg.hw);
      snprintf(val, tv, "%s / %s", s_cfg.firmware[0] ? s_cfg.firmware : "-", hw ? hw : "-");
      break;
    }
    case RC_I_REGIAO:
      snprintf(rot, tr, "region / modem");
      if (s_cfg.regiao >= 0) {
        snprintf(val, tv, "%s / %s", name_regiao(s_cfg.regiao), s_cfg.usa_preset ? name_preset(s_cfg.preset) : "personalizado");
      } else { snprintf(val, tv, "-"); }
      break;
    case RC_I_NOS:
      snprintf(rot, tr, "stored nodes");
      if (s_cfg.nos_no_radio >= 0) { snprintf(val, tv, "radio %ld  ·  Tab5 %d", (long)s_cfg.nos_no_radio, tab5_mesh_num_nos()); }
      else                         { snprintf(val, tv, "Tab5 %d  (the radio firmware does not report it)", tab5_mesh_num_nos()); }
      break;
    case RC_I_REINICIOS:
      snprintf(rot, tr, "node restarts");
      snprintf(val, tv, "%lu  ·  tap to see the log", (unsigned long)s_cfg.reinicios);
      break;
    case RC_I_AVISO:
      snprintf(rot, tr, "node's last notice");
      snprintf(val, tv, "%s", s_cfg.aviso[0] ? s_cfg.aviso : "-");
      break;
  }
}

// ---- icones das lines of configuracao --------------------------------------
// Cada line leva one drawing enabled on que ela faz, no lugar of engrenagem
// unica. Cabem em ~28 px em iteration of (cx, cy); everything com primitivas, sem
// imagem guardada. 'color' e a tinta; o furo/fundo e sempre preto.
static void icon_cfg(lgfx::LovyanGFX& d, int i, int cx, int cy, uint16_t color)
{
  const uint16_t F = TFT_BLACK;
  switch (i) {
    case RC_SOM:        // alto-falante com ondas
      d.fillRect(cx - 11, cy - 4, 6, 8, color);
      d.fillTriangle(cx - 5, cy - 4, cx + 2, cy - 11, cx + 2, cy + 11, color);
      d.fillTriangle(cx - 5, cy + 4, cx + 2, cy - 11, cx + 2, cy + 11, color);
      d.fillArc(cx + 3, cy, 7, 5, -45, 45, color);
      d.fillArc(cx + 3, cy, 12, 10, -45, 45, color);
      break;
    case RC_LONGO: case RC_CURTO: {   // crachá com name: longo = duas lines, curto = uma
      d.drawRoundRect(cx - 13, cy - 9, 26, 18, 3, color);
      d.drawRoundRect(cx - 12, cy - 8, 24, 16, 3, color);
      d.fillCircle(cx - 7, cy - 2, 3, color);
      d.fillRect(cx - 10, cy + 2, 7, 3, color);
      d.fillRect(cx - 1, cy - 4, 10, 2, color);
      if (i == RC_LONGO) { d.fillRect(cx - 1, cy + 1, 10, 2, color); }
      break;
    }
    case RC_PAPEL:      // pessoa
      d.fillCircle(cx, cy - 5, 5, color);
      d.fillArc(cx, cy + 12, 12, 0, 180, 360, color);
      break;
    case RC_SALTOS:     // tres nos em cadeia
      d.drawLine(cx - 10, cy + 6, cx, cy - 6, color);
      d.drawLine(cx, cy - 6, cx + 10, cy + 6, color);
      d.fillCircle(cx - 10, cy + 6, 4, color);
      d.fillCircle(cx, cy - 6, 4, color);
      d.fillCircle(cx + 10, cy + 6, 4, color);
      break;
    case RC_TX:         // antena com ondas
      d.fillTriangle(cx - 5, cy + 12, cx + 5, cy + 12, cx, cy - 2, color);
      d.fillCircle(cx, cy - 5, 3, color);
      d.fillArc(cx, cy - 5, 9, 7, -40, 40, color);
      d.fillArc(cx, cy - 5, 9, 7, 140, 220, color);
      d.fillArc(cx, cy - 5, 14, 12, -40, 40, color);
      d.fillArc(cx, cy - 5, 14, 12, 140, 220, color);
      break;
    case RC_POS_S: case RC_POS_FIXA: case RC_LOC_PRECISAO: case RC_POS_INTELIGENTE:
      // alfinete of mapa; o fixo leva a base, o "on se move" one rastro
      d.fillCircle(cx, cy - 4, 8, color);
      d.fillTriangle(cx - 7, cy, cx + 7, cy, cx, cy + 10, color);
      d.fillCircle(cx, cy - 4, 3, F);
      if (i == RC_POS_FIXA) { d.fillRect(cx - 9, cy + 11, 18, 3, color); }
      if (i == RC_POS_INTELIGENTE) { d.fillRect(cx - 14, cy + 11, 4, 3, color); d.fillRect(cx - 7, cy + 11, 4, 3, color); }
      if (i == RC_LOC_PRECISAO) { d.drawCircle(cx, cy - 4, 12, color); }
      break;
    case RC_LOC_TAB5:   // o tablet com o alfinete in screen
      d.drawRoundRect(cx - 12, cy - 11, 24, 22, 3, color);
      d.drawRoundRect(cx - 11, cy - 10, 22, 20, 3, color);
      d.fillCircle(cx, cy - 2, 4, color);
      d.fillTriangle(cx - 4, cy, cx + 4, cy, cx, cy + 6, color);
      break;
    case RC_GPS_NO:     // mira of GPS
      d.drawCircle(cx, cy, 9, color);
      d.drawCircle(cx, cy, 8, color);
      d.fillCircle(cx, cy, 3, color);
      d.fillRect(cx - 1, cy - 14, 3, 6, color);
      d.fillRect(cx - 1, cy + 9, 3, 6, color);
      d.fillRect(cx - 14, cy - 1, 6, 3, color);
      d.fillRect(cx + 9, cy - 1, 6, 3, color);
      break;
    case RC_NODEINFO_S: // cartao of identificacao
      d.drawRoundRect(cx - 13, cy - 9, 26, 18, 3, color);
      d.drawRoundRect(cx - 12, cy - 8, 24, 16, 3, color);
      d.fillCircle(cx - 6, cy - 2, 3, color);
      d.fillArc(cx - 6, cy + 8, 6, 0, 180, 360, color);
      d.fillRect(cx + 1, cy - 4, 9, 2, color);
      d.fillRect(cx + 1, cy + 1, 9, 2, color);
      break;
    case RC_TELE_S:     // barras of grafico
      d.fillRect(cx - 11, cy + 2, 6, 9, color);
      d.fillRect(cx - 3, cy - 4, 6, 15, color);
      d.fillRect(cx + 5, cy - 11, 6, 22, color);
      break;
    case RC_MQTT:       // nuvem
      d.fillCircle(cx - 7, cy + 2, 6, color);
      d.fillCircle(cx + 1, cy - 3, 8, color);
      d.fillCircle(cx + 8, cy + 3, 5, color);
      d.fillRect(cx - 7, cy + 2, 16, 7, color);
      break;
    case RC_PROXY:      // nuvem pequena com seta nos dois sentidos: a ponte
      d.fillCircle(cx - 6, cy - 4, 5, color);
      d.fillCircle(cx + 1, cy - 8, 6, color);
      d.fillCircle(cx + 7, cy - 3, 4, color);
      d.fillRect(cx - 6, cy - 4, 13, 5, color);
      d.fillRect(cx - 1, cy + 3, 3, 9, color);
      d.fillTriangle(cx - 5, cy + 6, cx + 5, cy + 6, cx, cy + 1, color);
      d.fillTriangle(cx - 5, cy + 9, cx + 5, cy + 9, cx, cy + 14, color);
      break;
    case RC_BT: {       // runa of Bluetooth
      const int t = cy - 12, b = cy + 12, dx = 7, q = 6;
      for (int k = -1; k <= 1; ++k) {
        d.drawLine(cx + k, t, cx + k, b, color);
        d.drawLine(cx + k, t, cx + dx + k, t + q, color);
        d.drawLine(cx + dx + k, t + q, cx - dx + k, cy + q, color);
        d.drawLine(cx + k, b, cx + dx + k, b - q, color);
        d.drawLine(cx + dx + k, b - q, cx - dx + k, cy - q, color);
      }
      break;
    }
    case RC_WIFI:       // arcos of Wi-Fi
      for (int k = 0; k < 3; ++k) { const int r = 7 + 6 * k; d.fillArc(cx, cy + 9, r, r - 3, 225, 315, color); }
      d.fillCircle(cx, cy + 8, 2, color);
      break;
    case RC_HORA:       // clock
      d.drawCircle(cx, cy, 12, color);
      d.drawCircle(cx, cy, 11, color);
      d.drawLine(cx, cy, cx, cy - 8, color);
      d.drawLine(cx, cy, cx + 6, cy + 3, color);
      d.fillCircle(cx, cy, 2, color);
      break;
    case RC_REINICIAR: case RC_I_REINICIOS:   // seta circular
      d.fillArc(cx, cy, 11, 8, 40, 330, color);
      d.fillTriangle(cx + 14, cy - 7, cx + 3, cy - 9, cx + 10, cy + 2, color);
      break;
    case RC_I_FIRMWARE: // chip
      d.fillRoundRect(cx - 8, cy - 8, 16, 16, 3, color);
      d.fillRect(cx - 4, cy - 4, 8, 8, F);
      for (int k = -1; k <= 1; ++k) {
        d.fillRect(cx - 12, cy + k * 5 - 1, 4, 2, color);
        d.fillRect(cx + 8,  cy + k * 5 - 1, 4, 2, color);
        d.fillRect(cx + k * 5 - 1, cy - 12, 2, 4, color);
        d.fillRect(cx + k * 5 - 1, cy + 8,  2, 4, color);
      }
      break;
    case RC_I_REGIAO:   // globo
      d.drawCircle(cx, cy, 12, color);
      d.drawCircle(cx, cy, 11, color);
      d.drawEllipse(cx, cy, 5, 12, color);
      d.drawFastHLine(cx - 12, cy, 25, color);
      d.drawFastHLine(cx - 10, cy - 6, 21, color);
      d.drawFastHLine(cx - 10, cy + 6, 21, color);
      break;
    case RC_I_NOS:      // rede of nos
      d.drawLine(cx, cy, cx - 10, cy - 8, color);
      d.drawLine(cx, cy, cx + 10, cy - 8, color);
      d.drawLine(cx, cy, cx, cy + 11, color);
      d.fillCircle(cx, cy, 4, color);
      d.fillCircle(cx - 10, cy - 8, 3, color);
      d.fillCircle(cx + 10, cy - 8, 3, color);
      d.fillCircle(cx, cy + 11, 3, color);
      break;
    case RC_I_AVISO:    // triangulo of aviso
      d.fillTriangle(cx, cy - 12, cx - 13, cy + 10, cx + 13, cy + 10, color);
      d.fillRect(cx - 1, cy - 4, 3, 8, F);
      d.fillRect(cx - 1, cy + 6, 3, 2, F);
      break;
    default:
      d.fillCircle(cx, cy, 4, color);
      break;
  }
}

static const uint16_t* tinta_cfg(lgfx::LovyanGFX& d, int i)
{
  static uint16_t t[RC_TOTAL];
  static bool pronto = false;
  if (!pronto) {
    pronto = true;
    for (int k = 0; k < RC_TOTAL; ++k) { t[k] = d.color565(110, 120, 135); }   // so leitura: cinza-azulado
    t[RC_SOM] = d.color565(255, 214, 90);
    t[RC_LONGO] = t[RC_CURTO] = d.color565(120, 200, 255);
    t[RC_PAPEL] = d.color565(255, 160, 90);
    t[RC_SALTOS] = d.color565(190, 150, 255);
    t[RC_TX] = d.color565(255, 110, 110);
    t[RC_POS_S] = t[RC_POS_FIXA] = t[RC_LOC_PRECISAO] = t[RC_LOC_TAB5] = d.color565(120, 220, 130);
    t[RC_POS_INTELIGENTE] = t[RC_GPS_NO] = d.color565(120, 220, 130);
    t[RC_NODEINFO_S] = d.color565(120, 200, 255);
    t[RC_TELE_S] = d.color565(90, 220, 210);
    t[RC_MQTT] = d.color565(150, 190, 255);
    t[RC_PROXY] = d.color565(120, 230, 200);
    t[RC_BT] = d.color565(80, 140, 255);
    t[RC_WIFI] = d.color565(60, 170, 255);
    t[RC_HORA] = d.color565(200, 200, 210);
    t[RC_REINICIAR] = d.color565(255, 140, 80);
  }
  return &t[i];
}

static int cfg_height(int, void*) { return CFG_LIN; }

// RESUMO DE UM GRUPO, in lista of top: o essencial of que esta dentro, para
// nao precisar open so para conferir. 'pend' = algo pending la dentro.
static void line_grupo(int g, char* rot, size_t tr, char* val, size_t tv, bool* pend)
{
  char r2[48], v2[96], tmp[40];
  bool p2 = false;
  *pend = false;
  for (int k = 0; k < GRUPOS[g].n; ++k) {
    line_cfg(GRUPOS[g].rc[k], r2, sizeof(r2), v2, sizeof(v2), &p2);
    *pend |= p2;
  }
  snprintf(rot, tr, "%s", GRUPOS[g].name);
  const bool pronto = s_cfg.conhecido[MESH_CFG_PAPEL];
  switch (g) {
    case G_NOME: {
      char n[MESH_NOME_LONGO] = "";
      tab5_mesh_name_longo_cache(tab5_mesh_meu_numero(), n, sizeof(n));
      if (s_mud.longo[0]) { snprintf(n, sizeof(n), "%s", s_mud.longo); }
      line_cfg(RC_PAPEL, r2, sizeof(r2), v2, sizeof(v2), &p2);
      snprintf(val, tv, "%s  ·  %s", n[0] ? n : "-", pronto ? v2 : "-");
      break;
    }
    case G_LOCAL: {
      const int32_t p = s_mud.precisao_muda ? s_mud.precisao : s_cfg.precisao;
      if (p == 0) { snprintf(val, tv, "not shared"); break; }
      if (p < 0)  { snprintf(val, tv, "-"); break; }
      line_cfg(RC_POS_S, r2, sizeof(r2), v2, sizeof(v2), &p2);
      snprintf(val, tv, "%s  ·  %s%s", precisao_name(p, tmp, sizeof(tmp)), v2,
               valor_current(MESH_CFG_POS_INTELIGENTE) ? " and on move" : "");
      break;
    }
    case G_RADIO:
      line_cfg(RC_SALTOS, r2, sizeof(r2), v2, sizeof(v2), &p2);
      if (s_cfg.regiao >= 0) {
        snprintf(val, tv, "%s / %s  ·  %s hops", name_regiao(s_cfg.regiao),
                 s_cfg.usa_preset ? name_preset(s_cfg.preset) : "custom", v2);
      } else { snprintf(val, tv, "-"); }
      break;
    case G_CONEXOES: {
      if (!pronto) { snprintf(val, tv, "-"); break; }
      char w[48];
      if (s_cfg.wifi_conhecido && s_cfg.wifi_enabled) { snprintf(w, sizeof(w), "Wi-Fi %s", s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "on"); }
      else                                           { snprintf(w, sizeof(w), "Wi-Fi off"); }
      snprintf(val, tv, "Bluetooth %s  ·  %s  ·  MQTT %s", valor_current(MESH_CFG_BT) ? "yes" : "no", w,
               !valor_current(MESH_CFG_MQTT) ? "no" : valor_current(MESH_CFG_MQTT_PROXY) ? "via the Tab5" : "yes");
      break;
    }
    case G_AVISOS:
      line_cfg(RC_TELE_S, r2, sizeof(r2), v2, sizeof(v2), &p2);
      snprintf(val, tv, "sound %s  ·  telemetry %s", tab5_mesh_beep_enabled() ? "on" : "muted", pronto ? v2 : "-");
      break;
    case G_MANUT:
      snprintf(val, tv, "%s  ·  %lu restarts", s_cfg.firmware[0] ? s_cfg.firmware : "-", (unsigned long)s_cfg.reinicios);
      break;
  }
}

// O text of item 'i' of lista of momento (grupo ou line).
static void line_item(int i, char* rot, size_t tr, char* val, size_t tv, bool* pend)
{
  if (s_grupo < 0) { line_grupo(i, rot, tr, val, tv, pend); }
  else             { line_cfg(GRUPOS[s_grupo].rc[i], rot, tr, val, tv, pend); }
}

// Um grupo: icon, name, summary e a seta of "abre".
static void grupo_draw(lgfx::LovyanGFX& d, int g, int x, int y, int width)
{
  char rot[48], val[96] = "";
  bool pend = false;
  line_grupo(g, rot, sizeof(rot), val, sizeof(val), &pend);
  snprintf(s_cfg_ant[g], sizeof(s_cfg_ant[g]), "%d|%s", (int)pend, val);
  s_cfg_pintada[g] = true;

  d.fillRect(x, y, width, CFG_LIN, TFT_BLACK);
  icon_cfg(d, GRUPOS[g].icon, x + 26, y + CFG_LIN / 2, *tinta_cfg(d, GRUPOS[g].icon));
  d.setTextDatum(middle_left);
  tab5_text(d, 3.6f);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(rot, x + 52, y + CFG_LIN / 2);
  const int rot_end = x + 52 + (int)d.textWidth(rot) + 24;

  // A seta: este item abre outra lista.
  const int sx = x + width - 30, sy = y + CFG_LIN / 2;
  d.drawLine(sx, sy - 9, sx + 9, sy, TFT_DARKGREY); d.drawLine(sx + 9, sy, sx, sy + 9, TFT_DARKGREY);
  d.drawLine(sx + 1, sy - 9, sx + 10, sy, TFT_DARKGREY); d.drawLine(sx + 10, sy, sx + 1, sy + 9, TFT_DARKGREY);

  tab5_text(d, 2.8f);
  d.setTextColor(pend ? TFT_YELLOW : TFT_LIGHTGREY, TFT_BLACK);
  char v2[100];
  snprintf(v2, sizeof(v2), pend ? "• %s" : "%s", val);
  const int vx = em_pe(system_area()) ? rot_end : (rot_end > x + 440 ? rot_end : x + 440);
  cortar_on_caber(d, v2, sx - 16 - vx);
  d.drawString(v2, vx, y + CFG_LIN / 2);
  d.drawFastHLine(x + 16, y + CFG_LIN - 1, width - 32, d.color565(38, 42, 50));
  d.setTextDatum(top_left);
}

static void cfg_draw(lgfx::LovyanGFX& d, int item, int x, int y, int width, void*)
{
  if (s_grupo < 0) { grupo_draw(d, item, x, y, width); return; }
  const int i = GRUPOS[s_grupo].rc[item];
  char rot[48], val[96] = "";
  bool pend = false;
  line_cfg(i, rot, sizeof(rot), val, sizeof(val), &pend);
  snprintf(s_cfg_ant[item], sizeof(s_cfg_ant[item]), "%d|%s", (int)pend, val);
  s_cfg_pintada[item] = true;

  // A LINHA INTEIRA, sempre: one redraw cobre everything o que havia nela.
  d.fillRect(x, y, width, CFG_LIN, TFT_BLACK);

  const bool editavel = (i < RC_EDITAVEIS);
  // Linha of radio sem no of outro lado: o icon stays apagado - ver,
  // sim; mudar, nao (so as of proprio Tab5).
  const bool active = editavel && (rc_of_tab5(i) || s_cfg.state == MESH_CFG_PRONTO);
  // Icone of line: colorido when of para mexer, apagado sem o no of outro lado.
  icon_cfg(d, i, x + 26, y + CFG_LIN / 2,
            editavel ? (active ? *tinta_cfg(d, i) : d.color565(60, 64, 72)) : d.color565(90, 98, 112));

  d.setTextDatum(middle_left);
  tab5_text(d, 3.2f);
  d.setTextColor(editavel ? TFT_LIGHTGREY : TFT_DARKGREY, TFT_BLACK);
  d.drawString(rot, x + 52, y + CFG_LIN / 2);
  const int rot_end = x + 52 + (int)d.textWidth(rot) + 24;

  tab5_text(d, 3.6f);
  d.setTextColor(pend ? TFT_YELLOW : (active || !editavel ? TFT_WHITE : TFT_DARKGREY), TFT_BLACK);
  char v2[100];
  snprintf(v2, sizeof(v2), pend ? "• %s" : "%s", val);
  if (em_pe(system_area())) {
    // EM PE nao ha width para a coluna fixa em 440: o valor encosta in
    // right e ocupa o que o rotulo deixar.
    cortar_on_caber(d, v2, x + width - 16 - rot_end);
    d.setTextDatum(middle_right);
    d.drawString(v2, x + width - 16, y + CFG_LIN / 2);
    d.setTextDatum(middle_left);
  } else {
    // Corte por caractere, para nao invadir a borda.
    cortar_on_caber(d, v2, width - 470);
    d.drawString(v2, x + 440, y + CFG_LIN / 2);
  }
  d.drawFastHLine(x + 16, y + CFG_LIN - 1, width - 32, d.color565(38, 42, 50));
  d.setTextDatum(top_left);
}

// O rodape so existe com algo pending: sem ele, a lista vai ate o end of
// area. Quando ele aparece ou some, a window muda of tamanho e a lista se
// redesenha inteira sozinha (lista_desenhar compara com a window anterior).
static area_t window_cfg(const area_t& a, int x0)
{
  const int rodape = ha_pending() ? CFG_RODAPE : 0;
  return area_t{ x0, a.y + CFG_TOPO, a.x + a.width - x0, a.height - CFG_TOPO - rodape };
}

// ---- diario of reinicios ---------------------------------------------------
static const int REI_LIN = 126, REI_TOPO = 80, REI_RODAPE = 84;

static void rei_duracao(char* o, size_t n, uint32_t s)
{
  if (s >= 3600)     { snprintf(o, n, "%lu h %02lu min", (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60)); }
  else if (s >= 60)  { snprintf(o, n, "%lu min", (unsigned long)(s / 60)); }
  else               { snprintf(o, n, "%lu s", (unsigned long)s); }
}


static void draw_diario(lgfx::LovyanGFX& d, const area_t& a, int x0, int larg)
{
  list_hide(&s_l_cfg);
  const bool conf = s_limpar_ms && (tab5_now_ms() - s_limpar_ms) < 4000;
  const uint32_t assin = mesh_diario_versao() * 2u + (conf ? 1u : 0u);
  static uint32_t s_assin_ant = 0xFFFFFFFFu;
  if (!TUDO && assin == s_assin_ant) { return; }
  s_assin_ant = assin;
  s_rei_n = mesh_diario_read(s_rei, MESH_DIARIO_MAX);

  tab5_screen_clear(x0, a.y, larg, a.height, TFT_BLACK);
  tab5_botao(d, x0 + 24, a.y + 12, 200, 56, "BACK", d.color565(50, 55, 65), TFT_WHITE);
  tab5_text(d, 3.6f);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Node restarts", x0 + 250, a.y + 40);

  const int cabe = (a.height - REI_TOPO - REI_RODAPE) / REI_LIN;
  const int mostra = s_rei_n < cabe ? s_rei_n : cabe;
  const int max_l = larg - 64;
  if (s_rei_n == 0) {
    tab5_text(d, 2.8f);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString("no restart logged yet", x0 + 32, a.y + REI_TOPO + 40);
  }
  for (int i = 0; i < mostra; ++i) {
    const mesh_reinicio_t& r = s_rei[i];
    const int y = a.y + REI_TOPO + i * REI_LIN;
    char hora[40], dur[24], l1[120], l2[120], l3[140];
    if (r.epoch) {
      const time_t t = (time_t)r.epoch;
      struct tm tmv; localtime_r(&t, &tmv);
      strftime(hora, sizeof(hora), "%d/%m %H:%M", &tmv);
    } else {
      rei_duracao(dur, sizeof(dur), r.up_tab5_s);
      snprintf(hora, sizeof(hora), "Tab5 on for %s", dur);
    }
    static const char* const ORI[] = { "first read", "on its own", "requested by the Tab5", "with the Tab5 disconnected" };
    snprintf(l1, sizeof(l1), "%s  ·  contador %lu  ·  %s", hora, (unsigned long)r.contador, ORI[r.origem < 4 ? r.origem : 1]);
    l2[0] = 0;
    if (r.origem != MESH_REI_BASE) {
      if (r.up_no_s) { rei_duracao(dur, sizeof(dur), r.up_no_s); snprintf(l2, sizeof(l2), "estava ligado ~%s", dur); }
      if (r.centivolts) {
        const size_t k = strlen(l2);
        snprintf(l2 + k, sizeof(l2) - k, "%sbateria %.2f V %ld mA", k ? "  ·  " : "", r.centivolts / 100.0, (long)r.ma);
      }
    }
    snprintf(l3, sizeof(l3), "%s", mesh_diario_veredito(&r));
    const uint16_t c1 = r.origem == MESH_REI_SOZINHO ? TFT_ORANGE : r.origem == MESH_REI_BASE ? TFT_DARKGREY
                      : r.origem == MESH_REI_AUSENTE ? TFT_YELLOW : TFT_LIGHTGREY;
    const bool sem_indicio = strncmp(l3, "no indication", 13) == 0;
    const uint16_t c3 = (r.origem == MESH_REI_SOZINHO && !sem_indicio) ? TFT_RED : TFT_CYAN;
    tab5_text(d, 2.8f);
    d.setTextDatum(top_left);
    cortar_on_caber(d, l1, max_l); cortar_on_caber(d, l2, max_l); cortar_on_caber(d, l3, max_l);
    d.setTextColor(c1, TFT_BLACK);        d.drawString(l1, x0 + 32, y + 6);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK); d.drawString(l2, x0 + 32, y + 42);
    d.setTextColor(c3, TFT_BLACK);        d.drawString(l3, x0 + 32, y + 78);
    d.drawFastHLine(x0 + 16, y + REI_LIN - 4, larg - 32, d.color565(38, 42, 50));
  }
  const int by = a.y + a.height - REI_RODAPE + 14;
  tab5_botao(d, x0 + 24, by, 320, 56, conf ? "TAP AGAIN" : "CLEAR LOG",
             conf ? TFT_RED : d.color565(50, 55, 65), TFT_WHITE);
  tab5_text(d, 2.4f);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  char rod[80];
  if (s_rei_n > mostra) { snprintf(rod, sizeof(rod), "+%d older (on the card: Meshtastic/reinicios.txt)", s_rei_n - mostra); }
  else                  { snprintf(rod, sizeof(rod), "the exact reason only shows in the node's USB log; this is a guess"); }
  cortar_on_caber(d, rod, larg - (24 + 320 + 24) - 16);
  d.drawString(rod, x0 + 24 + 320 + 24, by + 28);
  d.setTextDatum(top_left);
}

// ---- Node's Wi-Fi: everything on one screen ---------------------------------
// On/off switch up top; with the node's Wi-Fi on, the list of already-used
// networks (tap chooses, the x forgets) and the NEW NETWORK / PULL FROM TAB5
// actions. Toggling the switch or choosing a network is PENDING: SAVE TO THE
// NODE sends it and the node restarts, like any other change.
static const int RD_LIN = 78, RD_BTN_A = 60;
static const int WF_TOG_Y = 84, WF_TOG_H = 100, WF_LISTA_Y = 84 + 100 + 56;

static void interruptor(lgfx::LovyanGFX& d, int x, int y, int w, int h, bool enabled, bool active)
{
  const uint16_t trilho = !active ? d.color565(55, 58, 66) : enabled ? d.color565(52, 199, 89) : d.color565(110, 110, 118);
  d.fillRoundRect(x, y, w, h, h / 2, trilho);
  const int r = h / 2 - 6;
  d.fillCircle(enabled ? x + w - h / 2 : x + h / 2, y + h / 2, r, active ? TFT_WHITE : d.color565(150, 150, 158));
}

static uint32_t fnv_str(const char* t)
{
  uint32_t h = 2166136261u;
  for (; *t; ++t) { h = (h ^ (uint8_t)*t) * 16777619u; }
  return h;
}

static bool wf_liga_visivel(void) { return s_mud.wifi_muda ? s_mud.wifi_liga : s_cfg.wifi_enabled; }
// The list only exists once the node's Wi-Fi is actually ON (after a restart).
static bool wf_lista_visivel(void) { return s_cfg.wifi_conhecido && s_cfg.wifi_enabled && wf_liga_visivel(); }
// 0 = no buttons, 1 = SAVE/DISCARD, 2 = NEW NETWORK/PULL FROM TAB5
static int wf_botoes(void) { return ha_pending() ? 1 : (wf_lista_visivel() ? 2 : 0); }

static void draw_redes(lgfx::LovyanGFX& d, const area_t& a, int x0, int larg)
{
  list_hide(&s_l_cfg);
  const uint32_t assin = mesh_wifi_no_versao() * 31u + fnv_str(s_redes_msg) + fnv_str(s_cfg.wifi_ssid) * 7u
                       + fnv_str(s_mud.wifi_ssid) * 13u + (uint32_t)s_cfg.state * 131u + (s_cfg.wifi_enabled ? 977u : 0)
                       + (s_mud.wifi_muda ? 1999u : 0) + (s_mud.wifi_liga ? 4001u : 0) + (ha_pending() ? 8009u : 0);
  static uint32_t s_assin_ant = 0xFFFFFFFFu;
  if (!TUDO && assin == s_assin_ant) { return; }
  s_assin_ant = assin;

  tab5_screen_clear(x0, a.y, larg, a.height, TFT_BLACK);
  tab5_botao(d, x0 + 24, a.y + 12, 200, 56, "BACK", d.color565(50, 55, 65), TFT_WHITE);
  tab5_text(d, 3.6f);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Node's Wi-Fi", x0 + 250, a.y + 40);

  // Linha of interruptor.
  const bool liga = wf_liga_visivel();
  const bool pode = s_cfg.state == MESH_CFG_PRONTO && s_cfg.wifi_conhecido;
  icon_cfg(d, RC_WIFI, x0 + 50, a.y + WF_TOG_Y + WF_TOG_H / 2, pode ? d.color565(60, 170, 255) : d.color565(60, 64, 72));
  tab5_text(d, 3.6f);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Node's Wi-Fi", x0 + 96, a.y + WF_TOG_Y + 30);
  tab5_text(d, 2.8f);
  char sub[96];
  uint16_t csub = TFT_DARKGREY;
  if (!s_cfg.wifi_conhecido)               { snprintf(sub, sizeof(sub), "the node has not sent the network config yet"); }
  else if (s_mud.wifi_muda && s_mud.wifi_liga != s_cfg.wifi_enabled) {
    snprintf(sub, sizeof(sub), "will %s on save - the node restarts", s_mud.wifi_liga ? "turn on" : "turn off"); csub = TFT_YELLOW;
  }
  else if (s_cfg.wifi_enabled)              { snprintf(sub, sizeof(sub), "on%s%s", s_cfg.wifi_ssid[0] ? " on " : "", s_cfg.wifi_ssid); csub = TFT_GREENYELLOW; }
  else                                     { snprintf(sub, sizeof(sub), "off"); }
  d.setTextColor(csub, TFT_BLACK);
  cortar_on_caber(d, sub, larg - 96 - 24 - 132 - 24);   // para antes of interruptor
  d.drawString(sub, x0 + 96, a.y + WF_TOG_Y + 70);
  interruptor(d, x0 + larg - 24 - 132, a.y + WF_TOG_Y + (WF_TOG_H - 68) / 2, 132, 68, liga, pode);
  d.drawFastHLine(x0 + 16, a.y + WF_TOG_Y + WF_TOG_H, larg - 32, d.color565(38, 42, 50));

  // Lista of redes.
  const int botoes = wf_botoes();
  const int reserva = (botoes ? RD_BTN_A + 14 + 14 : 0) + 40;   // botoes + message
  if (wf_lista_visivel()) {
    tab5_text(d, 2.4f);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString("NETWORKS  (tap to choose)", x0 + 32, a.y + WF_LISTA_Y - 24);
    const int n = mesh_wifi_no_n();
    const int cabe = (a.height - WF_LISTA_Y - reserva) / RD_LIN;
    if (n == 0) {
      tab5_text(d, 2.8f);
      d.setTextColor(TFT_DARKGREY, TFT_BLACK);
      char t0[64];
      snprintf(t0, sizeof(t0), "no saved networks - use NEW NETWORK or PULL FROM TAB5");
      cortar_on_caber(d, t0, larg - 64);
      d.drawString(t0, x0 + 32, a.y + WF_LISTA_Y + 36);
    }
    for (int i = 0; i < n && i < cabe; ++i) {
      mesh_wifi_rede_t r;
      if (!mesh_wifi_no_rede(i, &r)) { break; }
      const int y = a.y + WF_LISTA_Y + i * RD_LIN;
      const bool no_no = s_cfg.wifi_ssid[0] && strcmp(r.ssid, s_cfg.wifi_ssid) == 0;
      const bool escolhida = s_mud.wifi_ssid[0] && strcmp(r.ssid, s_mud.wifi_ssid) == 0;
      char t[60];
      snprintf(t, sizeof(t), "%s", r.ssid);
      tab5_text(d, 3.6f);
      d.setTextColor(escolhida ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
      const int max_l = larg - 440;
      tab5_cortar_on_caber(d, t, max_l);
      d.drawString(t, x0 + 32, y + RD_LIN / 2);
      tab5_text(d, 2.8f);
      d.setTextColor(escolhida ? TFT_YELLOW : no_no ? TFT_GREENYELLOW : TFT_DARKGREY, TFT_BLACK);
      d.drawString(escolhida ? "• chosen" : no_no ? "● on the node now" : (r.open ? "open" : "password"), x0 + larg - 380, y + RD_LIN / 2);
      const int cx = x0 + larg - 70, cy = y + RD_LIN / 2;   // "x" para esquecer
      d.drawLine(cx - 12, cy - 12, cx + 12, cy + 12, TFT_RED);
      d.drawLine(cx - 12, cy + 12, cx + 12, cy - 12, TFT_RED);
      d.drawFastHLine(x0 + 16, y + RD_LIN - 2, larg - 32, d.color565(38, 42, 50));
      memset(&r, 0, sizeof(r));
    }
  } else if (s_cfg.wifi_conhecido && !s_cfg.wifi_enabled) {
    tab5_text(d, 2.8f);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    char t0[80];
    snprintf(t0, sizeof(t0), "turn on Wi-Fi and save: the networks show up here after it restarts");
    cortar_on_caber(d, t0, larg - 64);
    d.drawString(t0, x0 + 32, a.y + WF_LISTA_Y + 10);
  }

  // Mensagem e botoes.
  d.setTextDatum(middle_left);
  tab5_text(d, 2.4f);
  const int ym = a.y + a.height - (botoes ? RD_BTN_A + 14 + 14 : 0) - 24;
  if (s_cfg.state == MESH_CFG_GRAVANDO) { d.setTextColor(TFT_YELLOW, TFT_BLACK); d.drawString("saving - the node is restarting...", x0 + 24, ym); }
  else if (s_redes_msg[0]) {
    char m[sizeof(s_redes_msg)];
    snprintf(m, sizeof(m), "%s", s_redes_msg);
    cortar_on_caber(d, m, larg - 48);
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.drawString(m, x0 + 24, ym);
  }
  const int by = a.y + a.height - RD_BTN_A - 14;
  const int bl = (larg - 24 * 3) / 2;
  const bool pronto = s_cfg.state == MESH_CFG_PRONTO;
  if (botoes == 1) {
    tab5_botao(d, x0 + 24, by, bl, RD_BTN_A, "SAVE TO THE NODE", pronto ? TFT_GREENYELLOW : d.color565(50, 55, 65), pronto ? TFT_BLACK : TFT_DARKGREY);
    tab5_botao(d, x0 + 48 + bl, by, bl, RD_BTN_A, "DISCARD", d.color565(50, 55, 65), TFT_WHITE);
  } else if (botoes == 2) {
    tab5_botao(d, x0 + 24, by, bl, RD_BTN_A, "NEW NETWORK", TFT_GREENYELLOW, TFT_BLACK);
    tab5_botao(d, x0 + 48 + bl, by, bl, RD_BTN_A, "PULL FROM TAB5", d.color565(50, 55, 65), TFT_WHITE);
  }
  d.setTextDatum(top_left);
}

static void draw_config(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  mesh_cfg_read(&s_cfg);
  const int larg = a.x + a.width - x0;

  if (s_redes_pg) { draw_redes(d, a, x0, larg); return; }

  if (s_diario) { draw_diario(d, a, x0, larg); return; }

  // EDITANDO UM NOME: field e keyboard, nada mais in pagina.
  if (s_editando) {
    list_hide(&s_l_cfg);
    virtual_keyboard_position(&s_kb_cfg, a);
    static char s_ed_ant[sizeof(s_edit_txt) + 2];
    char ed[sizeof(s_edit_txt) + 2];
    snprintf(ed, sizeof(ed), "%s_", s_edit_txt);
    if (TUDO) {
      tab5_screen_clear(x0, a.y, larg, a.height - virtual_keyboard_height(a), TFT_BLACK);
      tab5_text(d, 2.8f);
      d.setTextDatum(top_left);
      d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      const char* tit = s_editando == RC_LONGO ? "node's long name (up to 39 letters)"
                      : s_editando == RC_CURTO ? "node's short name (up to 4 letters)"
                      : s_editando == ED_WIFI_SSID ? "node's Wi-Fi network (up to 32 letters)"
                      : "node's Wi-Fi password (8 to 64 letters)";
      char tit2[96];
      if (s_editando == ED_WIFI_SENHA && s_mud.wifi_ssid[0]) { snprintf(tit2, sizeof(tit2), "password for \"%s\" (8 to 64 characters)", s_mud.wifi_ssid); }
      else { snprintf(tit2, sizeof(tit2), "%s", tit); }
      cortar_on_caber(d, tit2, larg - 48);
      d.drawString(tit2, x0 + 24, a.y + 24);
    }
    if (paint_changed_s(&s_p, s_ed_ant, sizeof(s_ed_ant), ed)) {
      if (!TUDO) { tab5_screen_clear(x0 + 24, a.y + 70, larg - 48, 60, TFT_BLACK); }
      tab5_text(d, 4.2f);
      d.setTextDatum(top_left);
      d.setTextColor(TFT_YELLOW, TFT_BLACK);
      d.drawString(ed, x0 + 24, a.y + 76);
    }
    virtual_keyboard_paint(&s_kb_cfg, d, TUDO);
    return;
  }

  // Cabecalho: o estado of conversa com o no.
  char est[96];
  uint16_t color_est = TFT_DARKGREY;
  if (s_cfg.state == MESH_CFG_GRAVANDO)    { snprintf(est, sizeof(est), "saving - the node is restarting..."); color_est = TFT_YELLOW; }
  else if (s_cfg.state == MESH_CFG_PRONTO) { snprintf(est, sizeof(est), "%s", s_cfg_msg[0] ? s_cfg_msg : "connected to the node"); color_est = TFT_GREENYELLOW; }
  else { snprintf(est, sizeof(est), "no node (cable or Bluetooth) - view only"); }
  static char s_est_ant[96];
  // Dentro of one grupo: VOLTAR e o name dele no lugar of titulo.
  const char* titulo = s_grupo < 0 ? "Configure the node" : GRUPOS[s_grupo].name;
  const int tx = s_grupo < 0 ? x0 + 24 : x0 + 250;
  tab5_text(d, 3.6f);
  const int ex_min = tx + (int)d.textWidth(titulo) + 32;
  const int ex = ex_min > x0 + 380 ? ex_min : x0 + 380;
  if (TUDO) {
    tab5_screen_clear(x0, a.y, larg, CFG_TOPO, TFT_BLACK);
    if (s_grupo >= 0) { tab5_botao(d, x0 + 24, a.y + 12, 200, 56, "BACK", d.color565(50, 55, 65), TFT_WHITE); }
    tab5_text(d, 3.6f);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(titulo, tx, a.y + CFG_TOPO / 2);
  }
  if (paint_changed_s(&s_p, s_est_ant, sizeof(s_est_ant), est)) {
    if (!TUDO) { tab5_screen_clear(ex, a.y, a.x + a.width - ex, CFG_TOPO, TFT_BLACK); }
    tab5_text(d, 2.8f);
    cortar_on_caber(d, est, a.x + a.width - 16 - ex);
    d.setTextDatum(middle_left);
    d.setTextColor(color_est, TFT_BLACK);
    d.drawString(est, ex, a.y + CFG_TOPO / 2);
  }
  d.setTextDatum(top_left);

  // Linhas: so as que changed of text (a pagina roda a 1 Hz).
  const int n_items = cfg_n_items();
  if (TUDO || s_l_cfg.n != n_items) {
    list_items_changed(&s_l_cfg, n_items);
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
  } else {
    for (int i = 0; i < n_items; ++i) {
      // Nunca desenhada: a rolagem a desenha when ela aparecer, com o
      // valor of momento. Nao ha o que comparar - e marca-la aqui arrastaria
      // as visiveis junto (ver s_cfg_pintada).
      if (!s_cfg_pintada[i]) { continue; }
      char rot[48], val[96] = "", sig[112];
      bool pend = false;
      line_item(i, rot, sizeof(rot), val, sizeof(val), &pend);
      snprintf(sig, sizeof(sig), "%d|%s", (int)pend, val);
      if (strcmp(sig, s_cfg_ant[i]) != 0) { list_item_changed(&s_l_cfg, i); }
    }
  }
  // A engrenagem acende e apaga com o estado: muda everything of uma vez.
  static int s_estado_ant = -1;
  if (s_estado_ant != s_cfg.state) {
    s_estado_ant = s_cfg.state;
    list_items_changed(&s_l_cfg, n_items);
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
  }
  list_draw(&s_l_cfg, d, window_cfg(a, x0), TFT_BLACK, TUDO);

  // Rodape: GRAVAR (com pending e no pronto) e DESCARTAR.
  const bool pend = ha_pending();
  const int estado_rod = (pend ? 1 : 0) | (s_cfg.state == MESH_CFG_PRONTO ? 2 : 0);
  static int s_rod_ant = -1;
  if (TUDO || estado_rod != s_rod_ant) {
    s_rod_ant = estado_rod;
    // Sem pending a faixa e of lista (ja desenhada acima): nada a limpar.
    if (pend) {
      const int by = a.y + a.height - CFG_RODAPE + 12;
      tab5_screen_clear(x0, a.y + a.height - CFG_RODAPE, larg, CFG_RODAPE, TFT_BLACK);
      const bool pode = (s_cfg.state == MESH_CFG_PRONTO);
      const int bl = cfg_btn_l(a);
      tab5_botao(d, x0 + 24, by, bl, CFG_BTN_A, "SAVE TO THE NODE",
                 pode ? TFT_GREENYELLOW : d.color565(50, 55, 65), pode ? TFT_BLACK : TFT_DARKGREY);
      tab5_botao(d, x0 + 24 + bl + 24, by, bl, CFG_BTN_A, "DESCARTAR", d.color565(50, 55, 65), TFT_WHITE);
      // A dica so onde sobra lugar on lado dos botoes (em pe, nao sobra).
      tab5_text(d, 2.4f);
      const int dx = x0 + 24 + 2 * (bl + 24);
      if (dx + (int)d.textWidth("the node saves and restarts") < a.x + a.width - 16) {
        d.setTextDatum(middle_left);
        d.setTextColor(TFT_DARKGREY, TFT_BLACK);
        d.drawString("the node saves and restarts", dx, by + CFG_BTN_A / 2);
        d.setTextDatum(top_left);
      }
    }
  }
}

static void cfg_aviso(const char* msg)
{
  snprintf(s_cfg_msg, sizeof(s_cfg_msg), "%s", msg);
  system_request_redraw();
}

static void cfg_close_edicao(bool confirm)
{
  if (confirm && s_editando == ED_WIFI_SENHA && strlen(s_edit_txt) < 8) {
    cfg_aviso("a senha do Wi-Fi precisa de 8 letras ou mais");   // o firmware recusaria
    snprintf(s_redes_msg, sizeof(s_redes_msg), "a senha do Wi-Fi precisa de 8 letras ou mais");
    confirm = false;
  }
  if (confirm && s_edit_txt[0]) {
    char* dst; size_t size;
    switch (s_editando) {
      case RC_LONGO:     dst = s_mud.longo;     size = sizeof(s_mud.longo);     break;
      case ED_WIFI_SSID: dst = s_mud.wifi_ssid; size = sizeof(s_mud.wifi_ssid); break;
      case ED_WIFI_SENHA: dst = s_mud.wifi_psk; size = sizeof(s_mud.wifi_psk);  break;
      default:           dst = s_mud.curto;     size = sizeof(s_mud.curto);     break;
    }
    snprintf(dst, size, "%s", s_edit_txt);
  }
  const bool era_wifi = s_editando == ED_WIFI_SSID || s_editando == ED_WIFI_SENHA;
  const bool encadear = confirm && s_editando == ED_WIFI_SSID && s_edit_txt[0] && !s_mud.wifi_psk[0];
  if (confirm && s_editando == ED_WIFI_SENHA && s_edit_txt[0]) {
    s_mud.wifi_open = false;
    s_mud.wifi_muda = true; s_mud.wifi_liga = true;   // rede nova digitada: pronta para gravar
    snprintf(s_redes_msg, sizeof(s_redes_msg), "new network ready - tap SAVE TO THE NODE (the node restarts)");
  }
  memset(s_edit_txt, 0, sizeof(s_edit_txt));   // pode ter sido a senha of Wi-Fi
  // Rede nova digitada: em seguida pede a senha, sem exit of screen of Wi-Fi.
  s_editando = encadear ? ED_WIFI_SENHA : 0;
  if (!encadear) { virtual_keyboard_close(&s_kb_cfg); }
  if (era_wifi && !encadear) { s_redes_pg = true; }   // iteration a screen of Wi-Fi (confirmou ou cancelou)
  paint_request(&s_p);
  system_request_redraw_complete();
}

static void cfg_key(int ac)
{
  if (ac == TECVIRT_ENVIAR) { cfg_close_edicao(true); return; }
  if (ac == TECVIRT_FECHAR) { cfg_close_edicao(false); return; }
  const size_t max = s_editando == RC_LONGO ? MESH_NOME_LONGO - 1
                   : s_editando == ED_WIFI_SSID ? 32
                   : s_editando == ED_WIFI_SENHA ? 64 : MESH_NOME_CURTO - 2;   // curto: 4
  // Nome e senha of no sao ASCII (ver ao_teclar). 'max' caracteres + o zero.
  virtual_keyboard_edit(s_edit_txt, max + 1 < sizeof(s_edit_txt) ? max + 1 : sizeof(s_edit_txt), ac, true);
  system_request_redraw();
}

// SAVE TO THE NODE: validates the pending Wi-Fi, sends the transaction, and saves the network.
static void cfg_gravar(void)
{
  if (s_cfg.state != MESH_CFG_PRONTO) { return; }
  // Switch toggled without a new network: resend the current network WITH
  // the password we saved, so the node's password doesn't get lost when
  // this section is re-saved.
  if (s_mud.wifi_muda && !s_mud.wifi_ssid[0] && s_cfg.wifi_ssid[0]) {
    for (int k = 0; k < mesh_wifi_no_n(); ++k) {
      mesh_wifi_rede_t r;
      if (mesh_wifi_no_rede(k, &r) && strcmp(r.ssid, s_cfg.wifi_ssid) == 0) {
        snprintf(s_mud.wifi_ssid, sizeof(s_mud.wifi_ssid), "%s", r.ssid);
        snprintf(s_mud.wifi_psk, sizeof(s_mud.wifi_psk), "%s", r.senha);
        s_mud.wifi_open = r.open;
        memset(&r, 0, sizeof(r));
        break;
      }
      memset(&r, 0, sizeof(r));
    }
  }
  if (s_mud.wifi_psk[0] && !s_mud.wifi_ssid[0]) { cfg_aviso("Wi-Fi: enter both the network AND the password"); return; }
  if (s_mud.wifi_ssid[0] && !s_mud.wifi_psk[0] && !s_mud.wifi_open) { cfg_aviso("Wi-Fi: enter both the network AND the password"); return; }
  char nova_rede[33], nova_senha[65];
  snprintf(nova_rede, sizeof(nova_rede), "%s", s_mud.wifi_ssid);
  snprintf(nova_senha, sizeof(nova_senha), "%s", s_mud.wifi_psk);
  if (mesh_cfg_gravar(&s_mud)) {
    // Saved to pick from next time (card, password encrypted).
    if (nova_rede[0]) { mesh_wifi_no_salvar(nova_rede, nova_senha); }
    memset(&s_mud, 0, sizeof(s_mud)); cfg_aviso(""); s_redes_msg[0] = 0;
  }
  else {
    const char* reason = mesh_cfg_reason()[0] ? mesh_cfg_reason() : "could not save - check the log";
    cfg_aviso(reason);
    snprintf(s_redes_msg, sizeof(s_redes_msg), "%s", reason);
  }
  memset(nova_senha, 0, sizeof(nova_senha));
  paint_request(&s_p);
  system_request_redraw_complete();
}

static void touch_config(int x, int y)
{
  const area_t a = system_area();
  if (s_redes_pg) {
    const int bx = TRILHO_L + 24;
    const int larg = a.width - TRILHO_L;
    const int by = a.height - RD_BTN_A - 14;
    const int bl = (larg - 24 * 3) / 2;
    const int botoes = wf_botoes();
    const bool bt_y = y >= by && y < by + RD_BTN_A;
    const bool bt_1 = bt_y && x >= bx && x < bx + bl;
    const bool bt_2 = bt_y && x >= bx + bl + 24 && x < bx + 2 * bl + 24;
    if (y >= 12 && y < 68 && x >= bx && x < bx + 200) {                  // BACK
      s_redes_pg = false;
      list_items_changed(&s_l_cfg, cfg_n_items());
      memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
      paint_request(&s_p);
      system_request_redraw_complete();
    } else if (y >= WF_TOG_Y && y < WF_TOG_Y + WF_TOG_H) {               // switch
      if (s_cfg.state != MESH_CFG_PRONTO || !s_cfg.wifi_conhecido) { return; }
      const bool novo = !wf_liga_visivel();
      s_mud.wifi_liga = novo;
      s_mud.wifi_muda = (novo != s_cfg.wifi_enabled);
      if (!novo || !s_mud.wifi_muda) { memset(s_mud.wifi_ssid, 0, sizeof(s_mud.wifi_ssid)); memset(s_mud.wifi_psk, 0, sizeof(s_mud.wifi_psk)); s_mud.wifi_open = false; }
      s_redes_msg[0] = 0;
      system_request_redraw();
    } else if (botoes == 1 && bt_1) {                                    // SAVE TO THE NODE
      cfg_gravar();
    } else if (botoes == 1 && bt_2) {                                    // DISCARD
      memset(&s_mud, 0, sizeof(s_mud));
      s_redes_msg[0] = 0;
      paint_request(&s_p);
      system_request_redraw_complete();
    } else if (botoes == 2 && bt_1) {                                    // NEW NETWORK
      s_redes_pg = false;
      memset(s_mud.wifi_psk, 0, sizeof(s_mud.wifi_psk));
      s_mud.wifi_open = false;
      s_editando = ED_WIFI_SSID;
      s_edit_txt[0] = 0;
      virtual_keyboard_open(&s_kb_cfg);
      paint_request(&s_p);
      system_request_redraw_complete();
    } else if (botoes == 2 && bt_2) {                                    // PULL FROM TAB5
      const int n = mesh_wifi_no_trazer_of_tab5();
      if (n < 0)       { snprintf(s_redes_msg, sizeof(s_redes_msg), "the Tab5 has no loaded networks (turn on the Tab5's Wi-Fi once)"); }
      else if (n == 0) { snprintf(s_redes_msg, sizeof(s_redes_msg), "nothing new: all the Tab5's networks are already here"); }
      else             { snprintf(s_redes_msg, sizeof(s_redes_msg), "%d network(s) pulled from the Tab5", n); }
      system_request_redraw();
    } else if (wf_lista_visivel() && y >= WF_LISTA_Y && y < a.height - RD_BTN_A - 14 - 14 - 40) {
      const int i = (y - WF_LISTA_Y) / RD_LIN;
      mesh_wifi_rede_t r;
      if (mesh_wifi_no_rede(i, &r)) {
        if (x >= TRILHO_L + larg - 130) {                  // o "x": esquecer
          mesh_wifi_no_esquecer(i);
          snprintf(s_redes_msg, sizeof(s_redes_msg), "\"%s\" esquecida", r.ssid);
        } else if (s_cfg.state == MESH_CFG_PRONTO) {      // escolher: stays pending, GRAVAR aparece
          snprintf(s_mud.wifi_ssid, sizeof(s_mud.wifi_ssid), "%s", r.ssid);
          snprintf(s_mud.wifi_psk, sizeof(s_mud.wifi_psk), "%s", r.senha);
          s_mud.wifi_open = r.open;
          s_mud.wifi_muda = true;
          s_mud.wifi_liga = true;
          snprintf(s_redes_msg, sizeof(s_redes_msg), "\"%s\" chosen - tap SAVE TO THE NODE (the node restarts)", r.ssid);
        }
        memset(&r, 0, sizeof(r));
        system_request_redraw_complete();
      }
    }
    return;
  }
  if (s_diario) {
    const int bx = TRILHO_L + 24;
    const int ly = a.height - REI_RODAPE + 14;
    if (y >= 12 && y < 68 && x >= bx && x < bx + 200) {
      s_diario = false;
      s_limpar_ms = 0;
      list_items_changed(&s_l_cfg, cfg_n_items());
      memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
      paint_request(&s_p);
      system_request_redraw_complete();
    } else if (y >= ly && y < ly + 56 && x >= bx && x < bx + 320) {
      if (s_limpar_ms && (tab5_now_ms() - s_limpar_ms) < 4000) { s_limpar_ms = 0; mesh_diario_limpar(); }
      else { s_limpar_ms = tab5_now_ms() | 1u; }
      system_request_redraw();
    }
    return;
  }
  if (s_editando) {
    if (virtual_keyboard_contains(&s_kb_cfg, a.x + x, a.y + y)) {
      const int ac = virtual_keyboard_click(&s_kb_cfg, a.x + x, a.y + y);
      if (ac) { cfg_key(ac); }
    }
    return;
  }

  // VOLTAR of dentro of one grupo: sobe para a lista dos grupos.
  if (s_grupo >= 0 && y >= 12 && y < 68 && x >= TRILHO_L + 24 && x < TRILHO_L + 224) {
    s_grupo = -1;
    s_reiniciar_ms = 0;
    list_on_start(&s_l_cfg);
    list_items_changed(&s_l_cfg, cfg_n_items());
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
    paint_request(&s_p);
    system_request_redraw_complete();
    return;
  }

  // Rodape.
  const int by = a.height - CFG_RODAPE + 12;
  if (y >= by && y < by + CFG_BTN_A && ha_pending()) {
    const int bx = TRILHO_L + 24;
    const int bl = cfg_btn_l(a);
    if (x >= bx && x < bx + bl) {
      cfg_gravar();
    } else if (x >= bx + bl + 24 && x < bx + 2 * bl + 24) {
      memset(&s_mud, 0, sizeof(s_mud));
      paint_request(&s_p);
      system_request_redraw_complete();
    }
    return;
  }

  const int item = list_item_touched(&s_l_cfg, x, y, nullptr);
  if (item < 0) { return; }
  // Na lista dos grupos: abre o grupo touched.
  if (s_grupo < 0) {
    if (item >= G_TOTAL) { return; }
    s_grupo = item;
    s_cfg_msg[0] = 0;
    list_on_start(&s_l_cfg);
    list_items_changed(&s_l_cfg, cfg_n_items());
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
    paint_request(&s_p);
    system_request_redraw_complete();
    return;
  }
  if (item >= GRUPOS[s_grupo].n) { return; }
  const int i = GRUPOS[s_grupo].rc[item];
  if (i == RC_I_REINICIOS) {
    s_diario = true;
    s_limpar_ms = 0;
    paint_request(&s_p);
    system_request_redraw_complete();
    return;
  }
  if (i < 0 || i >= RC_EDITAVEIS) { return; }
  if (i == RC_SOM) { tab5_mesh_set_beep(!tab5_mesh_beep_enabled()); system_request_redraw(); return; }
  if (i == RC_LOC_TAB5) {
    settings_set_mesh_position_interval_min((uint8_t)opc_next(OPC_TAB5, N_OPC(OPC_TAB5), settings_mesh_position_interval_min()));
    system_request_redraw();
    return;
  }
  if (s_cfg.state != MESH_CFG_PRONTO) { return; }
  if (i == RC_LOC_PRECISAO) {
    if (s_cfg.precisao < 0) { cfg_aviso("the node's primary channel has not arrived yet"); return; }
    const int32_t current = s_mud.precisao_muda ? s_mud.precisao : s_cfg.precisao;
    s_mud.precisao = opc_next(OPC_PRECISAO, N_OPC(OPC_PRECISAO), current);
    s_mud.precisao_muda = (s_mud.precisao != s_cfg.precisao);
    system_request_redraw();
    return;
  }

  const int c = field_of_line(i);
  if (c >= 0) {
    if (!s_cfg.conhecido[c]) { return; }
    int32_t novo = 0;
    const int32_t v = valor_current(c);
    switch (i) {
      case RC_PAPEL:      novo = opc_next(OPC_PAPEL, N_OPC(OPC_PAPEL), v); break;
      case RC_SALTOS:     novo = opc_next(OPC_SALTOS, N_OPC(OPC_SALTOS), v == 0 ? 3 : v); break;
      case RC_TX:         novo = opc_next(OPC_TX, N_OPC(OPC_TX), v); break;
      case RC_POS_S:      novo = opc_next(OPC_POS_S, N_OPC(OPC_POS_S), v); break;
      case RC_NODEINFO_S: novo = opc_next(OPC_NODEINFO_S, N_OPC(OPC_NODEINFO_S), v); break;
      case RC_TELE_S:     novo = opc_next(OPC_TELE_S, N_OPC(OPC_TELE_S), v); break;
      case RC_MQTT:
        novo = v ? 0 : 1;
        // O espelho of regra of proxy: sem o MQTT o proxy nao tem o que
        // levar - disable o MQTT leva o proxy junto. Religar devolve o proxy
        // on que o no tinha.
        if (!novo && valor_current(MESH_CFG_MQTT_PROXY) && s_cfg.conhecido[MESH_CFG_MQTT_PROXY]) {
          s_mud.v[MESH_CFG_MQTT_PROXY] = 0;
          s_mud.muda[MESH_CFG_MQTT_PROXY] = (s_cfg.v[MESH_CFG_MQTT_PROXY] != 0);
        }
        if (novo && s_mud.muda[MESH_CFG_MQTT_PROXY] && s_cfg.v[MESH_CFG_MQTT_PROXY]) { s_mud.muda[MESH_CFG_MQTT_PROXY] = false; }
        break;
      case RC_POS_INTELIGENTE: novo = v ? 0 : 1; break;
      case RC_GPS_NO:
        // 2 = NOT_PRESENT: o firmware decide isso sozinho (a placa nao tem
        // GPS); enable daqui nao faria aparecer one.
        if (v == 2) { cfg_aviso("the node's board has no GPS"); return; }
        novo = v == 1 ? 0 : 1;
        break;
      case RC_PROXY:
        novo = v ? 0 : 1;
        // O proxy so age com o MQTT of no enabled: enable o proxy leva o MQTT
        // junto, in MESMA transacao (as duas lines ficam pendentes).
        // Desligar o proxy nao mexe no MQTT.
        if (novo && !valor_current(MESH_CFG_MQTT) && s_cfg.conhecido[MESH_CFG_MQTT]) {
          s_mud.v[MESH_CFG_MQTT] = 1;
          s_mud.muda[MESH_CFG_MQTT] = (s_cfg.v[MESH_CFG_MQTT] != 1);
        }
        // Desistiu of enable (voltou a disabled): o MQTT que tinha vindo junto
        // iteration tambem, se o no estava com ele disabled.
        if (!novo && s_mud.muda[MESH_CFG_MQTT] && !s_cfg.v[MESH_CFG_MQTT]) { s_mud.muda[MESH_CFG_MQTT] = false; }
        break;
      case RC_BT:
        // Desligar o Bluetooth of no com o Tab5 falando por ele cortaria a
        // propria conversa - e sem ela nao ha como religar daqui.
        if (v && tab5_mesh_via() == SETTINGS_MESH_TRANSPORT_BLUETOOTH) { cfg_aviso("the Tab5 is talking to the node over Bluetooth"); return; }
        novo = v ? 0 : 1;
        break;
    }
    // Voltou on valor que o no ja tem: deixa of ser pending.
    s_mud.v[c] = novo;
    s_mud.muda[c] = (novo != s_cfg.v[c]);
    system_request_redraw();
    return;
  }

  switch (i) {
    case RC_WIFI:
      s_redes_pg = true;
      s_redes_msg[0] = 0;
      paint_request(&s_p);
      system_request_redraw_complete();
      return;
    case RC_LONGO: case RC_CURTO:
      s_editando = i;
      s_edit_txt[0] = 0;
      virtual_keyboard_open(&s_kb_cfg);
      paint_request(&s_p);
      system_request_redraw_complete();
      return;
    case RC_POS_FIXA:
      if (s_cfg.pos_fixa) { cfg_aviso(mesh_cfg_tirar_position_fixa() ? "fixed position removed" : "could not remove it"); }
      else                { cfg_aviso(mesh_cfg_position_fixa_of_gps() ? "GPS position sent to the node" : "Tab5's GPS has no signal"); }
      return;
    case RC_HORA:
      cfg_aviso(mesh_cfg_acertar_hora() ? "node's clock set" : "the Tab5 does not know the time yet");
      return;
    case RC_REINICIAR:
      if (s_reiniciar_ms && (tab5_now_ms() - s_reiniciar_ms) < 4000) {
        s_reiniciar_ms = 0;
        cfg_aviso(mesh_cfg_reiniciar() ? "" : "could not restart");
      } else {
        s_reiniciar_ms = tab5_now_ms();
        system_request_redraw();
      }
      return;
  }
}

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  // O keyboard fisico tambem edita estes campos. Se ele foi encaixado com o
  // virtual open, libera a metade of bottom e a navigation sem abandonar a
  // edicao nem erase o text ja digitado.
  if (tab5_keyboard_present() && s_editando && virtual_keyboard_is_open(&s_kb_cfg)) {
    virtual_keyboard_close(&s_kb_cfg);
  }

  // O pintura_iniciar() ja consulta sozinho se o sistema limpou a area - era o
  // passo que, esquecido, deixava a screen em branco on wake.
  //
  // Repintar NAO implica reler o cartao: a copia em RAM continua valida.
  // Confundir as duas coisas fazia o app montar o SD a cada touch.
  // A ORDEM DA COLUNA MUDOU: reencontra a conversa open pela chave. Se ela
  // exited of coluna (canal desabilitado no no, servico trocando of via), a screen
  // iteration a "escolha uma" mas a chave stays: when ela reaparecer, reabre.
  // ANTES of pintura_iniciar, para esta pass ja exit complete - a coluna
  // inteira changed of lugar (ver a regra of repintura complete).
  const uint32_t cv = tab5_mesh_conversas_versao();
  if (cv != s_conv_ver) {
    s_conv_ver = cv;
    if (s_ab.ok) {
      const int novo = tab5_mesh_conversa_indice(s_ab.channel, s_ab.idx, s_ab.no);
      if (novo != s_conv) {
        if ((novo < 0) != (s_conv < 0)) { conversation_switch(&s_c); s_hist_pronto = false; }
        s_conv = novo;
      }
    }
    paint_request(&s_p);
    system_request_redraw_complete();
  }

  paint_start(&s_p);

  // A primeira carga tambem le of cartao os nomes e as conversas - e tem que
  // acontecer mesmo sem conversa open, senao a coluna nasce vazia sem o no.
  if (s_carga_cartao && !s_carga_requested) { carregar_hist(); }

  // Mensagem nova no servico: ai sim vale reler.
  //
  // tab5_mesh_total_msgs(), NAO tab5_mesh_num_msgs() - sintoma: message
  // nova so aparecia depois of switch of screen e voltar. O
  // segundo satura em MESH_MAX_MSGS (8) e stays parado para sempre depois
  // of oitava message since o boot of servico - a comparacao abaixo nunca
  // mais via diferenca, entao o app so recarregava por outros caminhos
  // (switch of conversa, switch of pagina) que zeram s_hist_pronto direto,
  // nunca sozinho enquanto a screen ficava open. Ver o aviso grande no
  // .hpp junto of tab5_mesh_total_msgs().
  const int64_t nm = (int64_t)tab5_mesh_total_msgs();
  // COMPARACAO DIRETA, e nao pintura_mudou_i(). O helper devolve true em toda
  // pass COMPLETA, por drawing - e complete e o que faz o app repintar everything.
  // Usa-lo para decidir E/S cria loop: pass complete -> "changed" -> recarrega
  // -> pintura_pedir() -> next pass complete -> ... O keyboard repintava a
  // cada ciclo e piscava sozinho.
  //
  // Regra: pintura_mudou_* serve para decidir DESENHO. Decisao of read cartao,
  // open arquivo ou falar com hardware compara in mao.
  if (nm != s_msgs_when_li) {
    // So PEDE a carga nova (o worker le o cartao). Quando ela chega, o
    // ao_aviso troca os buffers e pede a repintura - que aqui seria cedo
    // demais (o historico novo ainda nao existe) e duplicada.
    //
    // A position of leitura e of lista (kernel/list): colado no end, a
    // message nova aparece; lendo o historico, a screen nao se mexe.
    s_hist_pronto = false;
    s_msgs_when_li = nm;
    system_request_redraw();
  }


  // ACK chegou (ou prazo estourou): sao os MESMOS baloes, so a marca changed.
  // Nao releia o cartao por causa disso - o content nao changed, e montar o SD a
  // cada confirmacao seria caro a toa. Comparacao direta pelo mesmo reason of
  // bloco acima.
  const uint32_t ev = tab5_mesh_envios_versao();
  if (ev != s_envios_ver) { s_envios_ver = ev; conversation_changed(&s_c); }   // as marcas of send_state

  // EM PE: ou a lista (trilho + coluna larga), ou a conversa por top of
  // everything. Deitado: trilho, coluna e conversa lado a lado, como sempre.
  const bool pe = em_pe(a);
  const bool frente = pe && s_pag == PAG_MSGS && s_conv_frente && s_conv >= 0;

  if (TUDO && !frente) { draw_trilho(d, a); }
  const int x0 = frente ? a.x
               : a.x + TRILHO_L + ((s_pag == PAG_MSGS && !pe) ? CONV_L : 0);

  switch (s_pag) {
    case PAG_NOS:   draw_nos(d, a, x0);   break;
    case PAG_MALHA: draw_malha(d, a, x0); break;
    case PAG_NO:    draw_config(d, a, x0); break;
    default:
      if (pe && !frente) {
        // Em pe, so a lista: os baloes nao estao in screen, entao nao rolam
        // nem respondem, e o keyboard (se estava open) fecha.
        conversation_close_keyboard(&s_c);
        list_hide(&s_c.lista);
      } else {
        draw_msgs(d, a, x0);
      }
      break;
  }
  // A coluna DEPOIS das mensagens: desenhar_msgs marca como lida a conversa
  // open, e o contador dela in coluna ja exit zerado nesta mesma pass.
  // Com a conversa in frente a coluna esta coberta: nao rola nem responde.
  if (s_pag == PAG_MSGS && !frente) { draw_conversas(d, a); }
  else                              { list_hide(&s_l_conv); }
}

// A TELA GIROU (kernel/orientation). Deitado -> em pe com uma conversa
// open: ela vem para a frente, e quem estava escrevendo segue escrevendo.
// Em pe -> deitado: lista e conversa voltam a ficar lado a lado. O resto se
// acomoda sozinho in pass complete que o sistema ja pediu.
static void on_rotate(void)
{
  s_conv_frente = (s_conv >= 0);
  paint_request(&s_p);
}

// Em pe: a conversa exit of frente e a lista iteration.
static void voltar_a_lista(void)
{
  s_conv_frente = false;
  conversation_close_keyboard(&s_c);
  paint_request(&s_p);
  system_request_redraw_complete();
}

// Teclado fisico of ExtPort1: o componente of conversa trata.
static void on_type(const char* text, uint8_t code, uint8_t mod)
{
  // Teclado fisico tambem edita o name of no.
  if (s_pag == PAG_NO && s_editando) {
    if      (code == TAB5_KEY_ENTER)  { cfg_close_edicao(true); }
    else if (code == TAB5_KEY_ESC)    { cfg_close_edicao(false); }
    else if (code == TAB5_KEY_BACKSPACE) { cfg_key(TECVIRT_APAGAR); }
    else if (text && text[0]) {
      // Nome e senha of no sao ASCII: a letra composta pela key morta
      // iteration a ser as duas teclas apertadas.
      char cru[8];
      tab5_keyboard_ascii(text, cru, sizeof(cru));
      for (const char* c = cru; *c && s_editando; ++c) { cfg_key((unsigned char)*c); }
    }
    return;
  }
  if (s_pag != PAG_MSGS || s_conv < 0) { return; }
  conversation_type(&s_c, text, code, mod);
}

// TROCA DE PAGINA num lugar so. As listas of pagina que exit param of
// responder on touch (lista_esconder) - voltam when forem desenhadas.
static void ir_para_pagina(pagina_t p)
{
  s_pag = p;
  s_no_sel = 0;   // touch no trilho iteration sempre para a tabela
  conversation_close_keyboard(&s_c);   // devolve a navigation, se estava travada
  list_hide(&s_c.lista);
  list_hide(&s_l_conv);
  list_hide(&s_l_nos);
  list_hide(&s_l_cfg);
  if (s_editando) { s_editando = 0; virtual_keyboard_close(&s_kb_cfg); }
  s_diario = false;
  s_redes_pg = false;
  s_grupo = -1;
  s_cfg_msg[0] = 0;
  paint_request(&s_p);
  system_request_redraw_complete();
}

// ABRIR UMA CONVERSA (c = -1: none, "escolha uma") - of coluna, of one no ou
// of one canal disponivel. Conversa abre in message mais recente.
static void open_conversa(int c)
{
  s_conv = c;
  s_conv_frente = (c >= 0);   // em pe, a escolhida vem para a frente (deitado, nada muda)
  s_ab = chave_of(c);
  s_conv_ver = tab5_mesh_conversas_versao();
  s_hist_pronto = false;
  atualizar_eh_canal();
  conversation_switch(&s_c);   // fecha o keyboard, vai on end, re-mede
  if (c >= 0) { tab5_mesh_marcar_conversa_lida(c); }
  if (s_pag == PAG_MSGS && c >= 0) {
    // Trocando of conversa pela coluna: o keyboard (e o rascunho) ficam.
    paint_request(&s_p);
    system_request_redraw_complete();
    return;
  }
  ir_para_pagina(PAG_MSGS);
}

static void on_touch_level(int x, int y, bool pressed)
{
  // Tecla sob o finger, soltura e rolagem dos baloes: o componente of conversa.
  if (s_pag == PAG_MSGS && s_conv >= 0) { conversation_touch_level(&s_c, x, y, pressed); }

  // As outras duas listas rolam cada uma in sua window; a que nao esta in screen
  // (lista_esconder) ignora o touch.
  // Teclado of name of no: as teclas confirmadas nesta sample.
  if (s_pag == PAG_NO && s_editando) {
    int actions[8];
    const int n = virtual_keyboard_level(&s_kb_cfg, actions, 8);
    for (int k = 0; k < n && s_editando; ++k) { cfg_key(actions[k]); }
    return;
  }

  bool rolou = false;
  rolou |= list_drag(&s_l_conv, x, y, pressed);
  rolou |= list_drag(&s_l_nos,  x, y, pressed);
  rolou |= list_drag(&s_l_cfg,  x, y, pressed);
  if (rolou) { system_request_redraw(); }
}

static void on_touch(int x, int y)
{
  const area_t a = system_area();

  // Teclado open: a faixa dele (width inteira) e so dele.
  if (conversation_keyboard_open(&s_c) && s_pag == PAG_MSGS && y >= a.height - virtual_keyboard_height(a)) {
    conversation_touch(&s_c, x, y);
    return;
  }

  // EM PE, CONVERSA NA FRENTE: nao ha trilho nem coluna embaixo of finger -
  // so a faixa of VOLTAR e a conversa.
  if (em_pe(a) && s_pag == PAG_MSGS && s_conv_frente && s_conv >= 0) {
    if (y < FRENTE_TOPO_A) {
      if (x >= 16 && x < 16 + FRENTE_VOLTAR_L
       && y >= FRENTE_VOLTAR_Y && y < FRENTE_VOLTAR_Y + FRENTE_VOLTAR_A) {
        voltar_a_lista();
      }
      return;
    }
    conversation_touch(&s_c, x, y);
    return;
  }

  if (x < TRILHO_L) {
    for (int i = 0; i < PAG_TOTAL; ++i) {
      const int iy = 16 + i * (ITEM_A + 12);
      if (y >= iy && y < iy + ITEM_A) {
        s_conv_frente = false;   // em pe, o trilho leva sempre a lista
        ir_para_pagina((pagina_t)i);
        return;
      }
    }
    return;
  }

  // PAGINA DE NOS: canal abre a conversa; no abre o cartao dele.
  if (s_pag == PAG_NOS) {
    // CARTAO DE UM NO open: ESTRELA, VOLTAR e CONVERSAR.
    if (s_no_sel) {
      const int fav_l = det_fav_l(a);
      const int fx = a.width - DET_FAV_DIR - fav_l;
      if (s_no_sel != tab5_mesh_meu_numero()
       && x >= fx && x < fx + fav_l && y >= DET_FAV_Y && y < DET_FAV_Y + DET_FAV_A) {
        tab5_mesh_set_favorito(s_no_sel, !tab5_mesh_favorito(s_no_sel));
        system_request_redraw();
        return;
      }
      const int by = a.height - DET_BTN_Y_DA_BASE;
      if (y >= by && y < by + DET_BTN_A) {
        const int bx = TRILHO_L + 24;
        const int bl = det_btn_l(a);
        if (x >= bx && x < bx + bl) {
          s_no_sel = 0;
          paint_request(&s_p);
          system_request_redraw_complete();
        } else if (x >= bx + bl + 24 && x < bx + 2 * bl + 24
                && s_no_sel != tab5_mesh_meu_numero() && !no_internet(s_no_sel)) {
          // -1 when o numero nao serve (zero, ou o proprio no).
          const int c = tab5_mesh_conversa_com(s_no_sel);
          s_no_sel = 0;
          if (c >= 0) { open_conversa(c); }
        }
      }
      return;
    }

    // A REDE: troca a tabela. E o liga/desliga of internet.
    for (int r = 0; r < 2; ++r) {
      const chip_t& k = s_chip_rede[r];
      if (x < k.x || x >= k.x + k.l || y < k.y || y >= k.y + k.a) { continue; }
      if (s_rede_vista != r) {
        s_rede_vista = (uint8_t)r;
        list_on_start(&s_l_nos);
        paint_request(&s_p);
        system_request_redraw_complete();
      }
      return;
    }
    {
      const chip_t& k = s_chip_internet_liga;
      if (k.l && x >= k.x && x < k.x + k.l && y >= k.y && y < k.y + k.a) {
        settings_set_mesh_public_network(!settings_mesh_public_network());   // o laco of servico sobe/derruba o cliente
        paint_request(&s_p);
        system_request_redraw_complete();
        return;
      }
    }

    // UM CANAL: abre a conversa dele (e a traz of iteration, se foi apagada).
    for (int i = 0; i < s_n_chips; ++i) {
      const chip_t& k = s_chips[i];
      if (x < k.x || x >= k.x + k.l || y < k.y || y >= k.y + k.a) { continue; }
      const int c = tab5_mesh_conversa_of_canal(k.idx);
      if (c >= 0) { open_conversa(c); }
      return;
    }

    // UM NO: o cartao dele. Dali se conversa - antes o touch abria a conversa
    // direto, e nao havia onde ver MAC, tensao ou position.
    const int i = list_item_touched(&s_l_nos, x, y, nullptr);
    if (i < 0) { return; }
    mesh_no_t no;
    if (!tab5_mesh_no_rede(s_rede_vista, i, &no)) { return; }
    s_no_sel = no.num;
    paint_request(&s_p);
    system_request_redraw_complete();
    return;
  }

  if (s_pag == PAG_NO) { touch_config(x, y); return; }
  if (s_pag == PAG_MALHA) { return; }

  if (s_pag != PAG_MSGS) { return; }

  // Coluna of conversas.
  if (x < TRILHO_L + s_col_l) {
    int dy = 0;
    const int i = list_item_touched(&s_l_conv, x, y, &dy);
    if (i < 0) { return; }
    if (i == s_conv_div) { dy -= CONV_DIV_A; }   // o traco of internet em top of item
    dy -= 8;                          // o item comeca pelo respiro (conv_desenhar)
    if (dy < 0 || dy >= ITEM_A) { return; }   // no respiro entre items: nada

    // O "x" vem ANTES of selecionar: ele esta dentro of item, e tratar a
    // selecao first faria o touch no botao virar touch no item.
    const int ax = TRILHO_L + 1 + erase_dx();
    if (i == s_conv
     && x >= ax && x < ax + APAGAR_L
     && dy >= APAGAR_DY && dy < APAGAR_DY + APAGAR_A
     && tab5_mesh_esquecer_conversa(i)) {
      // Nenhuma conversa seleciona no lugar: os indices acima deste andaram
      // uma casa, e herdar o indice abriria a conversa VIZINHA - uma que a
      // pessoa nao pediu. Melhor voltar para "escolha uma".
      open_conversa(-1);
      return;
    }
    open_conversa(i);
    return;
  }

  // Barra of escrita e botao send.
  // Coluna of conversa: barra of escrita, ENV e rolagem - o componente.
  if (s_conv >= 0) { conversation_touch(&s_c, x, y); }
}

static bool on_open(void)
{
  // O ARQUIVO PRIMEIRO. Sem o C6L plugado a lista of conversas nasce vazia -
  // ela so era povoada por message chegando - e o app abria dizendo "sem
  // conversas" com o cartao cheio delas.
  //
  // Vale com o radio no ar tambem: conversas antigas aparecem since a abertura,
  // e nao so when alguem iteration a falar.
  // Nomes, conversas e o historico vem of cartao PELO WORKER, junto of
  // primeira carga (ver trabalho_carregar) - a screen abre in hora.
  s_carga_cartao = true;
  s_carga_requested = false;
  s_carga_pronta = false;
  s_reload   = false;
  s_hist_chave   = chave_t{ };

  s_hist_pronto = false;
  conversation_open(&s_c, &s_p, conv_count, conv_read, conv_send, nullptr);
  s_c.limit = MESH_PAYLOAD_MAX;   // o teto of protocolo, nao o of buffer (ver tab5_mesh.hpp)
  s_c.color_send = lgfx::color565(103, 234, 148);   // o verde of marca (marca_mesh)
  list_set(&s_l_conv, ROLAGEM_INICIO, 0, conv_height, conv_draw, nullptr);
  s_l_conv.topo = 4;   // o 1o item ja traz 8px of respiro: 12 of topo, como sempre foi
  list_set(&s_l_nos, ROLAGEM_INICIO, 0, nos_height, nos_draw, nullptr);
  list_set(&s_l_cfg, ROLAGEM_INICIO, 0, cfg_height, cfg_draw, nullptr);
  paint_request(&s_p);
  system_lock_navigation(false);
  return true;
}

static void on_close(void)
{
  // Devolve o historico. Sao 38KB of PSRAM que nao fazem falta a ninguem com o
  // app closed - e o reason of ele nao ser mais estatico.
  // (O sistema ja esperou o worker antes of chamar isto.)
  if (s_hist)  { heap_caps_free(s_hist);  s_hist  = nullptr; }
  if (s_carga) { heap_caps_free(s_carga); s_carga = nullptr; }
  s_hist_chave = chave_t{ };
  if (s_editando) { s_editando = 0; virtual_keyboard_close(&s_kb_cfg); }
  s_diario = false;
  s_redes_pg = false;
  s_grupo = -1;
  conversation_close(&s_c);   // devolve o cache dos baloes e a navigation
  s_n_hist = 0;
  s_hist_pronto = false;
}

static uint32_t periodo_now(void)
{
  return (s_pag == PAG_NOS || s_pag == PAG_MALHA || s_pag == PAG_NO) ? 1000 : 0;
}

extern const app_t APP_MESH = {
  .name  = "Mesh",
  .color   = TFT_GREENYELLOW,
  // O Grove ja e mantido pelo ajuste of sistema. O IMU e para a screen follow
  // o aparelho.
  .profile = { .imu = true },
  .uses_sd = true,           // historico das conversas
  // Em pe ou deitado conforme o aparelho; com o keyboard encaixado, deitado -
  // a postura natural of quem escreve (ver o LAYOUT no topo of arquivo).
  .orientation = ORIENTATION_FOLLOWS_IMU,
  .piso_sleep_mhz = 0,
  .on_open  = on_open,
  .on_close = on_close,
  .draw  = draw,
  .on_touch  = on_touch,
  .on_touch_level = on_touch_level,   // acende a key sob o finger
  .on_type = on_type,             // keyboard fisico of ExtPort1
  .periodo_ms = 0,
  // Relogio SO nas paginas que mostram idade (Nos, Malha); nas mensagens o app
  // so acorda por aviso - message nova, send_state confirmado, no novo, carga of
  // historico pronta.
  .periodo_now = periodo_now,
  .notices = AVISO_MESH_MSG | AVISO_MESH_ENVIO | AVISO_MESH_NOS | AVISO_TRABALHO,
  .on_aviso = on_aviso,
  // O icon of screen inicial e a propria marca of Meshtastic: quem procura este
  // app procura o simbolo dele, nao a letra M.
  .icon = marca_mesh,
  .on_rotate = on_rotate,
};
