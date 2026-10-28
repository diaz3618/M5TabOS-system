// APP: Meshtastic - a JANELA do servico tab5_mesh.
//
// O app nao possui o enlace: quem fala com o C6L e o servico, que fica de pe
// com esta tela fechada. Aqui so se desenha o que ele ja sabe. E o mesmo
// desenho de Ajustes para o WiFi, e e o que permite avisar de mensagem nova sem
// o app aberto.
//
// SEGUE O APARELHO (ORI_SEGUE_IMU no descritor): deitado (1280x720) ou em pe
// (720x1280). Com o teclado encaixado, sempre deitado - a postura natural, e
// a lista de mensagens cabe muito mais.
//
// LAYOUT DEITADO
//   [trilho]  [conversas]   area principal
//     90px       200px      o resto
//
// O trilho e sempre visivel. A coluna de conversas SO aparece na pagina de
// mensagens - nas outras ela seria espaco morto. E a barra de escrita fica
// dentro da area das mensagens, nao sob as colunas.
//
// LAYOUT EM PE - nao cabem tres colunas em 720 px. Na pagina de mensagens a
// coluna de conversas ocupa o resto da largura (com mais espaco para o nome),
// e tocar numa conversa a abre POR CIMA de tudo, na largura inteira, com uma
// faixa de VOLTAR em cima. Voltar devolve a lista - o mesmo vai-e-volta de
// um app de mensagem de celular. Nas outras paginas o trilho fica, e o
// conteudo se acomoda: a tabela de nos esconde colunas, a malha vira uma
// coluna so, botoes lado a lado encolhem.

#include "list.hpp"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes na PSRAM
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
static const int ITEM_A   = 72;      // altura de um item nas colunas

// BOTAO DE APAGAR CONVERSA. Geometria num lugar so, porque desenho e toque
// precisam concordar - e ja foi por concordarem "quase" que as colunas dos nos
// sairam do lugar uma vez.
//
// Fica ABAIXO do nome, e nao ao lado: ao lado obrigaria a encurtar o nome de 8
// para 6 caracteres, e o item tem 72px de altura com o nome ocupando so a
// primeira linha. A altura estava sobrando; a largura, nao.
static const int APAGAR_L  = 30;
static const int APAGAR_A  = 28;
static const int APAGAR_DY = 40;            // a partir do y do item

// LARGURA DA COLUNA DE CONVERSAS: 200 deitado; em pe, tudo o que sobra do
// trilho. Gravada na passada que desenha a coluna e lida pelo item e pelo
// toque - os tres concordam porque leem o mesmo numero.
static int s_col_l = CONV_L;
// O "x" fica a 50 px do fim da coluna (fora da faixa da barra de rolagem),
// a partir do x da coluna.
static int apagar_dx(void) { return s_col_l - 50; }

// EM PE, A CONVERSA ABERTA POR CIMA DA COLUNA. false = a lista; true = a
// conversa na largura inteira, com VOLTAR. Deitado nao e consultado (as duas
// aparecem lado a lado).
static bool s_conv_frente = false;
static const int FRENTE_TOPO_A = 84;        // a faixa do VOLTAR e do nome
static const int FRENTE_VOLTAR_L = 200, FRENTE_VOLTAR_A = 56, FRENTE_VOLTAR_Y = 14;

static bool em_pe(const area_t& a) { return a.largura < a.altura; }

// Corta 't' no fim, por caractere (UTF-8), ate caber em max_l px na fonte
// que estiver ativa. Em pe quase todo texto longo passa por aqui.
static void cortar_ao_caber(lgfx::LovyanGFX& d, char* t, int max_l) { tab5_cortar_ao_caber(d, t, max_l); }

// Escolhe a fonte suave do tamanho 'tam' para baixo (passo 0,2) ate 't'
// caber em max_l; no menor tamanho, corta. Deixa a fonte escolhida ativa.
static void texto_que_cabe(lgfx::LovyanGFX& d, char* t, int max_l, float tam, float tam_min)
{
  tab5_texto(d, tam);
  while (tam > tam_min && (int)d.textWidth(t) > max_l) {
    tam -= 0.2f;
    tab5_texto(d, tam);
  }
  cortar_ao_caber(d, t, max_l);
}

// Ordem do trilho: CONVERSAS primeiro. E a pagina que se usa mais, e a que
// motiva abrir o app - as outras sao consulta.
// A ORDEM IMPORTA: ela e a mesma dos quatro primeiros glifos (GL_CONVERSAS,
// GL_NOS, GL_MALHA, GL_CONFIG), e o trilho converte a pagina em glifo por
// conversao direta. Trocar a ordem de um sem o outro poe o icone errado.
typedef enum { PAG_MSGS = 0, PAG_NOS, PAG_MALHA, PAG_NO, PAG_TOTAL } pagina_t;

static pagina_t s_pag = PAG_MSGS;

static int  s_conv = -1;              // conversa aberta (indice na coluna); -1 = nenhuma

// A CONVERSA ABERTA PELA CHAVE, e nao so pelo indice. O indice e a posicao na
// coluna, e a coluna se reordena (direta com mensagem nova sobe, conversa
// nova, canal desabilitado no no). Guardando so o indice, a tela passava a
// mostrar e a ENVIAR para a conversa vizinha. Quando a versao da ordem muda
// (tab5_mesh_conversas_versao), o indice e reencontrado por esta chave.
typedef struct { bool ok; bool canal; uint8_t idx; uint32_t no; } chave_t;
static chave_t  s_ab = { };            // a conversa aberta (ok = false: nenhuma)
static uint32_t s_conv_ver = 0;        // a versao da ordem que s_conv segue

static bool chave_igual(const chave_t& a, const chave_t& b)
{
  if (a.ok != b.ok) { return false; }
  if (!a.ok)        { return true; }
  return a.canal == b.canal && (a.canal ? a.idx == b.idx : a.no == b.no);
}

static chave_t chave_de(int c)
{
  mesh_conversa_t m;
  if (c < 0 || !tab5_mesh_conversa(c, &m)) { return chave_t{ }; }
  return chave_t{ true, m.canal, m.idx, m.no };
}

// A CONVERSA - baloes, barra de escrita, teclado e rolagem sao o componente do
// sistema (kernel/conversation). Ver conv_ler(), mais abaixo.
EXT_RAM_BSS_ATTR static conversa_t s_c;   // ~1 KB, so a interface toca: PSRAM
static bool s_eh_canal = false;       // conversa aberta e de canal (rotulo de remetente)
static void atualizar_eh_canal(void);

// Historico da conversa aberta. Em RAM para a tela desenhar rapido; a fonte da
// verdade e o cartao.
// HISTORICO NA PSRAM, e 200 em vez de 40.
//
// Eram 7,6KB de RAM INTERNA presos mesmo com o app fechado - o maior simbolo
// estatico do sistema inteiro. Na PSRAM, alocado ao abrir e devolvido ao
// fechar, custa zero quando ninguem esta conversando; e 200 mensagens sao
// 38KB, irrelevantes nos 29MB livres.
//
// O numero maior nao e capricho: sem rolagem, 40 bastavam porque so cabiam
// umas 8 na tela e o resto era inalcancavel. Com rolagem, o limite deixa de ser
// a tela e passa a ser este vetor - entao ele precisa valer a pena subir.
#define HIST_MAX 200
static mesh_msg_t* s_hist = nullptr;   // o que a tela mostra (so a interface le)
static chave_t     s_hist_chave = { }; // de qual conversa e o s_hist

// A CARGA DO HISTORICO, FORA DA INTERFACE (kernel/work). O
// worker le o cartao para ESTE buffer - nunca o que a tela desenha - e a
// interface so troca os dois no ao_aviso(AVISO_TRABALHO). Antes a leitura era
// feita na tarefa de interface a cada mensagem nova: montar o SD e ler a
// conversa inteira com a tela e o toque parados.
static mesh_msg_t*   s_carga = nullptr;
static volatile int  s_carga_n = 0;
static chave_t       s_carga_chave = { };   // pela chave: a ordem pode mudar ate o worker rodar
static bool          s_carga_cartao = false;   // o pedido inclui carregar_do_cartao
static volatile bool s_carga_pronta = false;
static bool          s_carga_pedida = false;
static bool          s_recarregar   = false;   // pediram de novo durante a carga
static int  s_n_hist = 0;

// ---------------------------------------------------------------------------
// AS TRES LISTAS DO APP - todas kernel/list
// ---------------------------------------------------------------------------
// Mensagens, conversas e nos rolam cada uma na sua janela; o trilho e a
// navegacao ficam parados. Por isso nao e a rolagem do sistema
// (altura_conteudo), que rola a area inteira.
//
//   baloes   - a lista do componente de conversa (s_c.lista).
//   s_l_conv - coluna de conversas (ate MESH_MAX_CONVERSAS; cabem umas 7).
//   s_l_nos  - tabela de nos (ate MESH_MAX_NOS; cabem umas 16), abaixo do
//              cabecalho fixo.
//
// A lista cuida de rolar, recortar, desenhar so o exposto, a barra, e do
// toque (arraste e "qual item"). O app so diz a altura e como desenhar um
// item. Ao trocar de pagina, ir_para_pagina() esconde as listas da pagina
// que saiu - uma lista que nao esta na tela nao responde a toque.
static lista_t s_l_conv;
static lista_t s_l_nos;
static bool s_hist_pronto = false;


// Repintura completa pendente. Sem isto o app pisca: ele redesenha a 1Hz
// (periodo_ms = 1000) e cada pagina limpava a area inteira antes de escrever.
// Texto com cor de fundo SOBRESCREVE, e nao pisca; quem pisca e o fillRect.
// Mesma licao de Ajustes e da barra de status.
static pintura_t s_p;

// Atalho local: dentro do desenhar() todos os blocos perguntam "esta passada e
// completa?", e escrever pintura_completa(&s_p) em cada um ficaria ilegivel.
#define TUDO  pintura_completa(&s_p)

// Quantas mensagens o servico tinha quando lemos o cartao pela ultima vez.
//
// Existe para NAO reler o historico a cada repintura. Ler significa montar o
// cartao, percorrer o arquivo e desmontar - caro, e desnecessario quando so o
// desenho mudou. Recarregamos apenas quando a conversa muda ou quando chega
// mensagem nova.
// int32_t, e nao uint32_t: -1 tem que caber como "nunca li ainda" e ser
// diferente de qualquer tab5_mesh_total_msgs() de verdade (que comeca em 0)
// - ver o aviso grande junto do uso, mais abaixo.
static int64_t s_msgs_quando_li = -1;

// Versao dos estados de envio que a tela ja desenhou.
static uint32_t s_envios_ver = 0;

// Roda NO WORKER. So toca s_carga*: a tela nao le nada daqui.
static void trabalho_carregar(void*)
{
  if (s_carga_cartao) { tab5_mesh_carregar_do_cartao(); }
  const chave_t k = s_carga_chave;
  s_carga_n = k.ok ? tab5_mesh_historico_de(k.canal, k.idx, k.no, s_carga, HIST_MAX) : 0;
  s_carga_pronta = true;
}

static bool alocar_hist(void)
{
  if (!s_hist)  { s_hist  = (mesh_msg_t*)heap_caps_malloc(sizeof(mesh_msg_t) * HIST_MAX, MALLOC_CAP_SPIRAM); }
  if (!s_carga) { s_carga = (mesh_msg_t*)heap_caps_malloc(sizeof(mesh_msg_t) * HIST_MAX, MALLOC_CAP_SPIRAM); }
  return s_hist && s_carga;
}

// PEDE a carga (nao le nada aqui). Conversa diferente da que esta na tela: a
// tela esvazia na hora, em vez de mostrar a conversa anterior ate a carga
// chegar.
static void carregar_hist(void)
{
  s_hist_pronto = true;   // pedido feito; o resultado chega por aviso
  if (!alocar_hist()) { s_n_hist = 0; return; }
  if (!chave_igual(s_hist_chave, s_ab)) {
    s_n_hist = 0;
    s_hist_chave = s_ab;
    atualizar_eh_canal();
    conversa_mudou(&s_c);
  }
  if (s_carga_pedida) { s_recarregar = true; return; }
  s_carga_chave  = s_ab;
  s_carga_pronta = false;
  s_carga_pedida = sistema_trabalho(trabalho_carregar, nullptr);
  if (s_carga_pedida) { s_carga_cartao = false; }
  else                { s_hist_pronto = false; }   // queue cheia: tenta na proxima passada
}

// Na interface. Carga terminou -> troca os buffers; qualquer outro aviso
// (mensagem, envio, nos) -> redesenha, e o desenhar() confere os contadores.
static void ao_aviso(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_carga_pronta) {
    s_carga_pronta = false;
    s_carga_pedida = false;
    if (chave_igual(s_carga_chave, s_ab)) {
      mesh_msg_t* t = s_hist; s_hist = s_carga; s_carga = t;
      s_n_hist     = s_carga_n;
      s_hist_chave = s_carga_chave;
      conversa_mudou(&s_c);
    }
    // Conversa trocou durante a carga, ou chegou mensagem no meio: de novo.
    if (!chave_igual(s_carga_chave, s_ab) || s_recarregar) {
      s_recarregar = false;
      s_hist_pronto = false;
    }
    // A primeira carga tambem trouxe nomes e conversas do cartao: a coluna de
    // conversas precisa ser repintada inteira.
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }
  sistema_pedir_redesenho();
}

// ---------------------------------------------------------------------------
// Desenho
// ---------------------------------------------------------------------------
// "caixa" e o nome que este app ja usava para tab5_botao() (ver tab5_ui.hpp) -
// mantido aqui como apelido fino para nao renomear cada chamada. Ate
// isto era outra copia local do mesmo botao que existia em tres
// apps a mais.
static void caixa(lgfx::LovyanGFX& d, int x, int y, int l, int a,
                  const char* txt, uint16_t fundo, uint16_t tinta, float tam)
{
  tab5_botao(d, x, y, l, a, txt, fundo, tinta, tam);
}

// ---------------------------------------------------------------------------
// GLIFOS DAS COLUNAS
// ---------------------------------------------------------------------------
// As letras "C", "N" e "M" eram a fonte embutida em tamanho 4 - cada pixel dela
// virava um quadrado de 4, e era isso que serrilhava. Mesma solucao dos icones
// da tela inicial: primitivas em fracao do tamanho, que saem limpas em qualquer
// escala e ainda dizem o que a letra so abreviava.
//
// A COR VEM DE FORA porque cada glifo tem dois estados: selecionado desenha em
// preto sobre o azul da caixa, nao selecionado em branco sobre o fundo escuro.
// O desenho e o mesmo nos dois - so a tinta muda.
typedef enum { GL_CONVERSAS = 0, GL_NOS, GL_MALHA, GL_CONFIG, GL_DIFUSAO, GL_DIRETO } glifo_t;

// ENGRENAGEM: o sinal de "aqui se configura" - no trilho e ao lado de cada
// opcao que da para alterar. Oito dentes em linhas grossas, o corpo e o furo
// com a cor do fundo (passada por quem chama).
static void engrenagem(lgfx::LovyanGFX& d, int cx, int cy, int r, uint16_t cor, uint16_t fundo)
{
  int dente = r * 2 / 5;
  if (dente < 2) { dente = 2; }
  for (int k = 0; k < 8; ++k) {
    const float ang = (float)M_PI * k / 4.0f;
    tab5_linha_grossa(d, cx + (int)(r * 0.55f * cosf(ang)), cy + (int)(r * 0.55f * sinf(ang)),
                   cx + (int)(r * cosf(ang)),         cy + (int)(r * sinf(ang)), dente, cor);
  }
  d.fillCircle(cx, cy, r * 3 / 4, cor);
  d.fillCircle(cx, cy, r / 3, fundo);
}

static void glifo(lgfx::LovyanGFX& d, glifo_t g, int cx, int cy, int lado, uint16_t cor)
{
  const int r = lado / 2;
  int gr = lado / 12;                       // espessura base
  if (gr < 2) { gr = 2; }

  switch (g) {
    case GL_CONVERSAS: {
      // Balao de fala: corpo arredondado e a ponta embaixo a esquerda. E o
      // desenho universal de "aqui se conversa".
      const int l = (int)(r * 1.5f), a = (int)(r * 1.1f);
      d.drawRoundRect(cx - l/2, cy - a/2 - r/6, l, a, a/3, cor);
      d.drawRoundRect(cx - l/2 + 1, cy - a/2 - r/6 + 1, l - 2, a - 2, a/3, cor);
      d.fillTriangle(cx - l/6, cy + a/2 - r/6 - 1,
                     cx - l/6 + a/3, cy + a/2 - r/6 - 1,
                     cx - l/3, cy + a/2 + r/3, cor);
      break;
    }
    case GL_NOS: {
      // Lista: tres pontos, cada um com a sua linha. E um CADASTRO - foi essa a
      // diferenca que faltava para nao se confundir com a malha.
      for (int i = -1; i <= 1; ++i) {
        const int y = cy + i * (r / 2);
        d.fillCircle(cx - r * 3 / 5, y, gr, cor);
        d.fillRect(cx - r / 5, y - gr / 2, r, gr, cor);
      }
      break;
    }
    case GL_MALHA: {
      // Grafo: um no no centro e quatro em volta, ligados. Aqui a informacao e
      // a LIGACAO, e nao a lista - por isso as arestas sao o que se ve primeiro.
      const int d4[4][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,1} };
      for (int i = 0; i < 4; ++i) {
        const int x = cx + d4[i][0] * r * 3 / 5;
        const int y = cy + d4[i][1] * r * 3 / 5;
        d.drawLine(cx, cy, x, y, cor);
        d.fillCircle(x, y, gr + 1, cor);
      }
      d.fillCircle(cx, cy, gr + 2, cor);
      break;
    }
    case GL_CONFIG: {
      // A cor de fundo do furo: preto sobre o azul da caixa selecionada,
      // o fundo do trilho na nao selecionada - a tinta diz qual e.
      engrenagem(d, cx, cy, r * 3 / 4, cor, cor == TFT_BLACK ? TFT_SKYBLUE : d.color565(13, 17, 23));
      break;
    }
    case GL_DIFUSAO: {
      // Difusao: um ponto e ondas saindo dele para os dois lados. Fala para
      // todo mundo, e nao para alguem - e a diferenca que o icone precisa
      // carregar contra o GL_DIRETO.
      d.fillCircle(cx, cy, gr + 1, cor);
      for (int k = 1; k <= 2; ++k) {
        const int rr = k * r / 2;
        d.drawArc(cx, cy, rr, rr - gr + 1,  30,  150, cor);
        d.drawArc(cx, cy, rr, rr - gr + 1, 210,  330, cor);
      }
      break;
    }
    case GL_DIRETO: {
      // Uma pessoa: cabeca e ombros. Conversa direta e com ALGUEM.
      d.fillCircle(cx, cy - r / 3, r / 3, cor);
      d.fillArc(cx, cy + r * 3 / 4, r * 2 / 3, r * 2 / 3 - gr * 2, 180, 360, cor);
      break;
    }
  }
}

// Caixa com glifo no lugar do texto. Mesma geometria e mesmas cores da caixa()
// de texto, para os dois conviverem sem desalinhar.
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
// Desenhada com primitivas e nao guardada como imagem: assim ela sai em
// qualquer tamanho sem serrilhar, nao ocupa flash, e o mesmo codigo serve para
// o icone de 190px da tela inicial e para o de 48px do trilho.
//
// A geometria saiu do SVG oficial (meshtastic/design, logo/logo.svg): fundo
// verde rgb(103,234,148) e duas cristas em rgb(44,45,60) - a crista cheia a
// direita e uma subida solta a esquerda, que e a perna de um segundo pico atras
// dela. O desenho e inspirado nos chirps do LoRa e le como montanha, tenda e a
// letra M ao mesmo tempo; a ambiguidade e proposital no original.
//
// Coordenadas em FRACAO do lado, nao em pixels. E o que permite o mesmo traco
// nos dois tamanhos sem uma segunda tabela para manter em dia.
static void marca_mesh(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t verde  = d.color565(103, 234, 148);
  const uint16_t escuro = d.color565(44, 45, 60);

  d.fillRoundRect(x, y, lado, lado, lado / 6, verde);

  // ESPESSURA MEDIDA NO SVG, nao estimada.
  //
  // A primeira versao usava 9% do lado e saiu gorda. No original o traco tem
  // ~14,7 unidades e a crista ~140 de largura: 10,5% DA CRISTA. Como a crista
  // ocupa cerca de metade do icone, sobra ~5% do lado - quase metade da
  // primeira estimativa.
  //
  // O piso de 3px continua: abaixo disso a marca some em vez de encolher.
  int grosso = (lado * 5) / 100;
  if (grosso < 3) { grosso = 3; }

  #define MX(f) (x + (int)((f) * lado))
  #define MY(f) (y + (int)((f) * lado))

  // Crista principal. Proporcao do original: ~0,49 de largura por ~0,36 de
  // altura - mais achatada do que a primeira versao.
  tab5_linha_grossa(d, MX(0.365f), MY(0.72f), MX(0.610f), MY(0.34f), grosso, escuro);
  tab5_linha_grossa(d, MX(0.610f), MY(0.34f), MX(0.855f), MY(0.72f), grosso, escuro);

  // A subida solta a esquerda: MESMA BASE, MESMO TOPO e MESMA inclinacao da
  // perna esquerda da crista. Ela e a perna de um segundo pico atras, e um pico
  // atras tem a altura do da frente - parando no meio do caminho, como estava,
  // ela virava um risco solto em vez de montanha.
  //
  // Sendo paralelas, as duas nunca se cruzam por construcao: a separacao e
  // constante em toda a altura, e nao depende da escolha exata dos
  // pontos.
  tab5_linha_grossa(d, MX(0.120f), MY(0.72f), MX(0.365f), MY(0.34f), grosso, escuro);

  #undef MX
  #undef MY
}

// Declaradas aqui, definidas mais abaixo perto de desenhar_msgs() (onde
// nasceram) - trilho e coluna de conversas tambem precisam da
// mesma paleta agora, e vem ANTES no arquivo.
static uint16_t fundo_msg(lgfx::LovyanGFX& d);

static void desenhar_trilho(lgfx::LovyanGFX& d, const area_t& a)
{
  // Era SIS_FUNDO (a paleta do sistema): as colunas sao
  // MOLDURA, nao conteudo, e tinham que recuar como a barra recua. Trocado
  // pela paleta do proprio Mesh (fundo_msg) - para o trilho, a lista de conversas e o
  // painel de mensagens lerem como UMA SO tela, nao duas paletas coladas.
  tab5_tela_limpar(a.x, a.y, TRILHO_L, a.altura, fundo_msg(d));
  // RETANGULO INTEIRO, e nao so a linha da direita. Com uma borda so, a coluna
  // parecia sangrar para fora da area em cima e embaixo; fechada, ela vira uma
  // peca.
  //
  // SIS_BORDA e nao branco: o branco puro tinha o maior contraste possivel
  // contra o fundo escuro e a moldura gritava mais que o conteudo dela. Este
  // cinza se ve sem disputar.
  d.drawRect(a.x, a.y, TRILHO_L, a.altura, SIS_BORDA);
  const int nao_lidas = tab5_mesh_nao_lidas();
  const int diretas   = tab5_mesh_nao_lidas_diretas();
  const uint16_t FUNDO = fundo_msg(d);

  for (int i = 0; i < PAG_TOTAL; ++i) {
    const int y = a.y + 16 + i * (ITEM_A + 12);
    const bool sel = ((int)s_pag == i);
    // Fundo do icone NAO selecionado: era TFT_BLACK puro (achado em
    // "as imagens... com o fundo antigo") e destoava do trilho
    // ja recolorido ao redor dele. Agora acompanha a mesma paleta.
    caixa_glifo(d, a.x + 12, y, TRILHO_L - 24, ITEM_A, (glifo_t)i,
                sel ? TFT_SKYBLUE : FUNDO, sel ? TFT_BLACK : TFT_WHITE);

    // PONTO, NAO NUMERO, e a diferenca e de proposito.
    //
    // O numero respondia "quantas" - uma pergunta que a coluna de conversas ja
    // responde melhor, conversa por conversa. Aqui, num icone de 66px, ele so
    // competia por espaco com a letra e ainda precisava apagar o proprio fundo.
    //
    // O que o trilho precisa dizer e "tem coisa nova ali", e para isso um ponto
    // basta - o olho pega de relance, sem ler. Vermelho quando ha mensagem
    // DIRETA: endereçada a voce nao e a mesma coisa que trafego do canal.
    if (i == PAG_MSGS && nao_lidas > 0) {
      const int px = a.x + TRILHO_L - 24;
      const int py = y + 12;
      d.fillCircle(px, py, 7, diretas > 0 ? TFT_RED : TFT_SKYBLUE);
      d.drawCircle(px, py, 7, TFT_BLACK);      // contorno: destaca no fundo claro
    }
  }
  // Estado do enlace no pe do trilho: verde vivo, vermelho mudo. E a resposta
  // para "esta conectado?" sem sair da pagina em que voce esta.
  const bool ok = tab5_mesh_no_ar();

  // MARCA EMBAIXO, PONTO EM CIMA. Juntos respondem "que rede e esta" e "ela
  // esta viva" sem ocupar linha de texto nenhuma - mas a marca e a identidade
  // do app e fecha melhor o pe do trilho; o ponto e estado, e estado sobe.
  const int ml    = TRILHO_L - 36;
  const int marca_y = a.y + a.altura - 18 - ml;   // mesma margem de pe de antes
  const int led_cy  = marca_y - 22 - 12;          // mesmo respiro de 22px

  d.fillCircle(a.x + TRILHO_L / 2, led_cy, 12, ok ? TFT_GREENYELLOW : TFT_RED);
  marca_mesh(d, a.x + (TRILHO_L - ml) / 2, marca_y, ml);
}

// A COLUNA DE CONVERSAS E UMA kernel/list. Item de altura fixa;
// a lista rola, recorta, desenha a faixa exposta, a barra e responde ao toque.
// A coluna so e repintada por inteiro em passada completa (a unica hora em que
// nomes, selecao e nao-lidas mudam); fora disso, so a rolagem.
static const int CONV_PASSO = ITEM_A + 8;

// O TRACO DA INTERNET: antes da primeira conversa da rede publica (que a ordem
// do servico poe por ultimo), uma linha com "INTERNET". Faz parte do proprio item -
// a lista so conhece alturas de item - e o toque desconta a altura dele.
static const int CONV_DIV_A = 34;
static int s_conv_div = -1;   // indice do item que leva o traco; -1 = nenhum

static int conv_altura(int i, void*) { return CONV_PASSO + (i == s_conv_div ? CONV_DIV_A : 0); }

static int achar_conv_div(void)
{
  const int n = tab5_mesh_num_conversas();
  for (int i = 0; i < n; ++i) {
    mesh_conversa_t c;
    if (tab5_mesh_conversa(i, &c) && c.canal && mesh_canal_internet(c.idx)) { return i; }
  }
  return -1;
}

// O que cada item mostra agora (nome e nao-lidas), para a coluna perceber
// sozinha o que mudou - antes, o contador de uma conversa que nao era a aberta
// so aparecia na proxima passada completa.
typedef struct { char nome[MESH_NOME_CONVERSA]; char sub[MESH_NOME_LONGO]; int nao_lidas; } conv_visto_t;
static conv_visto_t s_conv_visto[MESH_MAX_CONVERSAS];

static void conv_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int largura, void*)
{
  mesh_conversa_t c;
  if (!tab5_mesh_conversa(i, &c)) { return; }
  if (i < MESH_MAX_CONVERSAS) {
    snprintf(s_conv_visto[i].nome, sizeof(s_conv_visto[i].nome), "%s", c.nome);
    snprintf(s_conv_visto[i].sub, sizeof(s_conv_visto[i].sub), "%s", c.sub);
    s_conv_visto[i].nao_lidas = c.nao_lidas;
  }
  const uint16_t FUNDO = fundo_msg(d);
  if (i == s_conv_div) {
    const int ly = y + CONV_DIV_A / 2;
    tab5_texto(d, 1.9f);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_SKYBLUE, FUNDO);
    d.drawString("INTERNET", x + 12, ly);
    const int tx = x + 12 + (int)d.textWidth("INTERNET") + 10;
    d.drawFastHLine(tx, ly, largura - (tx - x) - 10, TFT_SKYBLUE);
    d.setTextDatum(top_left);
    y += CONV_DIV_A;
  }
  // O item comeca pelo respiro de 8px: o contador de nao-lidas sobe 6px acima
  // do icone e precisa estar DENTRO do proprio item - a lista so repinta o
  // item que cruza a faixa exposta, e um pedaco fora dele ficaria sem dono.
  y += 8;
  const bool sel = (i == s_conv);

  // O TIPO DA CONVERSA, e nao a inicial do nome. A inicial em tamanho 4 era a
  // fonte embutida ampliada, serrilhada, e dizia menos do que parecia: o nome
  // inteiro esta escrito ao lado, entao a inicial repetia informacao. O que a
  // coluna nao dizia era se aquilo e um CANAL (todo mundo ouve) ou alguem
  // especifico - e e isso que muda como voce escreve.
  // Cor do icone de TIPO (canal/direto) mantida saturada de proposito - e um
  // indicador funcional (distinguir os dois tipos de conversa de relance),
  // nao "a cor do balao"; deixei fora do recolorimento por
  // falta de referencia clara no que foi mandado (a captura de tela nao
  // mostra esta tela). Os avatares coloridos por remetente (cor_do_no), que
  // SAO equivalentes ao que a captura mostra, ja seguem a paleta nova.
  caixa_glifo(d, x + 10, y, ITEM_A, ITEM_A, c.canal ? GL_DIFUSAO : GL_DIRETO,
              sel ? TFT_SKYBLUE : (c.canal ? TFT_NAVY : TFT_DARKGREEN),
              sel ? TFT_BLACK : TFT_WHITE);

  // NOME COM LARGURA QUE CABE, medido de verdade: o texto comeca em
  // (10 + ITEM_A + 8) = 90px dentro da coluna, o divisor esta em CONV_L-1 =
  // 199. Antes disto era "12px por caractere" (a largura fixa da Font0);
  // a Font4 e proporcional, entao o corte agora mede com d.textWidth() e
  // tira uma letra de cada vez ate caber - a mesma tecnica usada no resto
  // do sistema desde a troca de fonte. O acolchoamento com
  // espacos a direita saiu: a lista limpa a faixa do item antes de
  // desenha-lo, entao nao sobra resto de nome antigo para apagar.
  const int nome_x = x + 10 + ITEM_A + 8;
  const int nome_max_l = largura - (nome_x - x);
  // +60%. RISCO
  // ACEITO: nome_max_l nao mudou (a coluna continua com 200px), entao um
  // nome que cabia inteiro antes pode passar a truncar uma letra a mais -
  // o corte por d.textWidth() logo abaixo ja protege contra estourar a
  // coluna, so mostra menos caracteres se precisar.
  tab5_texto(d, 3.84f);   // +60%
  d.setTextDatum(top_left);
  d.setTextColor(sel ? TFT_SKYBLUE : TFT_WHITE, FUNDO);
  char nome[24];
  snprintf(nome, sizeof(nome), "%s", c.nome);
  tab5_cortar_ao_caber(d, nome, nome_max_l);   // por caractere: o nome pode ter acento
  d.drawString(nome, nome_x, y + 12);

  // SEGUNDA LINHA: o nome longo de quem e a direta (ou "canal N"). E o que
  // responde "quem e" quando o curto e uma sigla - sem recorrer ao id. Na
  // conversa aberta ela para antes do botao de apagar, que mora nesta altura.
  if (c.sub[0]) {
    tab5_texto(d, 2.4f);
    d.setTextColor(TFT_DARKGREY, FUNDO);
    const int sub_max_l = sel ? (apagar_dx() - (nome_x - x) - 6) : nome_max_l;
    char sub[MESH_NOME_LONGO];
    snprintf(sub, sizeof(sub), "%s", c.sub);
    tab5_cortar_ao_caber(d, sub, sub_max_l);   // por caractere: o longo pode ter acento
    d.drawString(sub, nome_x, y + 46);
  }

  // Contador sobre o CANTO do icone, como em aplicativo de mensagem. Antes
  // ele ficava a direita do nome e disputava espaco com ele - e o nome sempre
  // ganhava, porque e desenhado depois.
  if (c.nao_lidas > 0) {
    char nl[8]; snprintf(nl, sizeof(nl), "%d", c.nao_lidas);
    caixa(d, x + 10 + ITEM_A - 24, y - 6, 30, 26, nl, TFT_RED, TFT_WHITE, 2);
  }

  // APAGAR: so na conversa ABERTA - canal TAMBEM.
  //
  // So na aberta para nao encher a coluna de botoes vermelhos - voce apaga a
  // que esta olhando, que e a que acabou de perceber que nao queria. Ate
  // canal nao ganhava este botao (um canal apagado voltava sozinho) - deixou de ser verdade quando canal passou a entrar na
  // mesma flag de apagados que ja existia para no (ver o aviso grande em
  // tab5_mesh.cpp, junto dela). Reabrir depois: pela secao "CANAIS
  // DISPONIVEIS" na pagina de Nos, ou so esperando mensagem nova chegar.
  if (sel) {
    caixa(d, x + apagar_dx(), y + APAGAR_DY, APAGAR_L, APAGAR_A,
          "x", TFT_MAROON, TFT_WHITE, 2);
  }
}

// A janela da coluna: por DENTRO da moldura (1px) e acima do teclado, que
// cobre a largura inteira quando aberto. O que o teclado cobre nao rola nem
// responde - a lista so conhece o que desenhou.
// A largura da coluna sai daqui e fica gravada em s_col_l: o item (o "x" de
// apagar) e o toque leem o mesmo numero que a moldura usou.
static area_t janela_conversas(const area_t& a)
{
  s_col_l = em_pe(a) ? a.largura - TRILHO_L : CONV_L;
  const int alt = a.altura - (conversa_teclado_aberto(&s_c) ? tecvirt_altura(a) : 0);
  return area_t{ a.x + TRILHO_L + 1, a.y + 1, s_col_l - 2, alt - 2 };
}

static void desenhar_conversas(lgfx::LovyanGFX& d, const area_t& a)
{
  const uint16_t FUNDO = fundo_msg(d);
  const area_t j = janela_conversas(a);
  const int n = tab5_mesh_num_conversas();
  const int div = achar_conv_div();
  const bool div_mudou = (div != s_conv_div);
  s_conv_div = div;

  if (TUDO || n != s_l_conv.n || div_mudou) {
    lista_itens_mudaram(&s_l_conv, n);
  } else {
    for (int i = 0; i < n && i < MESH_MAX_CONVERSAS; ++i) {
      mesh_conversa_t c;
      if (!tab5_mesh_conversa(i, &c)) { continue; }
      if (c.nao_lidas != s_conv_visto[i].nao_lidas || strcmp(c.nome, s_conv_visto[i].nome) != 0
       || strcmp(c.sub, s_conv_visto[i].sub) != 0) {
        lista_item_mudou(&s_l_conv, i);
      }
    }
  }
  const bool pintou = lista_desenhar(&s_l_conv, d, j, FUNDO, TUDO);

  // Lista vazia repintada (passada completa, ou a ultima conversa apagada):
  // o aviso vai por cima do fundo que a lista acabou de pintar.
  if (n == 0 && pintou) {
    tab5_texto(d, 2.4f);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_LIGHTGREY, FUNDO);
    d.drawString("sem conversas", j.x + 11, j.y + 19);
  }
  // Moldura FORA da janela da lista: nada que a lista pinte a alcanca, e
  // basta desenha-la na passada completa. No cinza da moldura do sistema.
  if (TUDO) { d.drawRect(a.x + TRILHO_L, a.y, s_col_l, j.altura + 2, SIS_BORDA); }
}

// "HA QUANTO TEMPO", curto.
//
// E a pergunta que o app do celular responde e a nossa lista nao respondia:
// SNR alto num no que ninguem ouve ha dois dias nao quer dizer nada. Formato
// curto de proposito - a coluna e estreita e o que importa e a ordem de
// grandeza, nao o segundo exato.
static void idade_texto(int32_t s, char* saida, size_t tam)
{
  if      (s < 0)     { snprintf(saida, tam, "?");     }
  else if (s < 90)    { snprintf(saida, tam, "agora"); }
  else if (s < 3600)  { snprintf(saida, tam, "%dm", (int)(s / 60)); }
  else if (s < 86400) { snprintf(saida, tam, "%dh%02d",
                                 (int)(s / 3600), (int)((s % 3600) / 60)); }
  else                { snprintf(saida, tam, "%dd", (int)(s / 86400)); }
}

// Verde ate 15 min, amarelo ate 2h, cinza depois. As 2h sao a fronteira que o
// proprio Meshtastic usa para chamar um no de "online" - vale copiar, para a
// lista aqui e a do celular contarem a mesma historia.
static uint16_t idade_cor(int32_t s)
{
  if (s < 0)    { return TFT_DARKGREY; }
  if (s <= 900) { return TFT_GREENYELLOW; }
  if (s <= 7200){ return TFT_YELLOW; }
  return TFT_DARKGREY;
}

// ---------------------------------------------------------------------------
// A GRADE DAS COLUNAS
// ---------------------------------------------------------------------------
// Cabecalho e linhas sao montados a partir DESTA tabela, e so dela. E a unica
// forma de os dois nao se descolarem - e eles tinham se descolado.
//
// Antes a linha era um formato unico com os vaos contados a mao:
//
//   "%-22.22s  %-6.6s %5.1f %3u%%   %u   "
//
// Repare nos espacos: 2, 1, 1, 3, 3. Cada coluna com um respiro diferente, e o
// cabecalho era OUTRA string, com a sua propria contagem - entao "snr" caia um
// caractere fora dos valores e "visto" encostava em "saltos". Mexer numa das
// duas sem mexer na outra desalinhava tudo de novo.
//
// Regra unica agora: vao de 2 caracteres entre colunas, e o titulo alinha do
// mesmo jeito que o valor - numero a direita, texto a esquerda.
typedef struct {
  int         col;      // caractere onde a coluna comeca
  int         larg;
  bool        direita;  // alinhamento do valor E do titulo
  const char* titulo;
} coluna_t;

enum { C_NOME = 0, C_CURTO, C_ID, C_HW, C_SNR, C_BAT, C_SALTOS, C_VISTO, C_TOTAL };

// IDENTIDADE E ETIQUETA, LADO A LADO.
//
// "nome" e "curto" sao ETIQUETAS: quem renomeia o no no celular troca as duas.
// Nao dizem com que aparelho voce esta falando, e era so isso que a tabela
// mostrava. As duas colunas novas dizem:
//
//   id  - "!a1b2c3d4", derivado do MAC, acompanha o radio para sempre. E o
//         identificador que o Meshtastic usa em todo lugar: log, mapa, celular.
//   hw  - qual placa e (T-Beam, Heltec V3...). Explica sozinho por que um no
//         tem bateria e outro nao, por que um anda e outro nao.
//
// A tela e de 1280px em paisagem. A ultima coluna termina no caractere 85
// (com os vaos de 2 caracteres corrigidos), que da 1126px com a margem e o
// x0 de 90px do trilho - cabe com ~150px de folga para o indicador de
// rolagem e ainda sobra chao para uma coluna futura.
// OS VAOS ERAM DE 1 CARACTERE, NAO DE 2 - o comentario acima descrevia a
// regra certa, mas os numeros abaixo nao a seguiam (nome termina em 20,
// "curto" comecava em 21 - so 1 de vao, nao 2). Numa tabela de largura fixa,
// campo direita-alinhado colado a 12px do proximo campo esquerda-alinhado e
// exatamente o que le como "torto": o valor de uma coluna quase encosta no
// texto da vizinha, sem o respiro visual que separa "isto e uma coluna" de
// "isto e a proxima". Refeita com os 2 caracteres que a regra sempre pediu.
static const coluna_t COLUNA[C_TOTAL] = {
  {  0, 20, false, "nome"   },
  { 22,  5, false, "curto"  },
  { 29, 10, false, "id"     },
  { 41, 15, false, "modelo" },
  { 58,  5, true,  "snr"    },
  { 65,  4, true,  "bat"    },
  { 71,  6, true,  "saltos" },
  { 79,  6, false, "visto"  },
};

// A MESMA GRADE EM PE. Em 720 px, tirando o trilho, cabem ~48 caracteres -
// nao os 85 de cima. Ficam as colunas que respondem "da para falar com ele
// agora?": nome, sinal, bateria, saltos e ha quanto tempo foi ouvido. Saem
// curto, id e modelo (largura 0: campo() nao escreve nada), que continuam a
// um toque, no cartao do no. Mesma regra de vao de 2 caracteres.
static const coluna_t COLUNA_PE[C_TOTAL] = {
  {  0, 16, false, "nome"   },
  {  0,  0, false, "curto"  },
  {  0,  0, false, "id"     },
  {  0,  0, false, "modelo" },
  { 18,  5, true,  "snr"    },
  { 25,  4, true,  "bat"    },
  { 31,  6, true,  "saltos" },
  { 39,  6, false, "visto"  },
};

// A grade em uso: escolhida por desenhar_nos() a cada passada, pela area.
static const coluna_t* s_grade = COLUNA;

// Fonte 6x8 no tamanho 2 = 12px por caractere.
static const int NOS_CH = 12;
// A linha principal vai ate o fim de "saltos"; "visto" e desenhado a parte,
// porque tem cor propria. NOS_LINHA_CH e o MAIOR caso (deitado) - o tamanho
// dos buffers; o comprimento de agora vem da grade.
#define NOS_LINHA_CH  77        // COLUNA[C_SALTOS].col + larg
static int linha_ch(void) { return s_grade[C_SALTOS].col + s_grade[C_SALTOS].larg; }
// Margem esquerda da tabela: a coluna da estrela (favorito) mora nela, e com
// 16 px a estrela encostava na borda da area.
static const int NOS_MARGEM  = 34;
static const int NOS_ESTRELA_X = 16;              // centro da estrela, dentro da margem
static int visto_x(void) { return NOS_MARGEM + s_grade[C_VISTO].col * NOS_CH; }

// OS CANAIS NO TOPO DA PAGINA DE NOS - todos os ativos do no, como botoes.
//
// Antes era uma lista de "canais disponiveis": so os SEM conversa na coluna,
// e so os que a configuracao do radio tinha marcado como ativos - e o canal 0
// nunca chegava a ser marcado (ver o leitor de canais em tab5_mesh.cpp). Um
// LongFast apagado da coluna nao tinha onde ser achado. Agora todo canal ativo
// esta sempre aqui; o que nao esta na coluna aparece apagado, com "+".
//
// A geometria e GRAVADA no desenho e LIDA no toque (s_chips): os dois nunca
// discordam, nem com nomes de larguras diferentes.
typedef struct { int x, y, l, a; uint8_t idx; } chip_t;   // coordenadas LOCAIS da area
static chip_t s_chips[MESH_MAX_CANAIS + 1];
static int    s_n_chips = 0;
static int    s_chips_altura = 0;          // altura da faixa dos canais, a partir do topo
static char   s_chips_assin[(MESH_MAX_CANAIS + 1) * (MESH_NOME_CONVERSA + 4) + 48];

// QUAL REDE A TABELA MOSTRA: no ou internet (botoes no topo da pagina). Na
// vista INTERNET, o botao de ligar/desligar a rede publica e o canal de la.
static uint8_t s_rede_vista = MESH_REDE_NO;
static chip_t  s_chip_rede[2];
static chip_t  s_chip_internet_liga;
static const int CHIP_A = 36;

// NO ESCOLHIDO NA TABELA: a pagina mostra o cartao dele em vez da tabela.
// Pelo NUMERO, nao pelo indice - a tabela se reordena por "visto".
static uint32_t s_no_sel = 0;

// Escreve um campo na sua coluna dentro de 'linha', que ja chega cheia de
// espacos. memcpy e nao snprintf: um NUL no meio cortaria o resto da linha.
// O strnlen limitado a largura tambem e o que impede um nome longo de invadir
// a coluna seguinte.
static void campo(char* linha, const coluna_t& c, const char* txt)
{
  const int n = (int)strnlen(txt, (size_t)c.larg);
  memcpy(linha + c.col + (c.direita ? c.larg - n : 0), txt, (size_t)n);
}

static void espacos(char* linha, size_t tam)
{
  memset(linha, ' ', tam - 1);
  linha[tam - 1] = 0;
}

// Troca cada sequencia UTF-8 de varios bytes (acento, emoji, qualquer coisa
// fora de ASCII puro) por UM UNICO '?'. Existe porque campo()/montar_linha()
// constroem a linha como uma GRADE DE LARGURA FIXA POR BYTE (1 byte = 1
// coluna de tela) - um nome com emoji (4 bytes UTF-8 desenhando UM icone so)
// ou so um acento (2 bytes por 1 letra, "Jose" -> "José") descola a conta:
// os bytes "extras" empurram os campos seguintes DAQUELA LINHA para a
// direita, sem empurrar as outras - e era exatamente o desalinhamento
// observado na tabela de nos.
//
// Nome de no vem de OUTRO radio, sem controle nosso, entao a defesa fica
// aqui, na entrada da tabela - nao adianta pedir para quem manda a mensagem
// n usar emoji. Nas CONVERSAS, onde o texto corre livre (sem grade de
// coluna), o emoji continua aparecendo de verdade - ver tab5_emoji_preparar().
static void sanitizar_ascii(const char* in, char* out, size_t out_tam)
{
  if (out_tam == 0) { return; }
  size_t o = 0;
  const unsigned char* p = (const unsigned char*)in;
  while (*p && o + 1 < out_tam) {
    if (*p < 0x80) { out[o++] = (char)*p; ++p; continue; }
    // Lead byte de sequencia multi-byte: pula ela INTEIRA de uma vez, senao
    // cada byte de continuacao viraria um '?' proprio - um emoji so ja vira
    // 4 pontos de interrogacao em vez de 1.
    const int n = ((*p & 0xE0) == 0xC0) ? 2
                : ((*p & 0xF0) == 0xE0) ? 3
                : ((*p & 0xF8) == 0xF0) ? 4
                : 1;    // byte de continuacao solto (UTF-8 invalido): 1 so
    out[o++] = '?';
    p += n;
  }
  out[o] = 0;
}

static void montar_linha(char* linha, size_t tam, const mesh_no_t& no)
{
  char v[24];
  char nome_limpo[MESH_NOME_LONGO];
  char curto_limpo[MESH_NOME_CURTO];
  espacos(linha, tam);

  // LONGO: mesma logica do curto logo abaixo - prefere a entrada AO VIVO
  // (NodeInfo desta sessao); sem ela, cai para o cache persistente
  // (tab5_mesh_nome_longo_cache(), nos.txt). Esta coluna so
  // olhava no.longo - a mesma classe de bug que o curto teve
  // (ver o aviso dele logo abaixo), so que nunca corrigida aqui: a maioria
  // dos nos nasce de telemetria/posicao/texto, nao de NodeInfo (a UNICA fonte
  // do nome longo), entao a tabela mostrava "(sem nome)" quase sempre.
  const char* longo_fonte = no.longo;
  char longo_cache[MESH_NOME_LONGO];
  if (!no.longo[0] && tab5_mesh_nome_longo_cache(no.num, longo_cache, sizeof(longo_cache))) {
    longo_fonte = longo_cache;
  }
  sanitizar_ascii(longo_fonte[0] ? longo_fonte : "(sem nome)", nome_limpo, sizeof(nome_limpo));

  // CURTO: prefere o que ESTE no informou ao vivo (NodeInfo desta sessao).
  // Sem isso, cai para o cache persistente (nos.txt) - o mesmo cache que a
  // conversa ja consulta via tab5_mesh_nome_exibicao(), mas aqui SEM o
  // fallback de hex dela (ver tab5_mesh_nome_cache() e o aviso completo no
  // .hpp): a coluna ID logo abaixo ja mostra o hex quando falta nome, entao
  // repeti-lo aqui na coluna de nome curto seria redundante.
  //
  // ACHADO (a maioria das linhas saia sem nome longo nem curto): esta tabela so olhava a entrada AO VIVO (no.curto). A maior
  // parte das entradas nasce de achar_no() sendo chamado por um pacote de
  // TELEMETRIA/POSICAO/TEXTO (ver ler_packet()/ler_data() no servico), nao
  // por um NodeInfo - e NodeInfo (o UNICO tipo que traz nome) e bem mais
  // raro na malha que os outros. Um no com quem ja se conversou antes (nome
  // gravado no cache ha muito tempo) aparecia em branco de novo, so porque a
  // entrada ao vivo de AGORA ainda nao recebeu um NodeInfo novo - o mesmo
  // problema que a conversa ja tinha resolvido, so que a tabela nunca ganhou
  // a mesma correcao.
  const char* curto_fonte = no.curto;
  char curto_cache[MESH_NOME_CURTO];
  if (!no.curto[0] && tab5_mesh_nome_cache(no.num, curto_cache, sizeof(curto_cache))) {
    curto_fonte = curto_cache;
  }
  sanitizar_ascii(curto_fonte, curto_limpo, sizeof(curto_limpo));

  campo(linha, s_grade[C_NOME],  nome_limpo);
  campo(linha, s_grade[C_CURTO], curto_limpo);

  // O id vem do proprio radio como "!a1b2c3d4". Quando ainda nao veio (o
  // node_info dele nao chegou), montamos a partir do numero - e a MESMA conta
  // que o Meshtastic faz, entao o valor confere.
  if (no.id[0]) { campo(linha, s_grade[C_ID], no.id); }
  else {
    snprintf(v, sizeof(v), "!%08lx", (unsigned long)no.num);
    campo(linha, s_grade[C_ID], v);
  }

  // Modelo conhecido pelo nome; desconhecido pelo numero, que da para procurar.
  // Inventar um nome para um valor que a tabela nao tem seria pior que o numero.
  const char* hw = mesh_hw_nome(no.hw);
  if (hw) { campo(linha, s_grade[C_HW], hw); }
  else    { snprintf(v, sizeof(v), "modelo %u", (unsigned)no.hw);
            campo(linha, s_grade[C_HW], v); }

  // O PONTO DECIMAL TEM QUE CAIR SEMPRE NA MESMA COLUNA - foi essa a raiz do
  // "torto" observado.
  //
  // campo() ja alinha a DIREITA corretamente (a string sempre termina rente a
  // borda direita do campo); o problema era o "%.1f" nao ter largura fixa.
  // "-18.5" tem 5 caracteres, "9.0" tem 3 - as duas terminam no mesmo lugar
  // (alinhadas a direita, do jeito certo), mas como comecam em colunas
  // DIFERENTES, o ponto decimal de uma linha nao fica embaixo do ponto
  // decimal da linha seguinte. Numa coluna de numeros, e o ponto decimal -
  // nao a borda - que o olho usa para saber que esta tudo alinhado.
  //
  // "%+05.1f" fixa sinal, dois digitos inteiros (com zero a esquerda quando
  // faltar) e uma casa decimal - sempre 5 caracteres, sempre no mesmo formato:
  // "-18.5", "+09.0", "-05.5", "+00.0". O ponto decimal cai sempre na mesma
  // posicao, para qualquer leitura entre -99.9 e +99.9 (a faixa real de SNR
  // do Meshtastic nunca chega perto disso).
  snprintf(v, sizeof(v), "%+05.1f", (double)no.snr);
  campo(linha, s_grade[C_SNR], (no.snr <= -128.0f) ? "-" : v);

  // 101 e o codigo do Meshtastic para "na tomada", nao uma bateria de 101%.
  // Mostrava "101%" antes. E 0 significa que a telemetria dele ainda nao veio.
  //
  // ZERO A ESQUERDA, pela mesma razao do SNR acima: "5%" e "100%" tem
  // larguras diferentes, e sem largura fixa o digito das dezenas de uma linha
  // nao cai embaixo do digito das dezenas da linha seguinte. "%03u%%" fixa
  // 4 caracteres sempre - "005%", "050%", "100%" - e o '%' e os digitos ficam
  // na mesma coluna em toda linha que tiver bateria de verdade.
  if      (no.bateria == 0)   { snprintf(v, sizeof(v), "-");  }
  else if (no.bateria > 100)  { snprintf(v, sizeof(v), "AC"); }
  else                        { snprintf(v, sizeof(v), "%03u%%", (unsigned)no.bateria); }
  campo(linha, s_grade[C_BAT], v);

  // "mqtt" no lugar do numero: no que chega pela internet nao tem salto de
  // radio nenhum, e mostrar "0" ali diria que ele e vizinho direto - o oposto
  // da verdade. E a informacao que decide se voce alcanca esse no com a
  // internet fora.
  //
  // "-" PARA DESCONHECIDO (nos apareciam sem nome e sem saltos): um no so criado por achar_no() ao ver um pacote
  // (sem NodeInfo ainda) tinha saltos=0 pelo memset, indistinguivel de um
  // vizinho direto de verdade. Agora o sentinela (MESH_SALTOS_DESCONHECIDO)
  // e explicito aqui, do jeito que o SNR (-128) ja era.
  if (no.via_mqtt) { snprintf(v, sizeof(v), "mqtt"); }
  else if (no.saltos == MESH_SALTOS_DESCONHECIDO) { snprintf(v, sizeof(v), "-"); }
  else             { snprintf(v, sizeof(v), "%u", (unsigned)no.saltos); }
  campo(linha, s_grade[C_SALTOS], v);
  // Em pe a linha e mais curta: termina aqui, senao os espacos do resto
  // passariam da borda da tela.
  if ((int)tam > linha_ch()) { linha[linha_ch()] = 0; }
}

// O que ja esta escrito em cada linha. Existe para NAO reescrever 32 linhas de
// texto por segundo: com o periodo de 1Hz e a idade correndo, quase toda
// passada mudava um punhado de linhas e redesenhava todas. Em paisagem isso
// custa caro - o M5GFX transpoe as coordenadas e cada linha de texto vira
// escrita em coluna na memoria do painel.
EXT_RAM_BSS_ATTR static char s_nos_ant[MESH_MAX_NOS][NOS_LINHA_CH + 1];
EXT_RAM_BSS_ATTR static char s_nos_visto_ant[MESH_MAX_NOS][8];
EXT_RAM_BSS_ATTR static bool s_nos_fav_ant[MESH_MAX_NOS];   // a estrela que esta desenhada

// 44 e nao 32: com rolagem, altura nao falta, e linhas coladas cansavam o
// olho. O texto (16 px) vai centrado e um fio escuro separa uma da outra.
static const int NOS_LINHA_A = 44;
static const int NOS_TEXTO_DY = (NOS_LINHA_A - 16) / 2;

static int nos_altura(int, void*) { return NOS_LINHA_A; }

// Uma linha da tabela. Guarda o que escreveu (s_nos_ant/s_nos_visto_ant): e a
// referencia com que desenhar_nos() decide, na passada seguinte, se a linha
// mudou. Font0 ja esta ativa (ver a excecao em desenhar_nos).
// ESTRELA de cinco pontas, em primitivas: nenhuma fonte do sistema tem o
// caractere, e desenhada ela sai igual em qualquer tamanho. Cheia = favorito;
// vazia (so o contorno) = o botao de marcar.
static void estrela(lgfx::LovyanGFX& d, int cx, int cy, int r, uint16_t cor, bool cheia)
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
    if (cheia) { d.fillTriangle(cx, cy, (int)px[k], (int)py[k], (int)px[j], (int)py[j], cor); }
    else       { d.drawLine((int)px[k], (int)py[k], (int)px[j], (int)py[j], cor); }
  }
}

static void nos_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int, void*)
{
  mesh_no_t no;
  if (!tab5_mesh_no_rede(s_rede_vista, i, &no) || i >= MESH_MAX_NOS) { return; }

  // BRANCO = o nosso no. AMARELO = ouvido pelo ar (tem SNR): fixos no topo,
  // os que da para alcancar com a internet fora. Pela internet, o azul do
  // MQTT das conversas; sem SNR, cinza.
  const bool meu = (no.num == tab5_mesh_meu_numero());
  const uint16_t cor = meu ? TFT_WHITE
                     : (no.fixado || tab5_mesh_no_pelo_ar(&no)) ? TFT_YELLOW
                     : no.via_mqtt ? TFT_SKYBLUE : TFT_DARKGREY;
  d.setTextSize(2);
  d.setTextDatum(top_left);
  montar_linha(s_nos_ant[i], sizeof(s_nos_ant[i]), no);
  d.setTextColor(cor, TFT_BLACK);
  d.drawString(s_nos_ant[i], x + NOS_MARGEM, y + NOS_TEXTO_DY);
  // Favorito: estrela na margem, antes do nome (os 16 px que ja sobravam).
  if (no.favorito) { estrela(d, x + NOS_ESTRELA_X, y + NOS_LINHA_A / 2, 8, TFT_GOLD, true); }
  // Divisao fraca, de pouco contraste: separa sem disputar com o texto.
  d.drawFastHLine(x + 6, y + NOS_LINHA_A - 1, visto_x() + 6 * NOS_CH - 6, d.color565(38, 42, 50));

  // Coluna separada porque tem cor propria: a idade responde "da para falar
  // com ele AGORA?", que e outra pergunta da qualidade do sinal.
  char curto[8];
  idade_texto(no.idade_s, curto, sizeof(curto));
  snprintf(s_nos_visto_ant[i], sizeof(s_nos_visto_ant[i]), "%-6.6s", curto);
  s_nos_fav_ant[i] = no.favorito;
  d.setTextColor(idade_cor(no.idade_s), TFT_BLACK);
  d.drawString(s_nos_visto_ant[i], x + visto_x(), y + NOS_TEXTO_DY);
}

// Assinatura dos canais oferecidos: muda quando um canal liga, desliga, troca
// de nome ou entra/sai da coluna - e ai a faixa e repintada inteira.
// O estado da rede de internet, so pelo que muda o desenho (nao pelo contador de
// pacotes, que correria a cada segundo).
static int internet_fase(void)
{
  return !ajustes_mesh_rede_internet() ? 0 : mesh_mqtt_internet_conectado() ? 2 : 1;
}

static void canais_assinatura(char* out, size_t tam)
{
  snprintf(out, tam, "r%u%d|", (unsigned)s_rede_vista, internet_fase());
  const int n = tab5_mesh_num_canais();
  for (int i = 0; i < n; ++i) {
    uint8_t idx; char nome[MESH_NOME_CONVERSA]; bool vis = false;
    if (!tab5_mesh_canal(i, &idx, nome, sizeof(nome), &vis)) { continue; }
    const size_t k = strlen(out);
    snprintf(out + k, tam - k, "%u%c%s|", (unsigned)idx, vis ? '+' : '-', nome);
  }
}

// Desenha a faixa dos canais e grava a geometria. Devolve a altura usada.
//
// NA FONTE DA TABELA (Font0 em tamanho 2, 12 px por caractere), e nao na
// suave do tab5_botao: a faixa e a tabela sao a mesma pagina, e duas fontes
// coladas liam como duas telas. Cada botao leva o numero do canal ("ch0") -
// e o que o Meshtastic usa para dizer qual e o primario, e o que distingue
// dois canais de nome parecido.
// Um botao da faixa, na fonte da tabela. Devolve a largura.
static int chip(lgfx::LovyanGFX& d, int x, int y, const char* rot, uint16_t fundo, uint16_t frente)
{
  const int l = (int)strlen(rot) * NOS_CH + 28;
  d.fillRoundRect(x, y, l, CHIP_A, 10, fundo);
  d.setTextColor(frente, fundo);
  d.drawString(rot, x + 14, y + CHIP_A / 2);
  return l;
}

static int desenhar_canais(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  d.setFont(&fonts::Font0);
  d.setTextSize(2);
  const int fim_x = a.x + a.largura - 16;
  const int ini_x = x0 + NOS_MARGEM + 8 * NOS_CH;   // depois do rotulo "canais"
  int x = ini_x, y = a.y + 8;
  d.setTextDatum(middle_left);

  // REDE: NODE | INTERNET, e na vista INTERNET o liga/desliga e o estado dela.
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString("rede", x0 + NOS_MARGEM, y + CHIP_A / 2);
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
    const bool lig = ajustes_mesh_rede_internet();
    const int l = chip(d, x, y, lig ? "DESLIGAR" : "LIGAR", lig ? TFT_MAROON : TFT_DARKGREEN, TFT_WHITE);
    s_chip_internet_liga = chip_t{ x - a.x, y - a.y, l, CHIP_A, 0 };
    x += l + 14;
    const int fase = internet_fase();
    char est[40];
    sanitizar_ascii(fase == 0 ? "desligada" : fase == 1 ? "conectando..." : "conectada", est, sizeof(est));
    d.setTextColor(fase == 2 ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
    if (x + (int)strlen(est) * NOS_CH < fim_x) { d.drawString(est, x, y + CHIP_A / 2); }
  }
  x = ini_x;
  y += CHIP_A + 8;
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString("canais", x0 + NOS_MARGEM, y + CHIP_A / 2);
  s_n_chips = 0;
  const int n = tab5_mesh_num_canais();
  for (int i = 0; i < n && s_n_chips < MESH_MAX_CANAIS + 1; ++i) {
    uint8_t idx; char nome[MESH_NOME_CONVERSA]; bool vis = false;
    if (!tab5_mesh_canal(i, &idx, nome, sizeof(nome), &vis)) { continue; }
    if (mesh_canal_internet(idx) != (s_rede_vista == MESH_REDE_INTERNET)) { continue; }   // so os da rede vista
    char limpo[MESH_NOME_CONVERSA];
    sanitizar_ascii(nome, limpo, sizeof(limpo));   // Font0: so ASCII (ver sanitizar_ascii)
    char rot[MESH_NOME_CONVERSA + 12];
    if (mesh_canal_internet(idx)) { snprintf(rot, sizeof(rot), "%s%s", vis ? "" : "+ ", limpo); }
    else { snprintf(rot, sizeof(rot), "%sch%u %s", vis ? "" : "+ ", (unsigned)idx, limpo); }
    const int l = (int)strlen(rot) * NOS_CH + 28;
    if (x + l > fim_x && x > ini_x) { x = ini_x; y += CHIP_A + 8; }
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

// CARTAO DO NO - tudo o que se sabe dele, como o cliente web mostra. Cabe o
// que a tabela nao tem largura para mostrar (MAC, tensao, posicao) sem
// apertar mais uma coluna nela.
static const int DET_LIN   = 50;
static const int DET_BTN_A = 64;
static const int DET_BTN_L = 300;
static const int DET_BTN_Y_DA_BASE = 84;    // de baixo para cima, ver ao_tocar
// Botao da ESTRELA, no canto de cima a direita do cartao (coordenadas a partir
// da direita da area, para valer em qualquer largura).
static const int DET_FAV_L = 260, DET_FAV_A = 56, DET_FAV_Y = 14, DET_FAV_DIR = 24;
// EM PE as medidas fixas passam da borda: dois botoes de 300 lado a lado nao
// cabem nos ~630 px que sobram do trilho. Desenho e toque passam pelas mesmas
// funcoes, entao concordam nas duas orientacoes. 'larg' = da coluna x0 ate o
// fim da area.
static int det_btn_l(const area_t& a)
{
  const int larg = a.largura - TRILHO_L;
  const int cabe = (larg - 3 * 24) / 2;
  return cabe < DET_BTN_L ? cabe : DET_BTN_L;
}
static int det_fav_l(const area_t& a) { return em_pe(a) ? 210 : DET_FAV_L; }
static int det_val_dx(const area_t& a) { return em_pe(a) ? 170 : 220; }   // coluna dos valores
static char s_det_assin[512];

static void desenhar_no_detalhe(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  mesh_no_t no;
  const bool achou = no_por_num(s_no_sel, &no);
  const bool meu = (s_no_sel == tab5_mesh_meu_numero());

  enum { R_ID = 0, R_MAC, R_HW, R_SINAL, R_SALTOS, R_BAT, R_VISTO, R_POS, R_TOTAL };
  static const char* const ROT[R_TOTAL] = { "id", "MAC", "modelo", "sinal", "saltos", "bateria", "visto", "posição" };
  char val[R_TOTAL][64];
  char titulo[MESH_NOME_LONGO + 16], curto[MESH_NOME_CURTO * 2] = "";
  for (int k = 0; k < R_TOTAL; ++k) { snprintf(val[k], sizeof(val[k]), "-"); }

  if (!achou) {
    tab5_mesh_nome_exibicao(s_no_sel, titulo, sizeof(titulo));
    snprintf(val[R_ID], sizeof(val[R_ID]), "!%08lx  (saiu da tabela ao vivo)", (unsigned long)s_no_sel);
  } else {
    // Titulo: o longo; sem ele, a regra de exibicao (curto, ou o id).
    char longo[MESH_NOME_LONGO] = "";
    if (no.longo[0]) { snprintf(longo, sizeof(longo), "%s", no.longo); }
    else             { tab5_mesh_nome_longo_cache(no.num, longo, sizeof(longo)); }
    if (longo[0]) { snprintf(titulo, sizeof(titulo), "%s", longo); }
    else          { tab5_mesh_nome_exibicao(no.num, titulo, sizeof(titulo)); }
    if (no.curto[0]) { snprintf(curto, sizeof(curto), "%s", no.curto); }
    else             { tab5_mesh_nome_cache(no.num, curto, sizeof(curto)); }

    snprintf(val[R_ID], sizeof(val[R_ID]), "%s   nº %lu",
             no.id[0] ? no.id : "", (unsigned long)no.num);
    if (!no.id[0]) { snprintf(val[R_ID], sizeof(val[R_ID]), "!%08lx   nº %lu", (unsigned long)no.num, (unsigned long)no.num); }

    // O MAC so quando o no o mandou (User.macaddr). O id e derivado dos 4
    // ultimos bytes dele, mas completar os 2 primeiros seria inventar.
    if (no.tem_mac) {
      snprintf(val[R_MAC], sizeof(val[R_MAC]), "%02X:%02X:%02X:%02X:%02X:%02X",
               no.mac[0], no.mac[1], no.mac[2], no.mac[3], no.mac[4], no.mac[5]);
    } else {
      snprintf(val[R_MAC], sizeof(val[R_MAC]), "não enviado pelo nó");
    }

    const char* hw = mesh_hw_nome(no.hw);
    if (no.hw == 0)  { snprintf(val[R_HW], sizeof(val[R_HW]), "não informado"); }
    else if (hw)     { snprintf(val[R_HW], sizeof(val[R_HW]), "%s", hw); }
    else             { snprintf(val[R_HW], sizeof(val[R_HW]), "modelo %u", (unsigned)no.hw); }

    if (no.snr > -128.0f) { snprintf(val[R_SINAL], sizeof(val[R_SINAL]), "SNR %+.1f dB%s", (double)no.snr, no.via_mqtt ? "  (do gateway)" : ""); }

    if (no.via_mqtt)                                   { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), "pela internet (MQTT)"); }
    else if (no.saltos == 0)                           { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), "0 - vizinho direto"); }
    else if (no.saltos != MESH_SALTOS_DESCONHECIDO)    { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), "%u", (unsigned)no.saltos); }

    char bat[32] = "";
    if (no.bateria > 100)    { snprintf(bat, sizeof(bat), "na tomada"); }
    else if (no.bateria > 0) { snprintf(bat, sizeof(bat), "%u %%", (unsigned)no.bateria); }
    if (no.tensao > 0.1f) {
      const size_t k = strlen(bat);
      snprintf(bat + k, sizeof(bat) - k, "%s%.2f V", k ? "   " : "", (double)no.tensao);
    }
    if (bat[0]) { snprintf(val[R_BAT], sizeof(val[R_BAT]), "%s", bat); }

    char idade[12];
    idade_texto(no.idade_s, idade, sizeof(idade));
    if (no.idade_s < 0)       { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), "não ouvido nesta sessão"); }
    else if (no.idade_s < 90) { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), "agora"); }
    else                      { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), "há %s", idade); }

    if (no.tem_pos) {
      const float lat = no.lat_i / 1e7f, lon = no.lon_i / 1e7f;
      float rlat, rlon; bool ref = false;
      const nmea_posicao_t fix = tab5_gps_posicao();
      if (fix.valida && fix.qualidade > 0) { rlat = (float)fix.lat; rlon = (float)fix.lon; ref = true; }
      else { ref = ajustes_casa(&rlat, &rlon); }
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

  // So repinta quando algum texto mudou: a pagina roda a 1 Hz.
  char assin[sizeof(s_det_assin)];
  snprintf(assin, sizeof(assin), "%s|%s|%d|", titulo, curto, (int)fav);
  for (int k = 0; k < R_TOTAL; ++k) {
    const size_t n = strlen(assin);
    snprintf(assin + n, sizeof(assin) - n, "%s|", val[k]);
  }
  if (!pintura_mudou_s(&s_p, s_det_assin, sizeof(s_det_assin), assin)) { return; }

  tab5_tela_limpar(x0, a.y, a.x + a.largura - x0, a.altura, TFT_BLACK);
  d.setTextDatum(top_left);
  tab5_texto(d, 4.2f);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  // O titulo para antes da estrela (e do chip do curto).
  const int fav_l = det_fav_l(a);
  const int fav_x = a.x + a.largura - DET_FAV_DIR - fav_l;
  const int tit_max = fav_x - (x0 + 24) - 160;
  tab5_cortar_ao_caber(d, titulo, tit_max);
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
    tab5_texto(d, 4.2f);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  if (curto[0]) {
    const int tx = x0 + 24 + (int)d.textWidth(titulo) + 20;
    tab5_botao(d, tx, a.y + 16, 120, 44, curto, TFT_NAVY, TFT_WHITE);
  }
  if (meu) {
    tab5_texto(d, 2.4f);
    d.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    d.drawString("este é o seu nó", x0 + 24, a.y + 62);
  }

  for (int k = 0; k < R_TOTAL; ++k) {
    const int y = a.y + 96 + k * DET_LIN;
    tab5_texto(d, 2.8f);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString(ROT[k], x0 + 24, y + 4);
    // Valor longo (a posicao com distancia, sobretudo em pe): encolhe ate
    // caber e, no limite, corta - nunca passa da borda.
    const int vx = x0 + det_val_dx(a);
    texto_que_cabe(d, val[k], a.x + a.largura - 16 - vx, 3.4f, 2.6f);
    d.setTextColor(k == R_MAC && !(achou && no.tem_mac) ? TFT_DARKGREY : TFT_WHITE, TFT_BLACK);
    d.drawString(val[k], vx, y);
  }

  const int by = a.y + a.altura - DET_BTN_Y_DA_BASE;
  const int bl = det_btn_l(a);
  tab5_botao(d, x0 + 24, by, bl, DET_BTN_A, "VOLTAR", d.color565(50, 55, 65), TFT_WHITE);
  // No da INTERNET nao tem CONVERSAR: a direta sairia pelo nosso radio (ou pela
  // internet, que nao leva direta) e nunca chegaria la. Fala-se com ele no
  // LongFast Internet.
  if (!meu && !(achou && no.rede == MESH_REDE_INTERNET)) {
    tab5_botao(d, x0 + 24 + bl + 24, by, bl, DET_BTN_A, "CONVERSAR", TFT_GREENYELLOW, TFT_BLACK);
  }
}

static void desenhar_nos(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  char linha[NOS_LINHA_CH + 1];
  char visto[8];
  // A grade pela orientacao de agora. Trocar de grade so acontece num giro,
  // e o giro ja vem com passada completa: tudo e remontado abaixo.
  s_grade = em_pe(a) ? COLUNA_PE : COLUNA;

  if (s_no_sel) {
    lista_esconder(&s_l_nos);   // a tabela nao esta na tela: nao rola nem responde
    desenhar_no_detalhe(d, a, x0);
    tab5_fonte_corpo(d);
    return;
  }

  // Os canais mudaram (config nova, conversa apagada/reaberta): a faixa muda
  // de altura e a tabela muda de lugar - passada completa.
  char assin[sizeof(s_chips_assin)];
  canais_assinatura(assin, sizeof(assin));
  if (strcmp(assin, s_chips_assin) != 0) {
    snprintf(s_chips_assin, sizeof(s_chips_assin), "%s", assin);
    if (!TUDO) { pintura_pedir(&s_p); sistema_pedir_redesenho_completo(); return; }
  }

  if (TUDO) {
    tab5_tela_limpar(x0, a.y, a.x + a.largura - x0, a.altura, TFT_BLACK);
    s_chips_altura = desenhar_canais(d, a, x0);
  }

  // EXCECAO A FONT4, DE PROPOSITO: esta tabela monta UMA STRING SO por linha
  // (campo() escreve cada valor num deslocamento de CARACTERE fixo dentro de
  // 'linha', ver COLUNA[].col acima) e desenha a linha inteira de uma vez. Isso
  // SO alinha em colunas se cada caractere tiver a MESMA largura em pixels -
  // exatamente o que a Font0 garante (6px, sempre) e a Font4 NAO (e
  // proporcional). Por isso ela fica na Font0 e so ela neste app.
  d.setFont(&fonts::Font0);

  if (TUDO) {
    // Cabecalho pela MESMA tabela das linhas - e o que garante que ele nunca
    // mais saia do lugar sozinho.
    espacos(linha, sizeof(linha));
    for (int i = 0; i < C_VISTO; ++i) { campo(linha, s_grade[i], s_grade[i].titulo); }
    linha[linha_ch()] = 0;
    d.setTextSize(2);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(linha, x0 + NOS_MARGEM, a.y + s_chips_altura + 8);
    d.drawString(s_grade[C_VISTO].titulo, x0 + visto_x(), a.y + s_chips_altura + 8);
  }
  d.setTextSize(2);
  d.setTextDatum(top_left);

  // CABECALHO FIXO ("sticky"): ele nao rola, so as linhas.
  const int cabecalho_a = s_chips_altura + 36;
  const area_t janela_nos = { x0, a.y + cabecalho_a, a.x + a.largura - x0, a.altura - cabecalho_a };

  // O QUE MUDOU DE VALOR (a idade corre a cada segundo, o SNR muda com o
  // proximo pacote, a ordem muda quando alguem fala): compara com o que esta
  // escrito e marca SO essas linhas. Mudou a CONTAGEM: a lista inteira.
  const int n = tab5_mesh_num_nos_rede(s_rede_vista);
  if (TUDO || n != s_l_nos.n) {
    lista_itens_mudaram(&s_l_nos, n);
  } else {
    for (int i = 0; i < n; ++i) {
      // Linha nunca desenhada (fora da janela): a rolagem a desenha quando
      // ela aparecer. Marca-la por ter o texto vazio arrastaria as visiveis
      // junto (a marca e um intervalo) - ver s_cfg_pintada na pagina "No".
      if (i < MESH_MAX_NOS && !s_nos_ant[i][0]) { continue; }
      mesh_no_t no;
      if (!tab5_mesh_no_rede(s_rede_vista, i, &no)) { continue; }
      montar_linha(linha, sizeof(linha), no);
      char curto[8];
      idade_texto(no.idade_s, curto, sizeof(curto));
      snprintf(visto, sizeof(visto), "%-6.6s", curto);
      if (strcmp(linha, s_nos_ant[i]) != 0 || strcmp(visto, s_nos_visto_ant[i]) != 0
       || no.favorito != s_nos_fav_ant[i]) {
        lista_item_mudou(&s_l_nos, i);
      }
    }
  }
  lista_desenhar(&s_l_nos, d, janela_nos, TFT_BLACK, TUDO);

  tab5_fonte_corpo(d);   // devolve a Font4 - ver a excecao acima
}

static void desenhar_malha(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  if (TUDO) { tab5_tela_limpar(x0, a.y, a.x + a.largura - x0, a.altura, TFT_BLACK); }
  mesh_saude_t s; tab5_mesh_saude(&s);
  char buf[160];

  // "-" QUANDO NAO VEIO. Os numeros de saude so existem com o LocalStats /
  // DeviceMetrics do NOSSO no; no modo internet (sem no) ou antes da primeira
  // telemetria eles sao zero - e "0.0 %" de uso do canal e um valor plausivel,
  // que mentia "malha vazia" quando a verdade era "nao sei".
  //
  // NOS ONLINE sem o LocalStats: contados aqui, com a mesma regra do
  // Meshtastic (ouvido nas ultimas 2 h) - marcados como tal.
  struct { const char* rot; char val[48]; uint16_t cor; } campos[10];
  if (s.tem_local) {
    snprintf(campos[0].val, sizeof(campos[0].val), "%u / %u", (unsigned)s.nos_online, (unsigned)s.nos_total);
    campos[0].rot = "nós online (do rádio)";
  } else {
    int on = 0;
    const int n = tab5_mesh_num_nos_rede(MESH_REDE_NO);   // a saude e da malha daqui
    for (int i = 0; i < n; ++i) {
      mesh_no_t no;
      if (tab5_mesh_no_rede(MESH_REDE_NO, i, &no) && no.idade_s >= 0 && no.idade_s <= 7200) { ++on; }
    }
    snprintf(campos[0].val, sizeof(campos[0].val), "%d / %d", on, n);
    campos[0].rot = "nós online (contados aqui, 2 h)";
  }
  campos[0].cor = TFT_GREENYELLOW;
  const bool tem_uso = s.tem_local || s.tem_metricas;
  if (tem_uso) { snprintf(campos[1].val, sizeof(campos[1].val), "%.1f %%", (double)s.uso_canal); }
  else         { snprintf(campos[1].val, sizeof(campos[1].val), "-"); }
  campos[1].rot = "uso do canal"; campos[1].cor = TFT_SKYBLUE;
  if (tem_uso) { snprintf(campos[2].val, sizeof(campos[2].val), "%.2f %%", (double)s.airtime_tx); }
  else         { snprintf(campos[2].val, sizeof(campos[2].val), "-"); }
  campos[2].rot = "airtime tx"; campos[2].cor = TFT_SKYBLUE;
  if (tem_uso) {
    snprintf(campos[3].val, sizeof(campos[3].val), "%lu h %lu min",
             (unsigned long)(s.uptime_s / 3600), (unsigned long)((s.uptime_s % 3600) / 60));
  } else {
    snprintf(campos[3].val, sizeof(campos[3].val), "-");
  }
  campos[3].rot = "uptime do nó"; campos[3].cor = TFT_WHITE;
  // ENLACE COM A VIA: "no ar" sozinho nao dizia por onde.
  const uint8_t via = tab5_mesh_via();
  const char* nome_via = !tab5_mesh_servico_ativo() ? "parado"
                       : via == AJUSTES_MESH_BLUETOOTH ? "Bluetooth"
                       : via == AJUSTES_MESH_INTERNET  ? "Internet" : "Grove";
  snprintf(campos[4].val, sizeof(campos[4].val), "%s · %s", nome_via, tab5_mesh_no_ar() ? "no ar" : "sem quadro");
  campos[4].rot = "enlace"; campos[4].cor = tab5_mesh_no_ar() ? TFT_GREENYELLOW : TFT_RED;

  // SEGUNDA COLUNA: o que o radio ouviu e o entorno imediato. Os contadores de
  // pacote so existem com o LocalStats ("-" sem ele, como acima).
  if (s.tem_local) {
    snprintf(campos[5].val, sizeof(campos[5].val), "%lu / %lu", (unsigned long)s.pkt_rx, (unsigned long)s.pkt_tx);
    snprintf(campos[6].val, sizeof(campos[6].val), "%lu · %lu", (unsigned long)s.pkt_ruins, (unsigned long)s.pkt_repetidos);
  } else {
    snprintf(campos[5].val, sizeof(campos[5].val), "-");
    snprintf(campos[6].val, sizeof(campos[6].val), "-");
  }
  campos[5].rot = "pacotes recebidos / enviados"; campos[5].cor = TFT_SKYBLUE;
  campos[6].rot = "pacotes ruins · repetidos";    campos[6].cor = TFT_SKYBLUE;
  {
    const uint32_t meu_no = tab5_mesh_meu_numero();
    int vizinhos = 0, mais_recente = -1;
    int bat = -1;
    const int nn = tab5_mesh_num_nos_rede(MESH_REDE_NO);
    for (int i = 0; i < nn; ++i) {
      mesh_no_t no;
      if (!tab5_mesh_no_rede(MESH_REDE_NO, i, &no)) { continue; }
      if (meu_no && no.num == meu_no) { bat = no.bateria; continue; }
      if (no.idade_s < 0) { continue; }
      if (mais_recente < 0 || no.idade_s < mais_recente) { mais_recente = no.idade_s; }
      if (no.saltos == 0 && !no.via_mqtt && no.idade_s <= 7200) { ++vizinhos; }
    }
    snprintf(campos[7].val, sizeof(campos[7].val), "%d", vizinhos);
    campos[7].rot = "vizinhos diretos (2 h)"; campos[7].cor = TFT_GREENYELLOW;
    if (mais_recente >= 0) {
      char t[8]; idade_texto(mais_recente, t, sizeof(t));
      snprintf(campos[8].val, sizeof(campos[8].val), "há %s", t);
    } else {
      snprintf(campos[8].val, sizeof(campos[8].val), "-");
    }
    campos[8].rot = "último nó ouvido"; campos[8].cor = TFT_WHITE;
    if (bat < 0)        { snprintf(campos[9].val, sizeof(campos[9].val), "-"); }
    else if (bat > 100) { snprintf(campos[9].val, sizeof(campos[9].val), "na tomada"); }
    else                { snprintf(campos[9].val, sizeof(campos[9].val), "%d %%", bat); }
    campos[9].rot = "bateria do meu nó"; campos[9].cor = TFT_WHITE;
  }

  // DUAS COLUNAS, passo vertical repartido pela altura que sobra (nunca menor
  // que antes). A largura de cada valor e medida: o que nao couber na coluna
  // cai para a fonte menor em vez de invadir a vizinha.
  //
  // EM PE, UMA COLUNA SO: a metade de ~630 px nao cabe nem o rotulo mais
  // longo, e a altura que sobra da para as dez linhas uma embaixo da outra.
  const int colunas = em_pe(a) ? 1 : 2;
  const int por_col = 10 / colunas;
  const int col_l = (a.x + a.largura - x0 - 48) / colunas;
  int passo = (a.altura - 24 - 40) / por_col;
  if (passo < 88) { passo = 88; }
  if (passo > 120) { passo = 120; }

  // Repinta so o valor que mudou: a pagina roda a 1 Hz.
  static char s_val_ant[10][48];
  for (int i = 0; i < 10; ++i) {
    const int x = x0 + 24 + (i / por_col) * col_l;
    const int y = a.y + 24 + (i % por_col) * passo;
    if (!pintura_mudou_s(&s_p, s_val_ant[i], sizeof(s_val_ant[i]), campos[i].val)) { continue; }
    if (!TUDO) { tab5_tela_limpar(x, y, col_l - 8, passo - 2, TFT_BLACK); }
    tab5_texto(d, 3.12f);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(campos[i].rot, x, y);
    tab5_texto(d, 5.85f);
    if (d.textWidth(campos[i].val) > col_l - 16) { tab5_texto(d, 4.2f); }
    d.setTextColor(campos[i].cor, TFT_BLACK);
    d.drawString(campos[i].val, x, y + 26);
  }

  // O NOSSO NO PELO NOME, com o id ao lado - era so o numero em hexa.
  const uint32_t meu = tab5_mesh_meu_numero();
  char curto[MESH_NOME_CURTO * 2] = "", longo[MESH_NOME_LONGO] = "";
  tab5_mesh_nome_cache(meu, curto, sizeof(curto));
  tab5_mesh_nome_longo_cache(meu, longo, sizeof(longo));
  if (!meu) { snprintf(buf, sizeof(buf), "meu nó: ainda não apresentado"); }
  else {
    snprintf(buf, sizeof(buf), "meu nó: %s%s%s%s  !%08lx   firmware %s",
             longo, (longo[0] && curto[0]) ? " (" : "", curto, (longo[0] && curto[0]) ? ")" : "",
             (unsigned long)meu, tab5_mesh_firmware()[0] ? tab5_mesh_firmware() : "-");
  }
  static char s_rodape_ant[sizeof(buf)];
  if (pintura_mudou_s(&s_p, s_rodape_ant, sizeof(s_rodape_ant), buf)) {
    if (!TUDO) { tab5_tela_limpar(x0, a.y + a.altura - 34, a.x + a.largura - x0, 34, TFT_BLACK); }
    texto_que_cabe(d, buf, a.x + a.largura - 16 - (x0 + 24), 3.12f, 2.4f);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString(buf, x0 + 24, a.y + a.altura - 30);
  }
}


// ROTULO DE REMETENTE, DENTRO do balao (era ACIMA, fora dele
// mudou) - so em conversa de CANAL, onde varias pessoas diferentes
// escrevem no mesmo lugar. Numa conversa direta o lado e a cor do balao ja
// dizem quem mandou; um rotulo ali so repetiria informacao. A ALTURA
// reservada para ele (NOME_A) e medida de verdade em desenhar_msgs() - ver o
// aviso la - nao mais um numero fixo aqui.

// Nome do remetente para o rotulo - ver tab5_mesh_nome_exibicao() no servico:
// shortname primeiro, ID em hexa se o shortname nao prestar (vazio, "?" ou
// emoji) ou o no ainda nao se apresentou. So um wrapper fino aqui; a regra em
// si mora no servico porque a conversa direta (tab5_mesh.cpp) precisa da
// MESMA regra, e duas copias dela e exatamente o tipo de duplicacao que
// tab5_ui.hpp existe para evitar (ver o aviso no topo daquele arquivo).
static void nome_do_no(uint32_t num, char* out, size_t out_tam)
{
  tab5_mesh_nome_exibicao(num, out, out_tam);
}

// COR POR REMETENTE, so para o rotulo de nome em conversa de CANAL - pedido
// depois de ver o app oficial do Meshtastic no celular, que
// pinta cada participante numa cor propria em vez de cinza uniforme igual
// para todo mundo. Hash simples (o proprio numero do no, modulo o tamanho da
// paleta): determinismo e tudo que se precisa aqui - a mesma pessoa sempre
// cai na mesma cor NESTA sessao, e isso basta para o olho aprender a
// distinguir quem escreveu o que numa janela cheia de gente. Nao precisa
// sobreviver a reinicio, e so identificacao visual, nao dado gravado.
static uint16_t cor_do_no(uint32_t num)
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
// Quem compartilha localizacao pelo app do Meshtastic no celular quase
// sempre manda uma mensagem de TEXTO normal com as coordenadas embutidas
// (um link tipo "https://maps.google.com/?q=-12.97,-38.50", ou so o par
// cru) - nao existe um tipo de pacote separado que a conversa precise
// entender. Entao a deteccao e sobre o TEXTO, nao sobre o protocolo.
//
// achar_coord() procura, em QUALQUER lugar da string, um par "numero,
// numero" (com o primeiro cabendo em latitude e o segundo em longitude) -
// serve tanto para o link quanto para o par cru, sem se importar com o
// resto do texto ao redor (URL, legenda, o que vier).
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
    // e o que distingue um par de coordenadas de dois numeros quaisquer
    // separados por virgula que por acaso apareçam num texto.
    if (a < -90.0 || a > 90.0)   { continue; }
    if (b < -180.0 || b > 180.0) { continue; }

    *lat = (float)a;
    *lon = (float)b;
    return true;
  }
  return false;
}

// Distancia (formula de haversine, correta em qualquer escala) e direcao
// (rumo inicial, arredondado para os 8 pontos cardeais) da posicao de
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

  static const char* NOMES[8] = { "N", "NE", "L", "SE", "S", "SO", "O", "NO" };
  *direcao = NOMES[((int)((rumo + 22.5) / 45.0)) % 8];
}

// ---------------------------------------------------------------------------
// A CONVERSA - o componente do sistema (kernel/conversation),
// ---------------------------------------------------------------------------
// Baloes, barra de escrita, teclado virtual e rolagem eram uma copia inteira
// aqui (~700 linhas). O componente nasceu DELES, para o ESP-NOW; agora o Mesh
// volta a usar o original, e uma correcao no teclado ou no balao vale para os
// dois. O que e so do Mesh mora nas tres respostas abaixo: rotulo do remetente
// no canal, "N saltos"/MQTT, coordenada virando distancia e rumo, e as quatro
// marcas de envio.
// Fundo da area de mensagens (o mesmo do componente - trilho e colunas usam).
static uint16_t fundo_msg(lgfx::LovyanGFX& d) { return d.color565(13, 17, 23); }

static float s_ref_lat = 0, s_ref_lon = 0;   // referencia de distancia (GPS ou casa)
static bool  s_tem_ref = false;

static void atualizar_eh_canal(void)
{
  s_eh_canal = s_ab.ok && s_ab.canal;
}

static int conv_contar(void*) { return (s_hist && s_ab.ok && chave_igual(s_hist_chave, s_ab)) ? s_n_hist : 0; }

static bool conv_ler(int i, conversa_msg_t* o, void*)
{
  if (!s_hist || i < 0 || i >= s_n_hist) { return false; }
  const mesh_msg_t* m = &s_hist[i];

  // O componente le em ordem, de 0 em diante: a referencia de distancia vale
  // para a passada inteira (uma leitura do GPS, nao duzentas).
  if (i == 0) {
    const nmea_posicao_t fix = tab5_gps_posicao();
    s_tem_ref = false;
    if (fix.valida && fix.qualidade > 0) { s_ref_lat = (float)fix.lat; s_ref_lon = (float)fix.lon; s_tem_ref = true; }
    else if (ajustes_casa(&s_ref_lat, &s_ref_lon)) { s_tem_ref = true; }
  }

  o->minha  = (m->de == tab5_mesh_meu_numero());
  o->quando = m->quando;

  // Coordenada no texto vira "alfinete, lat, lon, distancia e rumo".
  float lat, lon;
  if (achar_coord(m->texto, &lat, &lon)) {
    if (s_tem_ref) {
      float km; const char* dir;
      distancia_direcao(s_ref_lat, s_ref_lon, lat, lon, &km, &dir);
      snprintf(o->texto, sizeof(o->texto), "\xF0\x9F\x93\x8D %.4f, %.4f  %.1fkm %s",
               (double)lat, (double)lon, (double)km, dir);
    } else {
      snprintf(o->texto, sizeof(o->texto), "\xF0\x9F\x93\x8D %.4f, %.4f", (double)lat, (double)lon);
    }
  } else {
    snprintf(o->texto, sizeof(o->texto), "%s", m->texto);
  }

  // Rotulo so no CANAL (varias pessoas no mesmo lugar), e so na primeira de
  // uma sequencia do mesmo remetente.
  if (s_eh_canal && !o->minha && (i == 0 || s_hist[i - 1].de != m->de)) {
    nome_do_no(m->de, o->nome, sizeof(o->nome));
    o->cor_nome = cor_do_no(m->de);
  }

  if (!o->minha) {
    // Por onde chegou: MQTT (internet) ou quantos saltos de radio. Sem o dado
    // no pacote, o que a tabela de nos sabe do remetente.
    bool via_mqtt = m->via_mqtt;
    uint8_t saltos = m->saltos;
    if (!via_mqtt && saltos == MESH_SALTOS_DESCONHECIDO) {
      bool nm = false; uint8_t ns = MESH_SALTOS_DESCONHECIDO;
      if (tab5_mesh_no_enlace(m->de, &nm, &ns)) { via_mqtt = nm; saltos = ns; }
    }
    if (via_mqtt)                                { snprintf(o->meta, sizeof(o->meta), "MQTT"); }
    else if (saltos != MESH_SALTOS_DESCONHECIDO) { snprintf(o->meta, sizeof(o->meta), "%u salto%s", (unsigned)saltos, saltos == 1 ? "" : "s"); }
    else                                         { snprintf(o->meta, sizeof(o->meta), "--"); }
    o->cor_meta = via_mqtt ? (uint16_t)TFT_SKYBLUE
                : (saltos == MESH_SALTOS_DESCONHECIDO) ? (uint16_t)TFT_DARKGREY
                : (saltos <= 1) ? (uint16_t)TFT_GREENYELLOW
                : (saltos <= 3) ? lgfx::color565(140, 235, 140) : (uint16_t)TFT_ORANGE;
  } else if (m->id != 0) {
    // O estado vivo do envio (ACK chega depois), ou o que ficou gravado.
    uint8_t erro = 0;
    uint8_t est = tab5_mesh_estado_envio(m->id, &erro);
    if (est == MESH_ENVIO_NA) { est = m->envio; }
    o->envio = (est == MESH_ENVIANDO)     ? CONVERSA_ENVIO_PENDENTE
             : (est == MESH_ENTREGUE)     ? CONVERSA_ENVIO_OK
             : (est == MESH_FALHOU)       ? CONVERSA_ENVIO_FALHOU
             : (est == MESH_SEM_RESPOSTA) ? CONVERSA_ENVIO_SEM_RESPOSTA
             : (est == MESH_OUVIDO)       ? CONVERSA_ENVIO_OUVIDO
                                          : CONVERSA_ENVIO_NADA;
  }
  return true;
}

static bool conv_enviar(const char* texto, void*)
{
  // PELA CHAVE: e para quem esta na tela, mesmo que a coluna tenha se
  // reordenado entre o desenho e o toque em ENVIAR.
  if (s_conv < 0 || !s_ab.ok || !tab5_mesh_servico_ativo()) { return false; }
  const bool ok = tab5_mesh_enviar(s_ab.canal ? 0xFFFFFFFFu : s_ab.no, s_ab.canal ? s_ab.idx : 0, texto);
  s_hist_pronto = false;          // rele incluindo a que acabou de sair
  return ok;
}

// A FAIXA DE CIMA DA CONVERSA EM PE: VOLTAR, o tipo (canal ou direta) e o
// nome. Deitado ela nao existe - a coluna ao lado ja diz qual conversa e.
// Repintada so quando o nome muda (ou na passada completa).
static void desenhar_faixa_frente(lgfx::LovyanGFX& d, const area_t& a, const mesh_conversa_t& c)
{
  static char s_ant[MESH_NOME_CONVERSA + MESH_NOME_LONGO + 4];
  char sig[sizeof(s_ant)];
  snprintf(sig, sizeof(sig), "%s|%s", c.nome, c.sub);
  if (!pintura_mudou_s(&s_p, s_ant, sizeof(s_ant), sig)) { return; }

  const uint16_t FUNDO = fundo_msg(d);
  tab5_tela_limpar(a.x, a.y, a.largura, FRENTE_TOPO_A, FUNDO);
  tab5_botao(d, a.x + 16, a.y + FRENTE_VOLTAR_Y, FRENTE_VOLTAR_L, FRENTE_VOLTAR_A,
             "VOLTAR", d.color565(50, 55, 65), TFT_WHITE);
  const int gl = FRENTE_TOPO_A - 24;
  const int gx = a.x + 16 + FRENTE_VOLTAR_L + 20;
  caixa_glifo(d, gx, a.y + 12, gl, gl, c.canal ? GL_DIFUSAO : GL_DIRETO,
              c.canal ? TFT_NAVY : TFT_DARKGREEN, TFT_WHITE);

  // Nome e, embaixo, o longo (ou "canal N") - cortados por caractere ate
  // caber, como na coluna.
  const int tx = gx + gl + 16;
  const int max_l = a.x + a.largura - 16 - tx;
  char nome[MESH_NOME_CONVERSA], sub[MESH_NOME_LONGO];
  snprintf(nome, sizeof(nome), "%s", c.nome);
  snprintf(sub, sizeof(sub), "%s", c.sub);
  d.setTextDatum(top_left);
  tab5_texto(d, 3.84f);
  cortar_ao_caber(d, nome, max_l);
  d.setTextColor(TFT_WHITE, FUNDO);
  d.drawString(nome, tx, a.y + (sub[0] ? 10 : 24));
  if (sub[0]) {
    tab5_texto(d, 2.4f);
    cortar_ao_caber(d, sub, max_l);
    d.setTextColor(TFT_DARKGREY, FUNDO);
    d.drawString(sub, tx, a.y + 50);
  }
  d.drawFastHLine(a.x, a.y + FRENTE_TOPO_A - 1, a.largura, SIS_BORDA);
}

static void desenhar_msgs(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  const int larg = a.x + a.largura - x0;
  if (s_conv < 0) {
    conversa_fechar_teclado(&s_c);
    lista_esconder(&s_c.lista);   // sem conversa, nada de baloes para rolar
    if (TUDO) {
      const uint16_t FUNDO = fundo_msg(d);
      tab5_tela_limpar(x0, a.y, larg, a.altura, FUNDO);
      tab5_texto(d, 3.4f);
      d.setTextDatum(middle_center);
      d.setTextColor(TFT_LIGHTGREY, FUNDO);
      d.drawString("escolha uma conversa", x0 + larg / 2, a.y + a.altura / 2);
      d.setTextDatum(top_left);
    }
    return;
  }
  mesh_conversa_t c = {};
  if (tab5_mesh_conversa(s_conv, &c) && c.nao_lidas > 0) { tab5_mesh_marcar_conversa_lida(s_conv); }
  if (!s_hist_pronto) { carregar_hist(); }

  // Em pe a conversa esta POR CIMA da coluna: a faixa do VOLTAR ocupa o topo.
  int topo = 0;
  if (em_pe(a)) {
    topo = FRENTE_TOPO_A;
    desenhar_faixa_frente(d, a, c);
  }

  // A conversa e a coluna da direita (deitado) ou a area inteira abaixo da
  // faixa (em pe); o teclado, a largura inteira da area nos dois casos.
  const area_t r = { x0, a.y + topo, larg, a.altura - topo };
  conversa_area_teclado(&s_c, a);
  if (c.canal && mesh_canal_internet(c.idx)) {
    static char motivo[96];
    snprintf(motivo, sizeof(motivo), "rede internet %s - só consulta", tab5_mesh_rede_internet_estado());
    conversa_desenhar(&s_c, d, r, TUDO, tab5_mesh_servico_ativo() && mesh_mqtt_internet_conectado(), motivo);
  } else {
    conversa_desenhar(&s_c, d, r, TUDO, tab5_mesh_servico_ativo(),
                      "sem nó Meshtastic - só consulta");
  }
}


// ---------------------------------------------------------------------------
// PAGINA "NO" - configurar o no Meshtastic daqui (services/tab5_mesh_admin)
// ---------------------------------------------------------------------------
// Cada linha leva um icone do que ela faz (icone_cfg). Tocar NAO grava: muda o
// valor PENDENTE (amarelo, com um ponto). "GRAVAR NO NO" manda tudo numa
// transacao so - o no grava e reinicia UMA vez, por mais opcoes que mudem.
// Gravar a cada toque reiniciaria o no a cada degrau de um ciclo (3, 4, 5...).
//
// Depois de gravar, a tela mostra o que o NO confirmou no dump seguinte, nunca
// o que pedimos: se ele recusar algo, o valor antigo continua aparecendo.
//
// Acoes imediatas (sem reinicio): som das notificacoes e GPS do Tab5 para o no
// (os dois do Tab5), posicao fixa pelo GPS do Tab5, acertar a hora do no.
// Reiniciar pede confirmacao.
//
// EM GRUPOS. A pagina abre na lista dos GRUPOS (Nome e papel, Localizacao,
// Radio, Conexoes, Avisos, Manutencao), cada um com um resumo do que tem
// dentro; tocar abre o grupo com as linhas dele juntas, e VOLTAR sobe. O
// pendente vale para a pagina toda: mudar algo em dois grupos e gravar uma vez
// so continua sendo UMA transacao (o rodape aparece nos dois niveis).
enum {
  RC_SOM = 0, RC_LONGO, RC_CURTO, RC_PAPEL, RC_SALTOS, RC_TX,
  RC_LOC_PRECISAO, RC_LOC_TAB5, RC_POS_S, RC_POS_INTELIGENTE, RC_GPS_NO, RC_POS_FIXA,
  RC_NODEINFO_S, RC_TELE_S, RC_MQTT, RC_PROXY, RC_BT, RC_WIFI, RC_HORA, RC_REINICIAR,
  RC_I_FIRMWARE, RC_I_REGIAO, RC_I_NOS, RC_I_REINICIOS, RC_I_AVISO,
  RC_TOTAL
};
static const int RC_EDITAVEIS = RC_I_FIRMWARE;   // daqui para baixo, so leitura
// Linhas que sao do TAB5, nao do no: mudam na hora, com ou sem o no.
static bool rc_do_tab5(int i) { return i == RC_SOM || i == RC_LOC_TAB5; }

enum { G_NOME = 0, G_LOCAL, G_RADIO, G_CONEXOES, G_AVISOS, G_MANUT, G_TOTAL };
typedef struct { const char* nome; int8_t icone; int8_t n; int8_t rc[6]; } grupo_cfg_t;
static const grupo_cfg_t GRUPOS[G_TOTAL] = {
  { "Nome e papel",        RC_LONGO,     4, { RC_LONGO, RC_CURTO, RC_PAPEL, RC_NODEINFO_S } },
  { "Localização",         RC_POS_S,     6, { RC_LOC_PRECISAO, RC_LOC_TAB5, RC_POS_S, RC_POS_INTELIGENTE,
                                                RC_GPS_NO, RC_POS_FIXA } },
  { "Rádio LoRa",          RC_TX,        3, { RC_I_REGIAO, RC_SALTOS, RC_TX } },
  { "Conexões e internet", RC_WIFI,      4, { RC_BT, RC_WIFI, RC_MQTT, RC_PROXY } },
  { "Avisos e telemetria", RC_SOM,       3, { RC_SOM, RC_TELE_S, RC_I_AVISO } },
  { "Manutenção",          RC_REINICIAR, 5, { RC_HORA, RC_REINICIAR, RC_I_FIRMWARE, RC_I_NOS, RC_I_REINICIOS } },
};
static int s_grupo = -1;   // -1 = a lista dos grupos
static int cfg_n_itens(void) { return s_grupo < 0 ? G_TOTAL : GRUPOS[s_grupo].n; }
// Modos de edicao de texto do Wi-Fi (s_editando): fora da numeracao das linhas.
enum { ED_WIFI_SSID = 100, ED_WIFI_SENHA = 101 };

static const int CFG_LIN   = 68;
static const int CFG_TOPO  = 80;     // cabecalho da pagina (o VOLTAR dos grupos cabe nele)
static const int CFG_RODAPE = 88;    // faixa dos botoes GRAVAR / DESCARTAR
static const int CFG_BTN_L = 360, CFG_BTN_A = 64;
// Largura dos botoes GRAVAR/DESCARTAR: 360 deitado; em pe, o que couber lado
// a lado. Desenho e toque usam esta mesma funcao.
static int cfg_btn_l(const area_t& a)
{
  const int cabe = (a.largura - TRILHO_L - 3 * 24) / 2;
  return cabe < CFG_BTN_L ? cabe : CFG_BTN_L;
}

static lista_t            s_l_cfg;
static mesh_cfg_t         s_cfg;            // o que o no confirmou (copia por passada)
static mesh_cfg_mudanca_t s_mud;            // o que esta pendente
static char               s_cfg_ant[RC_TOTAL][112];
// A linha ja foi desenhada ALGUMA vez desde a ultima passada completa. Sem
// isto, uma linha fora da janela tinha assinatura vazia, "mudava" em toda
// passada, e a marca dela (lista_item_mudou marca um INTERVALO, do primeiro
// ao ultimo item) arrastava junto todas as linhas visiveis entre elas: cada
// amostra do arraste redesenhava a janela inteira por cima do que a rolagem
// acabara de deslocar - e o que sobrava disso eram rastros nas bordas.
static bool               s_cfg_pintada[RC_TOTAL];
static int                s_editando = 0;   // 0 = nada; RC_LONGO ou RC_CURTO
static char               s_edit_txt[66];   // o maior campo: senha do Wi-Fi (64)
static tecvirt_t          s_kb_cfg;
// DIARIO DE REINICIOS: sub-pagina cheia aberta tocando em "reinicios do no".
static bool               s_diario = false;
// REDES DO WI-FI DO NO: sub-pagina com as redes ja usadas, para escolher.
static bool               s_redes_pg = false;
static char               s_redes_msg[72];
static mesh_reinicio_t    s_rei[MESH_DIARIO_MAX];
static int                s_rei_n = 0;
static uint32_t           s_limpar_ms = 0;      // 1o toque em LIMPAR: confirma em 4 s
static uint32_t           s_reiniciar_ms = 0;   // 1o toque em REINICIAR: confirma em 4 s
static char               s_cfg_msg[64];        // resposta curta da ultima acao


typedef struct { int32_t v; const char* nome; } opcao_t;
#define N_OPC(x) ((int)(sizeof(x) / sizeof((x)[0])))

// Papeis oferecidos. REPEATER e ROUTER_CLIENT ficam de fora: obsoletos no
// firmware atual. Os outros nao fazem sentido para um no ligado a uma tela.
static const opcao_t OPC_PAPEL[] = {
  { 0, "cliente" }, { 1, "cliente mudo (não repete)" }, { 8, "cliente oculto" },
  { 5, "rastreador" }, { 6, "sensor" }, { 2, "roteador" }, { 11, "roteador tardio" },
};
static const opcao_t OPC_SALTOS[] = { {1,"1"},{2,"2"},{3,"3"},{4,"4"},{5,"5"},{6,"6"},{7,"7"} };
static const opcao_t OPC_TX[] = {
  { 0, "máximo da região" }, { 10, "10 dBm" }, { 14, "14 dBm" }, { 17, "17 dBm" },
  { 20, "20 dBm" }, { 22, "22 dBm" }, { 27, "27 dBm" }, { 30, "30 dBm" },
};
// LOCALIZACAO NA MALHA = precisao do canal primario (ver mesh_cfg_t.precisao).
static const opcao_t OPC_PRECISAO[] = {
  { 0, "não compartilhar" }, { 32, "exata" }, { 18, "aproximada (~90 m)" },
  { 16, "aproximada (~360 m)" }, { 14, "aproximada (~1,5 km)" }, { 12, "só a cidade (~6 km)" },
};
// De quantos em quantos minutos o GPS do Tab5 vai para o no (settings.h).
static const opcao_t OPC_TAB5[] = {
  { 0, "não usar" }, { 5, "a cada 5 min" }, { 15, "a cada 15 min" }, { 30, "a cada 30 min" }, { 60, "a cada 1 h" },
};
static const opcao_t OPC_POS_S[] = {
  { 0, "padrão (15 min)" }, { 300, "5 min" }, { 900, "15 min" }, { 1800, "30 min" },
  { 3600, "1 h" }, { 21600, "6 h" }, { 43200, "12 h" },
};
static const opcao_t OPC_NODEINFO_S[] = {
  { 0, "padrão (3 h)" }, { 3600, "1 h" }, { 10800, "3 h" }, { 21600, "6 h" }, { 43200, "12 h" }, { 86400, "24 h" },
};
static const opcao_t OPC_TELE_S[] = {
  { 0, "padrão (30 min)" }, { 900, "15 min" }, { 1800, "30 min" }, { 3600, "1 h" }, { 7200, "2 h" }, { 21600, "6 h" },
};

static const char* opc_nome(const opcao_t* o, int n, int32_t v, char* buf, size_t tam, const char* fmt)
{
  for (int i = 0; i < n; ++i) { if (o[i].v == v) { return o[i].nome; } }
  snprintf(buf, tam, fmt, (long)v);   // valor que nao esta na lista (posto pelo cliente web)
  return buf;
}

static int32_t opc_proximo(const opcao_t* o, int n, int32_t v)
{
  for (int i = 0; i < n; ++i) { if (o[i].v == v) { return o[(i + 1) % n].v; } }
  return o[0].v;
}

// Precisao fora da lista (posta pelo app do celular): a distancia dela. Cada
// bit a menos dobra a celula; 10 bits ~ 23 km.
static const char* precisao_nome(int32_t p, char* buf, size_t tam)
{
  for (int i = 0; i < N_OPC(OPC_PRECISAO); ++i) { if (OPC_PRECISAO[i].v == p) { return OPC_PRECISAO[i].nome; } }
  if (p < 0)            { return "-"; }
  if (p >= 32 || p < 1) { return p ? "exata" : "não compartilhar"; }
  const uint32_t m = p >= 10 ? (23300u >> (p - 10)) : (23300u << (10 - p));
  if (m >= 1000) { snprintf(buf, tam, "aproximada (~%lu km)", (unsigned long)((m + 500) / 1000)); }
  else           { snprintf(buf, tam, "aproximada (~%lu m)", (unsigned long)m); }
  return buf;
}

static const char* nome_regiao(int r)
{
  static const char* const N[] = {
    "não definida", "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW", "RU", "IN",
    "NZ_865", "TH", "LORA_24", "UA_433", "UA_868", "MY_433", "MY_919", "SG_923", "PH_433",
    "PH_868", "PH_915", "ANZ_433", "KZ_433", "KZ_863", "NP_865", "BR_902",
  };
  return (r >= 0 && r < N_OPC(N)) ? N[r] : "outra";
}

static const char* nome_preset(int p)
{
  static const char* const N[] = {
    "LongFast", "LongSlow", "VeryLongSlow", "MediumSlow", "MediumFast", "ShortSlow",
    "ShortFast", "LongModerate", "ShortTurbo", "LongTurbo",
  };
  return (p >= 0 && p < N_OPC(N)) ? N[p] : "outro";
}

// Qual campo do servico cada linha edita (-1 = nenhum).
static int campo_da_linha(int i)
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

static int32_t valor_atual(int campo)
{
  return s_mud.muda[campo] ? s_mud.v[campo] : s_cfg.v[campo];
}

static bool ha_pendente(void)
{
  for (int k = 0; k < MESH_CFG_N; ++k) { if (s_mud.muda[k]) { return true; } }
  return s_mud.longo[0] || s_mud.curto[0] || s_mud.wifi_muda || s_mud.wifi_ssid[0] || s_mud.wifi_psk[0]
      || s_mud.precisao_muda;
}

// Rotulo e valor de uma linha. 'pend' = valor pendente (ainda nao gravado).
static void linha_cfg(int i, char* rot, size_t tr, char* val, size_t tv, bool* pend)
{
  char tmp[40];
  *pend = false;
  if (tv) { val[0] = 0; }
  const uint32_t meu = tab5_mesh_meu_numero();
  const int c = campo_da_linha(i);
  if (c >= 0) {
    *pend = s_mud.muda[c];
    if (!s_cfg.conhecido[c] && !*pend) { snprintf(val, tv, "-"); }
  }
  const int32_t v = (c >= 0) ? valor_atual(c) : 0;
  const bool conhecido = (c < 0) || s_cfg.conhecido[c] || *pend;

  switch (i) {
    case RC_SOM:
      snprintf(rot, tr, "som das notificações");
      snprintf(val, tv, "%s", tab5_mesh_beep_ligado() ? "ligado" : "mudo");
      break;
    case RC_LONGO: case RC_CURTO: {
      const bool longo = (i == RC_LONGO);
      snprintf(rot, tr, longo ? "nome longo" : "nome curto");
      const char* pendente = longo ? s_mud.longo : s_mud.curto;
      if (pendente[0]) { *pend = true; snprintf(val, tv, "%s", pendente); break; }
      char n[MESH_NOME_LONGO] = "";
      if (longo) { tab5_mesh_nome_longo_cache(meu, n, sizeof(n)); } else { tab5_mesh_nome_cache(meu, n, sizeof(n)); }
      snprintf(val, tv, "%s", n[0] ? n : "-");
      break;
    }
    case RC_PAPEL:
      snprintf(rot, tr, "papel na malha");
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_PAPEL, N_OPC(OPC_PAPEL), v, tmp, sizeof(tmp), "papel %ld")); }
      break;
    case RC_SALTOS:
      snprintf(rot, tr, "saltos (hop limit)");
      if (conhecido) { if (v == 0) { snprintf(val, tv, "3 (padrão)"); } else { snprintf(val, tv, "%ld", (long)v); } }
      break;
    case RC_TX:
      snprintf(rot, tr, "potência de transmissão");
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_TX, N_OPC(OPC_TX), v, tmp, sizeof(tmp), "%ld dBm")); }
      break;
    case RC_LOC_PRECISAO: {
      // LIGA/DESLIGA a localizacao: e a precisao do canal primario. Com 0 o
      // no nao manda posicao nenhuma - as linhas de baixo ficam sem efeito.
      snprintf(rot, tr, "compartilhar a localização");
      *pend = s_mud.precisao_muda;
      snprintf(val, tv, "%s", precisao_nome(*pend ? s_mud.precisao : s_cfg.precisao, tmp, sizeof(tmp)));
      break;
    }
    case RC_LOC_TAB5: {
      // DO TAB5 (settings.h), muda na hora. So vai para o no; e o no que
      // difunde, no ritmo de "enviar a cada" (ver enviar_posicao).
      snprintf(rot, tr, "GPS do Tab5 para o nó");
      const uint8_t m = ajustes_mesh_posicao_min();
      const char* n = opc_nome(OPC_TAB5, N_OPC(OPC_TAB5), m, tmp, sizeof(tmp), "a cada %ld min");
      const bool fixa = s_cfg.pos_fixa;
      const bool gps_no = s_cfg.conhecido[MESH_CFG_GPS_MODO] && s_cfg.v[MESH_CFG_GPS_MODO] == 1;
      if (m && (fixa || gps_no)) { snprintf(val, tv, "parado: o nó usa %s", fixa ? "a posição fixa" : "o GPS dele"); }
      else                       { snprintf(val, tv, "%s", n); }
      break;
    }
    case RC_POS_S:
      snprintf(rot, tr, "enviar a posição a cada");
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_POS_S, N_OPC(OPC_POS_S), v, tmp, sizeof(tmp), "%ld s")); }
      break;
    case RC_POS_INTELIGENTE:
      snprintf(rot, tr, "enviar também ao se mover");
      if (conhecido) { snprintf(val, tv, "%s", v ? "ligado" : "desligado"); }
      break;
    case RC_GPS_NO:
      snprintf(rot, tr, "GPS do próprio nó");
      if (conhecido) {
        if (v == 2)      { snprintf(val, tv, "a placa não tem GPS"); }
        else             { snprintf(val, tv, "%s", v == 1 ? "ligado" : "desligado"); }
      }
      break;
    case RC_POS_FIXA: {
      snprintf(rot, tr, "posição fixa do nó");
      const nmea_posicao_t g = tab5_gps_posicao();
      const bool fix = g.valida && g.qualidade > 0;
      if (s_cfg.pos_fixa) { snprintf(val, tv, "fixa - tocar para soltar"); }
      else if (fix)       { snprintf(val, tv, "tocar para fixar no GPS do Tab5"); }
      else                { snprintf(val, tv, "GPS do Tab5 sem sinal"); }
      break;
    }
    case RC_NODEINFO_S:
      snprintf(rot, tr, "apresentar o nó a cada");
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_NODEINFO_S, N_OPC(OPC_NODEINFO_S), v, tmp, sizeof(tmp), "%ld s")); }
      break;
    case RC_TELE_S:
      snprintf(rot, tr, "telemetria a cada");
      if (conhecido) { snprintf(val, tv, "%s", opc_nome(OPC_TELE_S, N_OPC(OPC_TELE_S), v, tmp, sizeof(tmp), "%ld s")); }
      break;
    case RC_MQTT:
      snprintf(rot, tr, "MQTT do nó");
      if (conhecido) { snprintf(val, tv, "%s", v ? "ligado" : "desligado"); }
      break;
    case RC_PROXY:
      // INTERNET DO TAB5 PARA O NO (MQTT Client Proxy). O campo e do no
      // (proxy_to_client_enabled); com ele ligado e confirmado, o servico do
      // Tab5 leva ao servidor o que o no publica. Ao lado, como esta a ponte.
      snprintf(rot, tr, "internet do Tab5 (proxy)");
      if (conhecido) {
        if (v && !*pend) { snprintf(val, tv, "ligado  ·  %s", tab5_mesh_proxy_estado()); }
        else             { snprintf(val, tv, "%s", v ? "ligado" : "desligado"); }
      }
      break;
    case RC_BT:
      snprintf(rot, tr, "Bluetooth do nó");
      if (conhecido) {
        snprintf(val, tv, "%s%s", v ? "ligado" : "desligado",
                 tab5_mesh_via() == AJUSTES_MESH_BLUETOOTH ? " (em uso pelo Tab5)" : "");
      }
      break;
    case RC_WIFI: {
      snprintf(rot, tr, "Wi-Fi do nó");
      if (s_mud.wifi_ssid[0]) { *pend = true; snprintf(val, tv, "ligar em %s", s_mud.wifi_ssid); }
      else if (s_mud.wifi_muda) { *pend = true; snprintf(val, tv, "%s", s_mud.wifi_liga ? "ligar" : "desligar"); }
      else if (!s_cfg.wifi_conhecido) { snprintf(val, tv, "-"); }
      else if (!s_cfg.wifi_ligado) { snprintf(val, tv, "desligado"); }
      else { snprintf(val, tv, "ligado%s%s", s_cfg.wifi_ssid[0] ? "  ·  " : "", s_cfg.wifi_ssid); }
      break;
    }
    case RC_HORA:
      snprintf(rot, tr, "relógio do nó");
      snprintf(val, tv, "tocar para acertar com o Tab5");
      break;
    case RC_REINICIAR:
      snprintf(rot, tr, "reiniciar o nó");
      snprintf(val, tv, (s_reiniciar_ms && (tab5_agora_ms() - s_reiniciar_ms) < 4000) ? "toque de novo para confirmar" : "tocar para reiniciar");
      break;
    case RC_I_FIRMWARE: {
      snprintf(rot, tr, "firmware / placa");
      const char* hw = mesh_hw_nome(s_cfg.hw);
      snprintf(val, tv, "%s / %s", s_cfg.firmware[0] ? s_cfg.firmware : "-", hw ? hw : "-");
      break;
    }
    case RC_I_REGIAO:
      snprintf(rot, tr, "região / modem");
      if (s_cfg.regiao >= 0) {
        snprintf(val, tv, "%s / %s", nome_regiao(s_cfg.regiao), s_cfg.usa_preset ? nome_preset(s_cfg.preset) : "personalizado");
      } else { snprintf(val, tv, "-"); }
      break;
    case RC_I_NOS:
      snprintf(rot, tr, "nós guardados");
      if (s_cfg.nos_no_radio >= 0) { snprintf(val, tv, "rádio %ld  ·  Tab5 %d", (long)s_cfg.nos_no_radio, tab5_mesh_num_nos()); }
      else                         { snprintf(val, tv, "Tab5 %d  (o firmware do rádio não informa)", tab5_mesh_num_nos()); }
      break;
    case RC_I_REINICIOS:
      snprintf(rot, tr, "reinícios do nó");
      snprintf(val, tv, "%lu  ·  tocar para ver o diário", (unsigned long)s_cfg.reinicios);
      break;
    case RC_I_AVISO:
      snprintf(rot, tr, "último aviso do nó");
      snprintf(val, tv, "%s", s_cfg.aviso[0] ? s_cfg.aviso : "-");
      break;
  }
}

// ---- icones das linhas da configuracao --------------------------------------
// Cada linha leva um desenho ligado ao que ela faz, no lugar da engrenagem
// unica. Cabem em ~28 px em volta de (cx, cy); tudo com primitivas, sem
// imagem guardada. 'cor' e a tinta; o furo/fundo e sempre preto.
static void icone_cfg(lgfx::LovyanGFX& d, int i, int cx, int cy, uint16_t cor)
{
  const uint16_t F = TFT_BLACK;
  switch (i) {
    case RC_SOM:        // alto-falante com ondas
      d.fillRect(cx - 11, cy - 4, 6, 8, cor);
      d.fillTriangle(cx - 5, cy - 4, cx + 2, cy - 11, cx + 2, cy + 11, cor);
      d.fillTriangle(cx - 5, cy + 4, cx + 2, cy - 11, cx + 2, cy + 11, cor);
      d.fillArc(cx + 3, cy, 7, 5, -45, 45, cor);
      d.fillArc(cx + 3, cy, 12, 10, -45, 45, cor);
      break;
    case RC_LONGO: case RC_CURTO: {   // crachá com nome: longo = duas linhas, curto = uma
      d.drawRoundRect(cx - 13, cy - 9, 26, 18, 3, cor);
      d.drawRoundRect(cx - 12, cy - 8, 24, 16, 3, cor);
      d.fillCircle(cx - 7, cy - 2, 3, cor);
      d.fillRect(cx - 10, cy + 2, 7, 3, cor);
      d.fillRect(cx - 1, cy - 4, 10, 2, cor);
      if (i == RC_LONGO) { d.fillRect(cx - 1, cy + 1, 10, 2, cor); }
      break;
    }
    case RC_PAPEL:      // pessoa
      d.fillCircle(cx, cy - 5, 5, cor);
      d.fillArc(cx, cy + 12, 12, 0, 180, 360, cor);
      break;
    case RC_SALTOS:     // tres nos em cadeia
      d.drawLine(cx - 10, cy + 6, cx, cy - 6, cor);
      d.drawLine(cx, cy - 6, cx + 10, cy + 6, cor);
      d.fillCircle(cx - 10, cy + 6, 4, cor);
      d.fillCircle(cx, cy - 6, 4, cor);
      d.fillCircle(cx + 10, cy + 6, 4, cor);
      break;
    case RC_TX:         // antena com ondas
      d.fillTriangle(cx - 5, cy + 12, cx + 5, cy + 12, cx, cy - 2, cor);
      d.fillCircle(cx, cy - 5, 3, cor);
      d.fillArc(cx, cy - 5, 9, 7, -40, 40, cor);
      d.fillArc(cx, cy - 5, 9, 7, 140, 220, cor);
      d.fillArc(cx, cy - 5, 14, 12, -40, 40, cor);
      d.fillArc(cx, cy - 5, 14, 12, 140, 220, cor);
      break;
    case RC_POS_S: case RC_POS_FIXA: case RC_LOC_PRECISAO: case RC_POS_INTELIGENTE:
      // alfinete de mapa; o fixo leva a base, o "ao se mover" um rastro
      d.fillCircle(cx, cy - 4, 8, cor);
      d.fillTriangle(cx - 7, cy, cx + 7, cy, cx, cy + 10, cor);
      d.fillCircle(cx, cy - 4, 3, F);
      if (i == RC_POS_FIXA) { d.fillRect(cx - 9, cy + 11, 18, 3, cor); }
      if (i == RC_POS_INTELIGENTE) { d.fillRect(cx - 14, cy + 11, 4, 3, cor); d.fillRect(cx - 7, cy + 11, 4, 3, cor); }
      if (i == RC_LOC_PRECISAO) { d.drawCircle(cx, cy - 4, 12, cor); }
      break;
    case RC_LOC_TAB5:   // o tablet com o alfinete na tela
      d.drawRoundRect(cx - 12, cy - 11, 24, 22, 3, cor);
      d.drawRoundRect(cx - 11, cy - 10, 22, 20, 3, cor);
      d.fillCircle(cx, cy - 2, 4, cor);
      d.fillTriangle(cx - 4, cy, cx + 4, cy, cx, cy + 6, cor);
      break;
    case RC_GPS_NO:     // mira de GPS
      d.drawCircle(cx, cy, 9, cor);
      d.drawCircle(cx, cy, 8, cor);
      d.fillCircle(cx, cy, 3, cor);
      d.fillRect(cx - 1, cy - 14, 3, 6, cor);
      d.fillRect(cx - 1, cy + 9, 3, 6, cor);
      d.fillRect(cx - 14, cy - 1, 6, 3, cor);
      d.fillRect(cx + 9, cy - 1, 6, 3, cor);
      break;
    case RC_NODEINFO_S: // cartao de identificacao
      d.drawRoundRect(cx - 13, cy - 9, 26, 18, 3, cor);
      d.drawRoundRect(cx - 12, cy - 8, 24, 16, 3, cor);
      d.fillCircle(cx - 6, cy - 2, 3, cor);
      d.fillArc(cx - 6, cy + 8, 6, 0, 180, 360, cor);
      d.fillRect(cx + 1, cy - 4, 9, 2, cor);
      d.fillRect(cx + 1, cy + 1, 9, 2, cor);
      break;
    case RC_TELE_S:     // barras de grafico
      d.fillRect(cx - 11, cy + 2, 6, 9, cor);
      d.fillRect(cx - 3, cy - 4, 6, 15, cor);
      d.fillRect(cx + 5, cy - 11, 6, 22, cor);
      break;
    case RC_MQTT:       // nuvem
      d.fillCircle(cx - 7, cy + 2, 6, cor);
      d.fillCircle(cx + 1, cy - 3, 8, cor);
      d.fillCircle(cx + 8, cy + 3, 5, cor);
      d.fillRect(cx - 7, cy + 2, 16, 7, cor);
      break;
    case RC_PROXY:      // nuvem pequena com seta nos dois sentidos: a ponte
      d.fillCircle(cx - 6, cy - 4, 5, cor);
      d.fillCircle(cx + 1, cy - 8, 6, cor);
      d.fillCircle(cx + 7, cy - 3, 4, cor);
      d.fillRect(cx - 6, cy - 4, 13, 5, cor);
      d.fillRect(cx - 1, cy + 3, 3, 9, cor);
      d.fillTriangle(cx - 5, cy + 6, cx + 5, cy + 6, cx, cy + 1, cor);
      d.fillTriangle(cx - 5, cy + 9, cx + 5, cy + 9, cx, cy + 14, cor);
      break;
    case RC_BT: {       // runa do Bluetooth
      const int t = cy - 12, b = cy + 12, dx = 7, q = 6;
      for (int k = -1; k <= 1; ++k) {
        d.drawLine(cx + k, t, cx + k, b, cor);
        d.drawLine(cx + k, t, cx + dx + k, t + q, cor);
        d.drawLine(cx + dx + k, t + q, cx - dx + k, cy + q, cor);
        d.drawLine(cx + k, b, cx + dx + k, b - q, cor);
        d.drawLine(cx + dx + k, b - q, cx - dx + k, cy - q, cor);
      }
      break;
    }
    case RC_WIFI:       // arcos de Wi-Fi
      for (int k = 0; k < 3; ++k) { const int r = 7 + 6 * k; d.fillArc(cx, cy + 9, r, r - 3, 225, 315, cor); }
      d.fillCircle(cx, cy + 8, 2, cor);
      break;
    case RC_HORA:       // relogio
      d.drawCircle(cx, cy, 12, cor);
      d.drawCircle(cx, cy, 11, cor);
      d.drawLine(cx, cy, cx, cy - 8, cor);
      d.drawLine(cx, cy, cx + 6, cy + 3, cor);
      d.fillCircle(cx, cy, 2, cor);
      break;
    case RC_REINICIAR: case RC_I_REINICIOS:   // seta circular
      d.fillArc(cx, cy, 11, 8, 40, 330, cor);
      d.fillTriangle(cx + 14, cy - 7, cx + 3, cy - 9, cx + 10, cy + 2, cor);
      break;
    case RC_I_FIRMWARE: // chip
      d.fillRoundRect(cx - 8, cy - 8, 16, 16, 3, cor);
      d.fillRect(cx - 4, cy - 4, 8, 8, F);
      for (int k = -1; k <= 1; ++k) {
        d.fillRect(cx - 12, cy + k * 5 - 1, 4, 2, cor);
        d.fillRect(cx + 8,  cy + k * 5 - 1, 4, 2, cor);
        d.fillRect(cx + k * 5 - 1, cy - 12, 2, 4, cor);
        d.fillRect(cx + k * 5 - 1, cy + 8,  2, 4, cor);
      }
      break;
    case RC_I_REGIAO:   // globo
      d.drawCircle(cx, cy, 12, cor);
      d.drawCircle(cx, cy, 11, cor);
      d.drawEllipse(cx, cy, 5, 12, cor);
      d.drawFastHLine(cx - 12, cy, 25, cor);
      d.drawFastHLine(cx - 10, cy - 6, 21, cor);
      d.drawFastHLine(cx - 10, cy + 6, 21, cor);
      break;
    case RC_I_NOS:      // rede de nos
      d.drawLine(cx, cy, cx - 10, cy - 8, cor);
      d.drawLine(cx, cy, cx + 10, cy - 8, cor);
      d.drawLine(cx, cy, cx, cy + 11, cor);
      d.fillCircle(cx, cy, 4, cor);
      d.fillCircle(cx - 10, cy - 8, 3, cor);
      d.fillCircle(cx + 10, cy - 8, 3, cor);
      d.fillCircle(cx, cy + 11, 3, cor);
      break;
    case RC_I_AVISO:    // triangulo de aviso
      d.fillTriangle(cx, cy - 12, cx - 13, cy + 10, cx + 13, cy + 10, cor);
      d.fillRect(cx - 1, cy - 4, 3, 8, F);
      d.fillRect(cx - 1, cy + 6, 3, 2, F);
      break;
    default:
      d.fillCircle(cx, cy, 4, cor);
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

static int cfg_altura(int, void*) { return CFG_LIN; }

// RESUMO DE UM GRUPO, na lista de cima: o essencial do que esta dentro, para
// nao precisar abrir so para conferir. 'pend' = algo pendente la dentro.
static void linha_grupo(int g, char* rot, size_t tr, char* val, size_t tv, bool* pend)
{
  char r2[48], v2[96], tmp[40];
  bool p2 = false;
  *pend = false;
  for (int k = 0; k < GRUPOS[g].n; ++k) {
    linha_cfg(GRUPOS[g].rc[k], r2, sizeof(r2), v2, sizeof(v2), &p2);
    *pend |= p2;
  }
  snprintf(rot, tr, "%s", GRUPOS[g].nome);
  const bool pronto = s_cfg.conhecido[MESH_CFG_PAPEL];
  switch (g) {
    case G_NOME: {
      char n[MESH_NOME_LONGO] = "";
      tab5_mesh_nome_longo_cache(tab5_mesh_meu_numero(), n, sizeof(n));
      if (s_mud.longo[0]) { snprintf(n, sizeof(n), "%s", s_mud.longo); }
      linha_cfg(RC_PAPEL, r2, sizeof(r2), v2, sizeof(v2), &p2);
      snprintf(val, tv, "%s  ·  %s", n[0] ? n : "-", pronto ? v2 : "-");
      break;
    }
    case G_LOCAL: {
      const int32_t p = s_mud.precisao_muda ? s_mud.precisao : s_cfg.precisao;
      if (p == 0) { snprintf(val, tv, "não compartilhada"); break; }
      if (p < 0)  { snprintf(val, tv, "-"); break; }
      linha_cfg(RC_POS_S, r2, sizeof(r2), v2, sizeof(v2), &p2);
      snprintf(val, tv, "%s  ·  %s%s", precisao_nome(p, tmp, sizeof(tmp)), v2,
               valor_atual(MESH_CFG_POS_INTELIGENTE) ? " e ao se mover" : "");
      break;
    }
    case G_RADIO:
      linha_cfg(RC_SALTOS, r2, sizeof(r2), v2, sizeof(v2), &p2);
      if (s_cfg.regiao >= 0) {
        snprintf(val, tv, "%s / %s  ·  %s saltos", nome_regiao(s_cfg.regiao),
                 s_cfg.usa_preset ? nome_preset(s_cfg.preset) : "personalizado", v2);
      } else { snprintf(val, tv, "-"); }
      break;
    case G_CONEXOES: {
      if (!pronto) { snprintf(val, tv, "-"); break; }
      char w[48];
      if (s_cfg.wifi_conhecido && s_cfg.wifi_ligado) { snprintf(w, sizeof(w), "Wi-Fi %s", s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "ligado"); }
      else                                           { snprintf(w, sizeof(w), "sem Wi-Fi"); }
      snprintf(val, tv, "Bluetooth %s  ·  %s  ·  MQTT %s", valor_atual(MESH_CFG_BT) ? "sim" : "não", w,
               !valor_atual(MESH_CFG_MQTT) ? "não" : valor_atual(MESH_CFG_MQTT_PROXY) ? "pelo Tab5" : "sim");
      break;
    }
    case G_AVISOS:
      linha_cfg(RC_TELE_S, r2, sizeof(r2), v2, sizeof(v2), &p2);
      snprintf(val, tv, "som %s  ·  telemetria %s", tab5_mesh_beep_ligado() ? "ligado" : "mudo", pronto ? v2 : "-");
      break;
    case G_MANUT:
      snprintf(val, tv, "%s  ·  %lu reinícios", s_cfg.firmware[0] ? s_cfg.firmware : "-", (unsigned long)s_cfg.reinicios);
      break;
  }
}

// O texto do item 'i' da lista do momento (grupo ou linha).
static void linha_item(int i, char* rot, size_t tr, char* val, size_t tv, bool* pend)
{
  if (s_grupo < 0) { linha_grupo(i, rot, tr, val, tv, pend); }
  else             { linha_cfg(GRUPOS[s_grupo].rc[i], rot, tr, val, tv, pend); }
}

// Um grupo: icone, nome, resumo e a seta de "abre".
static void grupo_desenhar(lgfx::LovyanGFX& d, int g, int x, int y, int largura)
{
  char rot[48], val[96] = "";
  bool pend = false;
  linha_grupo(g, rot, sizeof(rot), val, sizeof(val), &pend);
  snprintf(s_cfg_ant[g], sizeof(s_cfg_ant[g]), "%d|%s", (int)pend, val);
  s_cfg_pintada[g] = true;

  d.fillRect(x, y, largura, CFG_LIN, TFT_BLACK);
  icone_cfg(d, GRUPOS[g].icone, x + 26, y + CFG_LIN / 2, *tinta_cfg(d, GRUPOS[g].icone));
  d.setTextDatum(middle_left);
  tab5_texto(d, 3.6f);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(rot, x + 52, y + CFG_LIN / 2);
  const int rot_fim = x + 52 + (int)d.textWidth(rot) + 24;

  // A seta: este item abre outra lista.
  const int sx = x + largura - 30, sy = y + CFG_LIN / 2;
  d.drawLine(sx, sy - 9, sx + 9, sy, TFT_DARKGREY); d.drawLine(sx + 9, sy, sx, sy + 9, TFT_DARKGREY);
  d.drawLine(sx + 1, sy - 9, sx + 10, sy, TFT_DARKGREY); d.drawLine(sx + 10, sy, sx + 1, sy + 9, TFT_DARKGREY);

  tab5_texto(d, 2.8f);
  d.setTextColor(pend ? TFT_YELLOW : TFT_LIGHTGREY, TFT_BLACK);
  char v2[100];
  snprintf(v2, sizeof(v2), pend ? "• %s" : "%s", val);
  const int vx = em_pe(sistema_area()) ? rot_fim : (rot_fim > x + 440 ? rot_fim : x + 440);
  cortar_ao_caber(d, v2, sx - 16 - vx);
  d.drawString(v2, vx, y + CFG_LIN / 2);
  d.drawFastHLine(x + 16, y + CFG_LIN - 1, largura - 32, d.color565(38, 42, 50));
  d.setTextDatum(top_left);
}

static void cfg_desenhar(lgfx::LovyanGFX& d, int item, int x, int y, int largura, void*)
{
  if (s_grupo < 0) { grupo_desenhar(d, item, x, y, largura); return; }
  const int i = GRUPOS[s_grupo].rc[item];
  char rot[48], val[96] = "";
  bool pend = false;
  linha_cfg(i, rot, sizeof(rot), val, sizeof(val), &pend);
  snprintf(s_cfg_ant[item], sizeof(s_cfg_ant[item]), "%d|%s", (int)pend, val);
  s_cfg_pintada[item] = true;

  // A LINHA INTEIRA, sempre: um redesenho cobre tudo o que havia nela.
  d.fillRect(x, y, largura, CFG_LIN, TFT_BLACK);

  const bool editavel = (i < RC_EDITAVEIS);
  // Linha do radio sem no do outro lado: o icone fica apagado - ver,
  // sim; mudar, nao (so as do proprio Tab5).
  const bool ativa = editavel && (rc_do_tab5(i) || s_cfg.estado == MESH_CFG_PRONTO);
  // Icone da linha: colorido quando da para mexer, apagado sem o no do outro lado.
  icone_cfg(d, i, x + 26, y + CFG_LIN / 2,
            editavel ? (ativa ? *tinta_cfg(d, i) : d.color565(60, 64, 72)) : d.color565(90, 98, 112));

  d.setTextDatum(middle_left);
  tab5_texto(d, 3.2f);
  d.setTextColor(editavel ? TFT_LIGHTGREY : TFT_DARKGREY, TFT_BLACK);
  d.drawString(rot, x + 52, y + CFG_LIN / 2);
  const int rot_fim = x + 52 + (int)d.textWidth(rot) + 24;

  tab5_texto(d, 3.6f);
  d.setTextColor(pend ? TFT_YELLOW : (ativa || !editavel ? TFT_WHITE : TFT_DARKGREY), TFT_BLACK);
  char v2[100];
  snprintf(v2, sizeof(v2), pend ? "• %s" : "%s", val);
  if (em_pe(sistema_area())) {
    // EM PE nao ha largura para a coluna fixa em 440: o valor encosta na
    // direita e ocupa o que o rotulo deixar.
    cortar_ao_caber(d, v2, x + largura - 16 - rot_fim);
    d.setTextDatum(middle_right);
    d.drawString(v2, x + largura - 16, y + CFG_LIN / 2);
    d.setTextDatum(middle_left);
  } else {
    // Corte por caractere, para nao invadir a borda.
    cortar_ao_caber(d, v2, largura - 470);
    d.drawString(v2, x + 440, y + CFG_LIN / 2);
  }
  d.drawFastHLine(x + 16, y + CFG_LIN - 1, largura - 32, d.color565(38, 42, 50));
  d.setTextDatum(top_left);
}

// O rodape so existe com algo pendente: sem ele, a lista vai ate o fim da
// area. Quando ele aparece ou some, a janela muda de tamanho e a lista se
// redesenha inteira sozinha (lista_desenhar compara com a janela anterior).
static area_t janela_cfg(const area_t& a, int x0)
{
  const int rodape = ha_pendente() ? CFG_RODAPE : 0;
  return area_t{ x0, a.y + CFG_TOPO, a.x + a.largura - x0, a.altura - CFG_TOPO - rodape };
}

// ---- diario de reinicios ---------------------------------------------------
static const int REI_LIN = 126, REI_TOPO = 80, REI_RODAPE = 84;

static void rei_duracao(char* o, size_t n, uint32_t s)
{
  if (s >= 3600)     { snprintf(o, n, "%lu h %02lu min", (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60)); }
  else if (s >= 60)  { snprintf(o, n, "%lu min", (unsigned long)(s / 60)); }
  else               { snprintf(o, n, "%lu s", (unsigned long)s); }
}


static void desenhar_diario(lgfx::LovyanGFX& d, const area_t& a, int x0, int larg)
{
  lista_esconder(&s_l_cfg);
  const bool conf = s_limpar_ms && (tab5_agora_ms() - s_limpar_ms) < 4000;
  const uint32_t assin = mesh_diario_versao() * 2u + (conf ? 1u : 0u);
  static uint32_t s_assin_ant = 0xFFFFFFFFu;
  if (!TUDO && assin == s_assin_ant) { return; }
  s_assin_ant = assin;
  s_rei_n = mesh_diario_ler(s_rei, MESH_DIARIO_MAX);

  tab5_tela_limpar(x0, a.y, larg, a.altura, TFT_BLACK);
  tab5_botao(d, x0 + 24, a.y + 12, 200, 56, "VOLTAR", d.color565(50, 55, 65), TFT_WHITE);
  tab5_texto(d, 3.6f);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Reinícios do nó", x0 + 250, a.y + 40);

  const int cabe = (a.altura - REI_TOPO - REI_RODAPE) / REI_LIN;
  const int mostra = s_rei_n < cabe ? s_rei_n : cabe;
  const int max_l = larg - 64;
  if (s_rei_n == 0) {
    tab5_texto(d, 2.8f);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString("nenhum reinício anotado ainda", x0 + 32, a.y + REI_TOPO + 40);
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
      snprintf(hora, sizeof(hora), "Tab5 ligado há %s", dur);
    }
    static const char* const ORI[] = { "primeira leitura", "sozinho", "pedido pelo Tab5", "com o Tab5 desconectado" };
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
    const bool sem_indicio = strncmp(l3, "sem indício", 11) == 0;
    const uint16_t c3 = (r.origem == MESH_REI_SOZINHO && !sem_indicio) ? TFT_RED : TFT_CYAN;
    tab5_texto(d, 2.8f);
    d.setTextDatum(top_left);
    cortar_ao_caber(d, l1, max_l); cortar_ao_caber(d, l2, max_l); cortar_ao_caber(d, l3, max_l);
    d.setTextColor(c1, TFT_BLACK);        d.drawString(l1, x0 + 32, y + 6);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK); d.drawString(l2, x0 + 32, y + 42);
    d.setTextColor(c3, TFT_BLACK);        d.drawString(l3, x0 + 32, y + 78);
    d.drawFastHLine(x0 + 16, y + REI_LIN - 4, larg - 32, d.color565(38, 42, 50));
  }
  const int by = a.y + a.altura - REI_RODAPE + 14;
  tab5_botao(d, x0 + 24, by, 320, 56, conf ? "TOQUE DE NOVO" : "LIMPAR DIÁRIO",
             conf ? TFT_RED : d.color565(50, 55, 65), TFT_WHITE);
  tab5_texto(d, 2.4f);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  char rod[80];
  if (s_rei_n > mostra) { snprintf(rod, sizeof(rod), "+%d mais antigos (no cartão: Meshtastic/reinicios.txt)", s_rei_n - mostra); }
  else                  { snprintf(rod, sizeof(rod), "o motivo exato só aparece no log USB do nó; aqui é dedução"); }
  cortar_ao_caber(d, rod, larg - (24 + 320 + 24) - 16);
  d.drawString(rod, x0 + 24 + 320 + 24, by + 28);
  d.setTextDatum(top_left);
}

// ---- Wi-Fi do no: tudo numa tela ---------------------------------------------
// Interruptor liga/desliga em cima; com o Wi-Fi do no ligado, a lista das redes
// ja usadas (tocar escolhe, o x esquece) e as acoes NOVA REDE / TRAZER DO TAB5.
// Mudar o interruptor ou escolher rede e PENDENTE: GRAVAR NO NO manda e o no
// reinicia, como qualquer outra mudanca.
static const int RD_LIN = 78, RD_BTN_A = 60;
static const int WF_TOG_Y = 84, WF_TOG_H = 100, WF_LISTA_Y = 84 + 100 + 56;

static void interruptor(lgfx::LovyanGFX& d, int x, int y, int w, int h, bool ligado, bool ativo)
{
  const uint16_t trilho = !ativo ? d.color565(55, 58, 66) : ligado ? d.color565(52, 199, 89) : d.color565(110, 110, 118);
  d.fillRoundRect(x, y, w, h, h / 2, trilho);
  const int r = h / 2 - 6;
  d.fillCircle(ligado ? x + w - h / 2 : x + h / 2, y + h / 2, r, ativo ? TFT_WHITE : d.color565(150, 150, 158));
}

static uint32_t fnv_str(const char* t)
{
  uint32_t h = 2166136261u;
  for (; *t; ++t) { h = (h ^ (uint8_t)*t) * 16777619u; }
  return h;
}

static bool wf_liga_visivel(void) { return s_mud.wifi_muda ? s_mud.wifi_liga : s_cfg.wifi_ligado; }
// A lista so existe com o Wi-Fi do no LIGADO de fato (depois do reinicio).
static bool wf_lista_visivel(void) { return s_cfg.wifi_conhecido && s_cfg.wifi_ligado && wf_liga_visivel(); }
// 0 = sem botoes, 1 = GRAVAR/DESCARTAR, 2 = NOVA REDE/TRAZER DO TAB5
static int wf_botoes(void) { return ha_pendente() ? 1 : (wf_lista_visivel() ? 2 : 0); }

static void desenhar_redes(lgfx::LovyanGFX& d, const area_t& a, int x0, int larg)
{
  lista_esconder(&s_l_cfg);
  const uint32_t assin = mesh_wifi_no_versao() * 31u + fnv_str(s_redes_msg) + fnv_str(s_cfg.wifi_ssid) * 7u
                       + fnv_str(s_mud.wifi_ssid) * 13u + (uint32_t)s_cfg.estado * 131u + (s_cfg.wifi_ligado ? 977u : 0)
                       + (s_mud.wifi_muda ? 1999u : 0) + (s_mud.wifi_liga ? 4001u : 0) + (ha_pendente() ? 8009u : 0);
  static uint32_t s_assin_ant = 0xFFFFFFFFu;
  if (!TUDO && assin == s_assin_ant) { return; }
  s_assin_ant = assin;

  tab5_tela_limpar(x0, a.y, larg, a.altura, TFT_BLACK);
  tab5_botao(d, x0 + 24, a.y + 12, 200, 56, "VOLTAR", d.color565(50, 55, 65), TFT_WHITE);
  tab5_texto(d, 3.6f);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Wi-Fi do nó", x0 + 250, a.y + 40);

  // Linha do interruptor.
  const bool liga = wf_liga_visivel();
  const bool pode = s_cfg.estado == MESH_CFG_PRONTO && s_cfg.wifi_conhecido;
  icone_cfg(d, RC_WIFI, x0 + 50, a.y + WF_TOG_Y + WF_TOG_H / 2, pode ? d.color565(60, 170, 255) : d.color565(60, 64, 72));
  tab5_texto(d, 3.6f);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Wi-Fi do nó", x0 + 96, a.y + WF_TOG_Y + 30);
  tab5_texto(d, 2.8f);
  char sub[96];
  uint16_t csub = TFT_DARKGREY;
  if (!s_cfg.wifi_conhecido)               { snprintf(sub, sizeof(sub), "o nó ainda não enviou a configuração de rede"); }
  else if (s_mud.wifi_muda && s_mud.wifi_liga != s_cfg.wifi_ligado) {
    snprintf(sub, sizeof(sub), "vai %s ao gravar - o nó reinicia", s_mud.wifi_liga ? "ligar" : "desligar"); csub = TFT_YELLOW;
  }
  else if (s_cfg.wifi_ligado)              { snprintf(sub, sizeof(sub), "ligado%s%s", s_cfg.wifi_ssid[0] ? " em " : "", s_cfg.wifi_ssid); csub = TFT_GREENYELLOW; }
  else                                     { snprintf(sub, sizeof(sub), "desligado"); }
  d.setTextColor(csub, TFT_BLACK);
  cortar_ao_caber(d, sub, larg - 96 - 24 - 132 - 24);   // para antes do interruptor
  d.drawString(sub, x0 + 96, a.y + WF_TOG_Y + 70);
  interruptor(d, x0 + larg - 24 - 132, a.y + WF_TOG_Y + (WF_TOG_H - 68) / 2, 132, 68, liga, pode);
  d.drawFastHLine(x0 + 16, a.y + WF_TOG_Y + WF_TOG_H, larg - 32, d.color565(38, 42, 50));

  // Lista de redes.
  const int botoes = wf_botoes();
  const int reserva = (botoes ? RD_BTN_A + 14 + 14 : 0) + 40;   // botoes + mensagem
  if (wf_lista_visivel()) {
    tab5_texto(d, 2.4f);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString("REDES  (toque para escolher)", x0 + 32, a.y + WF_LISTA_Y - 24);
    const int n = mesh_wifi_no_n();
    const int cabe = (a.altura - WF_LISTA_Y - reserva) / RD_LIN;
    if (n == 0) {
      tab5_texto(d, 2.8f);
      d.setTextColor(TFT_DARKGREY, TFT_BLACK);
      char t0[64];
      snprintf(t0, sizeof(t0), "nenhuma rede guardada - use NOVA REDE ou TRAZER DO TAB5");
      cortar_ao_caber(d, t0, larg - 64);
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
      tab5_texto(d, 3.6f);
      d.setTextColor(escolhida ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
      const int max_l = larg - 440;
      tab5_cortar_ao_caber(d, t, max_l);
      d.drawString(t, x0 + 32, y + RD_LIN / 2);
      tab5_texto(d, 2.8f);
      d.setTextColor(escolhida ? TFT_YELLOW : no_no ? TFT_GREENYELLOW : TFT_DARKGREY, TFT_BLACK);
      d.drawString(escolhida ? "• escolhida" : no_no ? "● no nó agora" : (r.aberta ? "aberta" : "com senha"), x0 + larg - 380, y + RD_LIN / 2);
      const int cx = x0 + larg - 70, cy = y + RD_LIN / 2;   // "x" para esquecer
      d.drawLine(cx - 12, cy - 12, cx + 12, cy + 12, TFT_RED);
      d.drawLine(cx - 12, cy + 12, cx + 12, cy - 12, TFT_RED);
      d.drawFastHLine(x0 + 16, y + RD_LIN - 2, larg - 32, d.color565(38, 42, 50));
      memset(&r, 0, sizeof(r));
    }
  } else if (s_cfg.wifi_conhecido && !s_cfg.wifi_ligado) {
    tab5_texto(d, 2.8f);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    char t0[80];
    snprintf(t0, sizeof(t0), "ligue o Wi-Fi e grave: depois de reiniciar, as redes aparecem aqui");
    cortar_ao_caber(d, t0, larg - 64);
    d.drawString(t0, x0 + 32, a.y + WF_LISTA_Y + 10);
  }

  // Mensagem e botoes.
  d.setTextDatum(middle_left);
  tab5_texto(d, 2.4f);
  const int ym = a.y + a.altura - (botoes ? RD_BTN_A + 14 + 14 : 0) - 24;
  if (s_cfg.estado == MESH_CFG_GRAVANDO) { d.setTextColor(TFT_YELLOW, TFT_BLACK); d.drawString("gravando - o nó está reiniciando...", x0 + 24, ym); }
  else if (s_redes_msg[0]) {
    char m[sizeof(s_redes_msg)];
    snprintf(m, sizeof(m), "%s", s_redes_msg);
    cortar_ao_caber(d, m, larg - 48);
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.drawString(m, x0 + 24, ym);
  }
  const int by = a.y + a.altura - RD_BTN_A - 14;
  const int bl = (larg - 24 * 3) / 2;
  const bool pronto = s_cfg.estado == MESH_CFG_PRONTO;
  if (botoes == 1) {
    tab5_botao(d, x0 + 24, by, bl, RD_BTN_A, "GRAVAR NO NÓ", pronto ? TFT_GREENYELLOW : d.color565(50, 55, 65), pronto ? TFT_BLACK : TFT_DARKGREY);
    tab5_botao(d, x0 + 48 + bl, by, bl, RD_BTN_A, "DESCARTAR", d.color565(50, 55, 65), TFT_WHITE);
  } else if (botoes == 2) {
    tab5_botao(d, x0 + 24, by, bl, RD_BTN_A, "NOVA REDE", TFT_GREENYELLOW, TFT_BLACK);
    tab5_botao(d, x0 + 48 + bl, by, bl, RD_BTN_A, "TRAZER DO TAB5", d.color565(50, 55, 65), TFT_WHITE);
  }
  d.setTextDatum(top_left);
}

static void desenhar_config(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  mesh_cfg_ler(&s_cfg);
  const int larg = a.x + a.largura - x0;

  if (s_redes_pg) { desenhar_redes(d, a, x0, larg); return; }

  if (s_diario) { desenhar_diario(d, a, x0, larg); return; }

  // EDITANDO UM NOME: campo e teclado, nada mais na pagina.
  if (s_editando) {
    lista_esconder(&s_l_cfg);
    tecvirt_posicionar(&s_kb_cfg, a);
    static char s_ed_ant[sizeof(s_edit_txt) + 2];
    char ed[sizeof(s_edit_txt) + 2];
    snprintf(ed, sizeof(ed), "%s_", s_edit_txt);
    if (TUDO) {
      tab5_tela_limpar(x0, a.y, larg, a.altura - tecvirt_altura(a), TFT_BLACK);
      tab5_texto(d, 2.8f);
      d.setTextDatum(top_left);
      d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      const char* tit = s_editando == RC_LONGO ? "nome longo do nó (até 39 letras)"
                      : s_editando == RC_CURTO ? "nome curto do nó (até 4 letras)"
                      : s_editando == ED_WIFI_SSID ? "rede Wi-Fi do nó (até 32 letras)"
                      : "senha do Wi-Fi do nó (8 a 64 letras)";
      char tit2[96];
      if (s_editando == ED_WIFI_SENHA && s_mud.wifi_ssid[0]) { snprintf(tit2, sizeof(tit2), "senha da rede \"%s\" (8 a 64 letras)", s_mud.wifi_ssid); }
      else { snprintf(tit2, sizeof(tit2), "%s", tit); }
      cortar_ao_caber(d, tit2, larg - 48);
      d.drawString(tit2, x0 + 24, a.y + 24);
    }
    if (pintura_mudou_s(&s_p, s_ed_ant, sizeof(s_ed_ant), ed)) {
      if (!TUDO) { tab5_tela_limpar(x0 + 24, a.y + 70, larg - 48, 60, TFT_BLACK); }
      tab5_texto(d, 4.2f);
      d.setTextDatum(top_left);
      d.setTextColor(TFT_YELLOW, TFT_BLACK);
      d.drawString(ed, x0 + 24, a.y + 76);
    }
    tecvirt_pintar(&s_kb_cfg, d, TUDO);
    return;
  }

  // Cabecalho: o estado da conversa com o no.
  char est[96];
  uint16_t cor_est = TFT_DARKGREY;
  if (s_cfg.estado == MESH_CFG_GRAVANDO)    { snprintf(est, sizeof(est), "gravando - o nó está reiniciando..."); cor_est = TFT_YELLOW; }
  else if (s_cfg.estado == MESH_CFG_PRONTO) { snprintf(est, sizeof(est), "%s", s_cfg_msg[0] ? s_cfg_msg : "conectado ao nó"); cor_est = TFT_GREENYELLOW; }
  else { snprintf(est, sizeof(est), "sem o nó (cabo ou Bluetooth) - só leitura"); }
  static char s_est_ant[96];
  // Dentro de um grupo: VOLTAR e o nome dele no lugar do titulo.
  const char* titulo = s_grupo < 0 ? "Configurar o nó" : GRUPOS[s_grupo].nome;
  const int tx = s_grupo < 0 ? x0 + 24 : x0 + 250;
  tab5_texto(d, 3.6f);
  const int ex_min = tx + (int)d.textWidth(titulo) + 32;
  const int ex = ex_min > x0 + 380 ? ex_min : x0 + 380;
  if (TUDO) {
    tab5_tela_limpar(x0, a.y, larg, CFG_TOPO, TFT_BLACK);
    if (s_grupo >= 0) { tab5_botao(d, x0 + 24, a.y + 12, 200, 56, "VOLTAR", d.color565(50, 55, 65), TFT_WHITE); }
    tab5_texto(d, 3.6f);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(titulo, tx, a.y + CFG_TOPO / 2);
  }
  if (pintura_mudou_s(&s_p, s_est_ant, sizeof(s_est_ant), est)) {
    if (!TUDO) { tab5_tela_limpar(ex, a.y, a.x + a.largura - ex, CFG_TOPO, TFT_BLACK); }
    tab5_texto(d, 2.8f);
    cortar_ao_caber(d, est, a.x + a.largura - 16 - ex);
    d.setTextDatum(middle_left);
    d.setTextColor(cor_est, TFT_BLACK);
    d.drawString(est, ex, a.y + CFG_TOPO / 2);
  }
  d.setTextDatum(top_left);

  // Linhas: so as que mudaram de texto (a pagina roda a 1 Hz).
  const int n_itens = cfg_n_itens();
  if (TUDO || s_l_cfg.n != n_itens) {
    lista_itens_mudaram(&s_l_cfg, n_itens);
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
  } else {
    for (int i = 0; i < n_itens; ++i) {
      // Nunca desenhada: a rolagem a desenha quando ela aparecer, com o
      // valor do momento. Nao ha o que comparar - e marca-la aqui arrastaria
      // as visiveis junto (ver s_cfg_pintada).
      if (!s_cfg_pintada[i]) { continue; }
      char rot[48], val[96] = "", sig[112];
      bool pend = false;
      linha_item(i, rot, sizeof(rot), val, sizeof(val), &pend);
      snprintf(sig, sizeof(sig), "%d|%s", (int)pend, val);
      if (strcmp(sig, s_cfg_ant[i]) != 0) { lista_item_mudou(&s_l_cfg, i); }
    }
  }
  // A engrenagem acende e apaga com o estado: muda tudo de uma vez.
  static int s_estado_ant = -1;
  if (s_estado_ant != s_cfg.estado) {
    s_estado_ant = s_cfg.estado;
    lista_itens_mudaram(&s_l_cfg, n_itens);
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
  }
  lista_desenhar(&s_l_cfg, d, janela_cfg(a, x0), TFT_BLACK, TUDO);

  // Rodape: GRAVAR (com pendente e no pronto) e DESCARTAR.
  const bool pend = ha_pendente();
  const int estado_rod = (pend ? 1 : 0) | (s_cfg.estado == MESH_CFG_PRONTO ? 2 : 0);
  static int s_rod_ant = -1;
  if (TUDO || estado_rod != s_rod_ant) {
    s_rod_ant = estado_rod;
    // Sem pendente a faixa e da lista (ja desenhada acima): nada a limpar.
    if (pend) {
      const int by = a.y + a.altura - CFG_RODAPE + 12;
      tab5_tela_limpar(x0, a.y + a.altura - CFG_RODAPE, larg, CFG_RODAPE, TFT_BLACK);
      const bool pode = (s_cfg.estado == MESH_CFG_PRONTO);
      const int bl = cfg_btn_l(a);
      tab5_botao(d, x0 + 24, by, bl, CFG_BTN_A, "GRAVAR NO NÓ",
                 pode ? TFT_GREENYELLOW : d.color565(50, 55, 65), pode ? TFT_BLACK : TFT_DARKGREY);
      tab5_botao(d, x0 + 24 + bl + 24, by, bl, CFG_BTN_A, "DESCARTAR", d.color565(50, 55, 65), TFT_WHITE);
      // A dica so onde sobra lugar ao lado dos botoes (em pe, nao sobra).
      tab5_texto(d, 2.4f);
      const int dx = x0 + 24 + 2 * (bl + 24);
      if (dx + (int)d.textWidth("o nó grava e reinicia") < a.x + a.largura - 16) {
        d.setTextDatum(middle_left);
        d.setTextColor(TFT_DARKGREY, TFT_BLACK);
        d.drawString("o nó grava e reinicia", dx, by + CFG_BTN_A / 2);
        d.setTextDatum(top_left);
      }
    }
  }
}

static void cfg_aviso(const char* msg)
{
  snprintf(s_cfg_msg, sizeof(s_cfg_msg), "%s", msg);
  sistema_pedir_redesenho();
}

static void cfg_fechar_edicao(bool confirmar)
{
  if (confirmar && s_editando == ED_WIFI_SENHA && strlen(s_edit_txt) < 8) {
    cfg_aviso("a senha do Wi-Fi precisa de 8 letras ou mais");   // o firmware recusaria
    snprintf(s_redes_msg, sizeof(s_redes_msg), "a senha do Wi-Fi precisa de 8 letras ou mais");
    confirmar = false;
  }
  if (confirmar && s_edit_txt[0]) {
    char* dst; size_t tam;
    switch (s_editando) {
      case RC_LONGO:     dst = s_mud.longo;     tam = sizeof(s_mud.longo);     break;
      case ED_WIFI_SSID: dst = s_mud.wifi_ssid; tam = sizeof(s_mud.wifi_ssid); break;
      case ED_WIFI_SENHA: dst = s_mud.wifi_psk; tam = sizeof(s_mud.wifi_psk);  break;
      default:           dst = s_mud.curto;     tam = sizeof(s_mud.curto);     break;
    }
    snprintf(dst, tam, "%s", s_edit_txt);
  }
  const bool era_wifi = s_editando == ED_WIFI_SSID || s_editando == ED_WIFI_SENHA;
  const bool encadear = confirmar && s_editando == ED_WIFI_SSID && s_edit_txt[0] && !s_mud.wifi_psk[0];
  if (confirmar && s_editando == ED_WIFI_SENHA && s_edit_txt[0]) {
    s_mud.wifi_aberta = false;
    s_mud.wifi_muda = true; s_mud.wifi_liga = true;   // rede nova digitada: pronta para gravar
    snprintf(s_redes_msg, sizeof(s_redes_msg), "rede nova pronta - toque GRAVAR NO NÓ (o nó reinicia)");
  }
  memset(s_edit_txt, 0, sizeof(s_edit_txt));   // pode ter sido a senha do Wi-Fi
  // Rede nova digitada: em seguida pede a senha, sem sair da tela do Wi-Fi.
  s_editando = encadear ? ED_WIFI_SENHA : 0;
  if (!encadear) { tecvirt_fechar(&s_kb_cfg); }
  if (era_wifi && !encadear) { s_redes_pg = true; }   // volta a tela do Wi-Fi (confirmou ou cancelou)
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

static void cfg_tecla(int ac)
{
  if (ac == TECVIRT_ENVIAR) { cfg_fechar_edicao(true); return; }
  if (ac == TECVIRT_FECHAR) { cfg_fechar_edicao(false); return; }
  const size_t max = s_editando == RC_LONGO ? MESH_NOME_LONGO - 1
                   : s_editando == ED_WIFI_SSID ? 32
                   : s_editando == ED_WIFI_SENHA ? 64 : MESH_NOME_CURTO - 2;   // curto: 4
  // Nome e senha do no sao ASCII (ver ao_teclar). 'max' caracteres + o zero.
  tecvirt_editar(s_edit_txt, max + 1 < sizeof(s_edit_txt) ? max + 1 : sizeof(s_edit_txt), ac, true);
  sistema_pedir_redesenho();
}

// GRAVAR NO NO: valida o Wi-Fi pendente, manda a transacao e guarda a rede.
static void cfg_gravar(void)
{
  if (s_cfg.estado != MESH_CFG_PRONTO) { return; }
  // Interruptor mexido sem rede nova: reenvia a rede atual COM a senha que
  // guardamos, para a senha do no nao se perder na regravacao da secao.
  if (s_mud.wifi_muda && !s_mud.wifi_ssid[0] && s_cfg.wifi_ssid[0]) {
    for (int k = 0; k < mesh_wifi_no_n(); ++k) {
      mesh_wifi_rede_t r;
      if (mesh_wifi_no_rede(k, &r) && strcmp(r.ssid, s_cfg.wifi_ssid) == 0) {
        snprintf(s_mud.wifi_ssid, sizeof(s_mud.wifi_ssid), "%s", r.ssid);
        snprintf(s_mud.wifi_psk, sizeof(s_mud.wifi_psk), "%s", r.senha);
        s_mud.wifi_aberta = r.aberta;
        memset(&r, 0, sizeof(r));
        break;
      }
      memset(&r, 0, sizeof(r));
    }
  }
  if (s_mud.wifi_psk[0] && !s_mud.wifi_ssid[0]) { cfg_aviso("Wi-Fi: informe a rede E a senha"); return; }
  if (s_mud.wifi_ssid[0] && !s_mud.wifi_psk[0] && !s_mud.wifi_aberta) { cfg_aviso("Wi-Fi: informe a rede E a senha"); return; }
  char nova_rede[33], nova_senha[65];
  snprintf(nova_rede, sizeof(nova_rede), "%s", s_mud.wifi_ssid);
  snprintf(nova_senha, sizeof(nova_senha), "%s", s_mud.wifi_psk);
  if (mesh_cfg_gravar(&s_mud)) {
    // Guardada para escolher da proxima vez (cartao, senha cifrada).
    if (nova_rede[0]) { mesh_wifi_no_salvar(nova_rede, nova_senha); }
    memset(&s_mud, 0, sizeof(s_mud)); cfg_aviso(""); s_redes_msg[0] = 0;
  }
  else {
    const char* motivo = mesh_cfg_motivo()[0] ? mesh_cfg_motivo() : "não deu para gravar - veja o log";
    cfg_aviso(motivo);
    snprintf(s_redes_msg, sizeof(s_redes_msg), "%s", motivo);
  }
  memset(nova_senha, 0, sizeof(nova_senha));
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

static void tocar_config(int x, int y)
{
  const area_t a = sistema_area();
  if (s_redes_pg) {
    const int bx = TRILHO_L + 24;
    const int larg = a.largura - TRILHO_L;
    const int by = a.altura - RD_BTN_A - 14;
    const int bl = (larg - 24 * 3) / 2;
    const int botoes = wf_botoes();
    const bool bt_y = y >= by && y < by + RD_BTN_A;
    const bool bt_1 = bt_y && x >= bx && x < bx + bl;
    const bool bt_2 = bt_y && x >= bx + bl + 24 && x < bx + 2 * bl + 24;
    if (y >= 12 && y < 68 && x >= bx && x < bx + 200) {                  // VOLTAR
      s_redes_pg = false;
      lista_itens_mudaram(&s_l_cfg, cfg_n_itens());
      memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
    } else if (y >= WF_TOG_Y && y < WF_TOG_Y + WF_TOG_H) {               // interruptor
      if (s_cfg.estado != MESH_CFG_PRONTO || !s_cfg.wifi_conhecido) { return; }
      const bool novo = !wf_liga_visivel();
      s_mud.wifi_liga = novo;
      s_mud.wifi_muda = (novo != s_cfg.wifi_ligado);
      if (!novo || !s_mud.wifi_muda) { memset(s_mud.wifi_ssid, 0, sizeof(s_mud.wifi_ssid)); memset(s_mud.wifi_psk, 0, sizeof(s_mud.wifi_psk)); s_mud.wifi_aberta = false; }
      s_redes_msg[0] = 0;
      sistema_pedir_redesenho();
    } else if (botoes == 1 && bt_1) {                                    // GRAVAR NO NO
      cfg_gravar();
    } else if (botoes == 1 && bt_2) {                                    // DESCARTAR
      memset(&s_mud, 0, sizeof(s_mud));
      s_redes_msg[0] = 0;
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
    } else if (botoes == 2 && bt_1) {                                    // NOVA REDE
      s_redes_pg = false;
      memset(s_mud.wifi_psk, 0, sizeof(s_mud.wifi_psk));
      s_mud.wifi_aberta = false;
      s_editando = ED_WIFI_SSID;
      s_edit_txt[0] = 0;
      tecvirt_abrir(&s_kb_cfg);
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
    } else if (botoes == 2 && bt_2) {                                    // TRAZER DO TAB5
      const int n = mesh_wifi_no_trazer_do_tab5();
      if (n < 0)       { snprintf(s_redes_msg, sizeof(s_redes_msg), "o Tab5 não tem redes carregadas (ligue o Wi-Fi do Tab5 uma vez)"); }
      else if (n == 0) { snprintf(s_redes_msg, sizeof(s_redes_msg), "nada novo: todas as redes do Tab5 já estão aqui"); }
      else             { snprintf(s_redes_msg, sizeof(s_redes_msg), "%d rede(s) trazida(s) do Tab5", n); }
      sistema_pedir_redesenho();
    } else if (wf_lista_visivel() && y >= WF_LISTA_Y && y < a.altura - RD_BTN_A - 14 - 14 - 40) {
      const int i = (y - WF_LISTA_Y) / RD_LIN;
      mesh_wifi_rede_t r;
      if (mesh_wifi_no_rede(i, &r)) {
        if (x >= TRILHO_L + larg - 130) {                  // o "x": esquecer
          mesh_wifi_no_esquecer(i);
          snprintf(s_redes_msg, sizeof(s_redes_msg), "\"%s\" esquecida", r.ssid);
        } else if (s_cfg.estado == MESH_CFG_PRONTO) {      // escolher: fica pendente, GRAVAR aparece
          snprintf(s_mud.wifi_ssid, sizeof(s_mud.wifi_ssid), "%s", r.ssid);
          snprintf(s_mud.wifi_psk, sizeof(s_mud.wifi_psk), "%s", r.senha);
          s_mud.wifi_aberta = r.aberta;
          s_mud.wifi_muda = true;
          s_mud.wifi_liga = true;
          snprintf(s_redes_msg, sizeof(s_redes_msg), "\"%s\" escolhida - toque GRAVAR NO NÓ (o nó reinicia)", r.ssid);
        }
        memset(&r, 0, sizeof(r));
        sistema_pedir_redesenho_completo();
      }
    }
    return;
  }
  if (s_diario) {
    const int bx = TRILHO_L + 24;
    const int ly = a.altura - REI_RODAPE + 14;
    if (y >= 12 && y < 68 && x >= bx && x < bx + 200) {
      s_diario = false;
      s_limpar_ms = 0;
      lista_itens_mudaram(&s_l_cfg, cfg_n_itens());
      memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
    } else if (y >= ly && y < ly + 56 && x >= bx && x < bx + 320) {
      if (s_limpar_ms && (tab5_agora_ms() - s_limpar_ms) < 4000) { s_limpar_ms = 0; mesh_diario_limpar(); }
      else { s_limpar_ms = tab5_agora_ms() | 1u; }
      sistema_pedir_redesenho();
    }
    return;
  }
  if (s_editando) {
    if (tecvirt_contem(&s_kb_cfg, a.x + x, a.y + y)) {
      const int ac = tecvirt_clique(&s_kb_cfg, a.x + x, a.y + y);
      if (ac) { cfg_tecla(ac); }
    }
    return;
  }

  // VOLTAR de dentro de um grupo: sobe para a lista dos grupos.
  if (s_grupo >= 0 && y >= 12 && y < 68 && x >= TRILHO_L + 24 && x < TRILHO_L + 224) {
    s_grupo = -1;
    s_reiniciar_ms = 0;
    lista_ao_inicio(&s_l_cfg);
    lista_itens_mudaram(&s_l_cfg, cfg_n_itens());
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }

  // Rodape.
  const int by = a.altura - CFG_RODAPE + 12;
  if (y >= by && y < by + CFG_BTN_A && ha_pendente()) {
    const int bx = TRILHO_L + 24;
    const int bl = cfg_btn_l(a);
    if (x >= bx && x < bx + bl) {
      cfg_gravar();
    } else if (x >= bx + bl + 24 && x < bx + 2 * bl + 24) {
      memset(&s_mud, 0, sizeof(s_mud));
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
    }
    return;
  }

  const int item = lista_item_tocado(&s_l_cfg, x, y, nullptr);
  if (item < 0) { return; }
  // Na lista dos grupos: abre o grupo tocado.
  if (s_grupo < 0) {
    if (item >= G_TOTAL) { return; }
    s_grupo = item;
    s_cfg_msg[0] = 0;
    lista_ao_inicio(&s_l_cfg);
    lista_itens_mudaram(&s_l_cfg, cfg_n_itens());
    memset(s_cfg_pintada, 0, sizeof(s_cfg_pintada));
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }
  if (item >= GRUPOS[s_grupo].n) { return; }
  const int i = GRUPOS[s_grupo].rc[item];
  if (i == RC_I_REINICIOS) {
    s_diario = true;
    s_limpar_ms = 0;
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }
  if (i < 0 || i >= RC_EDITAVEIS) { return; }
  if (i == RC_SOM) { tab5_mesh_definir_beep(!tab5_mesh_beep_ligado()); sistema_pedir_redesenho(); return; }
  if (i == RC_LOC_TAB5) {
    ajustes_definir_mesh_posicao_min((uint8_t)opc_proximo(OPC_TAB5, N_OPC(OPC_TAB5), ajustes_mesh_posicao_min()));
    sistema_pedir_redesenho();
    return;
  }
  if (s_cfg.estado != MESH_CFG_PRONTO) { return; }
  if (i == RC_LOC_PRECISAO) {
    if (s_cfg.precisao < 0) { cfg_aviso("o canal principal do nó ainda não chegou"); return; }
    const int32_t atual = s_mud.precisao_muda ? s_mud.precisao : s_cfg.precisao;
    s_mud.precisao = opc_proximo(OPC_PRECISAO, N_OPC(OPC_PRECISAO), atual);
    s_mud.precisao_muda = (s_mud.precisao != s_cfg.precisao);
    sistema_pedir_redesenho();
    return;
  }

  const int c = campo_da_linha(i);
  if (c >= 0) {
    if (!s_cfg.conhecido[c]) { return; }
    int32_t novo = 0;
    const int32_t v = valor_atual(c);
    switch (i) {
      case RC_PAPEL:      novo = opc_proximo(OPC_PAPEL, N_OPC(OPC_PAPEL), v); break;
      case RC_SALTOS:     novo = opc_proximo(OPC_SALTOS, N_OPC(OPC_SALTOS), v == 0 ? 3 : v); break;
      case RC_TX:         novo = opc_proximo(OPC_TX, N_OPC(OPC_TX), v); break;
      case RC_POS_S:      novo = opc_proximo(OPC_POS_S, N_OPC(OPC_POS_S), v); break;
      case RC_NODEINFO_S: novo = opc_proximo(OPC_NODEINFO_S, N_OPC(OPC_NODEINFO_S), v); break;
      case RC_TELE_S:     novo = opc_proximo(OPC_TELE_S, N_OPC(OPC_TELE_S), v); break;
      case RC_MQTT:
        novo = v ? 0 : 1;
        // O espelho da regra do proxy: sem o MQTT o proxy nao tem o que
        // levar - desligar o MQTT leva o proxy junto. Religar devolve o proxy
        // ao que o no tinha.
        if (!novo && valor_atual(MESH_CFG_MQTT_PROXY) && s_cfg.conhecido[MESH_CFG_MQTT_PROXY]) {
          s_mud.v[MESH_CFG_MQTT_PROXY] = 0;
          s_mud.muda[MESH_CFG_MQTT_PROXY] = (s_cfg.v[MESH_CFG_MQTT_PROXY] != 0);
        }
        if (novo && s_mud.muda[MESH_CFG_MQTT_PROXY] && s_cfg.v[MESH_CFG_MQTT_PROXY]) { s_mud.muda[MESH_CFG_MQTT_PROXY] = false; }
        break;
      case RC_POS_INTELIGENTE: novo = v ? 0 : 1; break;
      case RC_GPS_NO:
        // 2 = NOT_PRESENT: o firmware decide isso sozinho (a placa nao tem
        // GPS); ligar daqui nao faria aparecer um.
        if (v == 2) { cfg_aviso("a placa do nó não tem GPS"); return; }
        novo = v == 1 ? 0 : 1;
        break;
      case RC_PROXY:
        novo = v ? 0 : 1;
        // O proxy so age com o MQTT do no ligado: ligar o proxy leva o MQTT
        // junto, na MESMA transacao (as duas linhas ficam pendentes).
        // Desligar o proxy nao mexe no MQTT.
        if (novo && !valor_atual(MESH_CFG_MQTT) && s_cfg.conhecido[MESH_CFG_MQTT]) {
          s_mud.v[MESH_CFG_MQTT] = 1;
          s_mud.muda[MESH_CFG_MQTT] = (s_cfg.v[MESH_CFG_MQTT] != 1);
        }
        // Desistiu de ligar (voltou a desligado): o MQTT que tinha vindo junto
        // volta tambem, se o no estava com ele desligado.
        if (!novo && s_mud.muda[MESH_CFG_MQTT] && !s_cfg.v[MESH_CFG_MQTT]) { s_mud.muda[MESH_CFG_MQTT] = false; }
        break;
      case RC_BT:
        // Desligar o Bluetooth do no com o Tab5 falando por ele cortaria a
        // propria conversa - e sem ela nao ha como religar daqui.
        if (v && tab5_mesh_via() == AJUSTES_MESH_BLUETOOTH) { cfg_aviso("o Tab5 está falando com o nó pelo Bluetooth"); return; }
        novo = v ? 0 : 1;
        break;
    }
    // Voltou ao valor que o no ja tem: deixa de ser pendente.
    s_mud.v[c] = novo;
    s_mud.muda[c] = (novo != s_cfg.v[c]);
    sistema_pedir_redesenho();
    return;
  }

  switch (i) {
    case RC_WIFI:
      s_redes_pg = true;
      s_redes_msg[0] = 0;
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
      return;
    case RC_LONGO: case RC_CURTO:
      s_editando = i;
      s_edit_txt[0] = 0;
      tecvirt_abrir(&s_kb_cfg);
      pintura_pedir(&s_p);
      sistema_pedir_redesenho_completo();
      return;
    case RC_POS_FIXA:
      if (s_cfg.pos_fixa) { cfg_aviso(mesh_cfg_tirar_posicao_fixa() ? "posição fixa removida" : "não deu para remover"); }
      else                { cfg_aviso(mesh_cfg_posicao_fixa_do_gps() ? "posição do GPS enviada ao nó" : "GPS do Tab5 sem sinal"); }
      return;
    case RC_HORA:
      cfg_aviso(mesh_cfg_acertar_hora() ? "hora do nó acertada" : "o Tab5 ainda não sabe a hora");
      return;
    case RC_REINICIAR:
      if (s_reiniciar_ms && (tab5_agora_ms() - s_reiniciar_ms) < 4000) {
        s_reiniciar_ms = 0;
        cfg_aviso(mesh_cfg_reiniciar() ? "" : "não deu para reiniciar");
      } else {
        s_reiniciar_ms = tab5_agora_ms();
        sistema_pedir_redesenho();
      }
      return;
  }
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  // O teclado fisico tambem edita estes campos. Se ele foi encaixado com o
  // virtual aberto, libera a metade de baixo e a navegacao sem abandonar a
  // edicao nem apagar o texto ja digitado.
  if (tab5_teclado_presente() && s_editando && tecvirt_aberto(&s_kb_cfg)) {
    tecvirt_fechar(&s_kb_cfg);
  }

  // O pintura_iniciar() ja consulta sozinho se o sistema limpou a area - era o
  // passo que, esquecido, deixava a tela em branco ao acordar.
  //
  // Repintar NAO implica reler o cartao: a copia em RAM continua valida.
  // Confundir as duas coisas fazia o app montar o SD a cada toque.
  // A ORDEM DA COLUNA MUDOU: reencontra a conversa aberta pela chave. Se ela
  // saiu da coluna (canal desabilitado no no, servico trocando de via), a tela
  // volta a "escolha uma" mas a chave fica: quando ela reaparecer, reabre.
  // ANTES do pintura_iniciar, para esta passada ja sair completa - a coluna
  // inteira mudou de lugar (ver a regra da repintura completa).
  const uint32_t cv = tab5_mesh_conversas_versao();
  if (cv != s_conv_ver) {
    s_conv_ver = cv;
    if (s_ab.ok) {
      const int novo = tab5_mesh_conversa_indice(s_ab.canal, s_ab.idx, s_ab.no);
      if (novo != s_conv) {
        if ((novo < 0) != (s_conv < 0)) { conversa_trocar(&s_c); s_hist_pronto = false; }
        s_conv = novo;
      }
    }
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
  }

  pintura_iniciar(&s_p);

  // A primeira carga tambem le do cartao os nomes e as conversas - e tem que
  // acontecer mesmo sem conversa aberta, senao a coluna nasce vazia sem o no.
  if (s_carga_cartao && !s_carga_pedida) { carregar_hist(); }

  // Mensagem nova no servico: ai sim vale reler.
  //
  // tab5_mesh_total_msgs(), NAO tab5_mesh_num_msgs() - sintoma: mensagem
  // nova so aparecia depois de trocar de tela e voltar. O
  // segundo satura em MESH_MAX_MSGS (8) e fica parado para sempre depois
  // da oitava mensagem desde o boot do servico - a comparacao abaixo nunca
  // mais via diferenca, entao o app so recarregava por outros caminhos
  // (trocar de conversa, trocar de pagina) que zeram s_hist_pronto direto,
  // nunca sozinho enquanto a tela ficava aberta. Ver o aviso grande no
  // .hpp junto de tab5_mesh_total_msgs().
  const int64_t nm = (int64_t)tab5_mesh_total_msgs();
  // COMPARACAO DIRETA, e nao pintura_mudou_i(). O helper devolve true em toda
  // passada COMPLETA, por desenho - e completa e o que faz o app repintar tudo.
  // Usa-lo para decidir E/S cria loop: passada completa -> "mudou" -> recarrega
  // -> pintura_pedir() -> proxima passada completa -> ... O teclado repintava a
  // cada ciclo e piscava sozinho.
  //
  // Regra: pintura_mudou_* serve para decidir DESENHO. Decisao de ler cartao,
  // abrir arquivo ou falar com hardware compara na mao.
  if (nm != s_msgs_quando_li) {
    // So PEDE a carga nova (o worker le o cartao). Quando ela chega, o
    // ao_aviso troca os buffers e pede a repintura - que aqui seria cedo
    // demais (o historico novo ainda nao existe) e duplicada.
    //
    // A posicao de leitura e da lista (kernel/list): colado no fim, a
    // mensagem nova aparece; lendo o historico, a tela nao se mexe.
    s_hist_pronto = false;
    s_msgs_quando_li = nm;
    sistema_pedir_redesenho();
  }


  // ACK chegou (ou prazo estourou): sao os MESMOS baloes, so a marca mudou.
  // Nao releia o cartao por causa disso - o conteudo nao mudou, e montar o SD a
  // cada confirmacao seria caro a toa. Comparacao direta pelo mesmo motivo do
  // bloco acima.
  const uint32_t ev = tab5_mesh_envios_versao();
  if (ev != s_envios_ver) { s_envios_ver = ev; conversa_mudou(&s_c); }   // as marcas de envio

  // EM PE: ou a lista (trilho + coluna larga), ou a conversa por cima de
  // tudo. Deitado: trilho, coluna e conversa lado a lado, como sempre.
  const bool pe = em_pe(a);
  const bool frente = pe && s_pag == PAG_MSGS && s_conv_frente && s_conv >= 0;

  if (TUDO && !frente) { desenhar_trilho(d, a); }
  const int x0 = frente ? a.x
               : a.x + TRILHO_L + ((s_pag == PAG_MSGS && !pe) ? CONV_L : 0);

  switch (s_pag) {
    case PAG_NOS:   desenhar_nos(d, a, x0);   break;
    case PAG_MALHA: desenhar_malha(d, a, x0); break;
    case PAG_NO:    desenhar_config(d, a, x0); break;
    default:
      if (pe && !frente) {
        // Em pe, so a lista: os baloes nao estao na tela, entao nao rolam
        // nem respondem, e o teclado (se estava aberto) fecha.
        conversa_fechar_teclado(&s_c);
        lista_esconder(&s_c.lista);
      } else {
        desenhar_msgs(d, a, x0);
      }
      break;
  }
  // A coluna DEPOIS das mensagens: desenhar_msgs marca como lida a conversa
  // aberta, e o contador dela na coluna ja sai zerado nesta mesma passada.
  // Com a conversa na frente a coluna esta coberta: nao rola nem responde.
  if (s_pag == PAG_MSGS && !frente) { desenhar_conversas(d, a); }
  else                              { lista_esconder(&s_l_conv); }
}

// A TELA GIROU (kernel/orientation). Deitado -> em pe com uma conversa
// aberta: ela vem para a frente, e quem estava escrevendo segue escrevendo.
// Em pe -> deitado: lista e conversa voltam a ficar lado a lado. O resto se
// acomoda sozinho na passada completa que o sistema ja pediu.
static void ao_girar(void)
{
  s_conv_frente = (s_conv >= 0);
  pintura_pedir(&s_p);
}

// Em pe: a conversa sai da frente e a lista volta.
static void voltar_a_lista(void)
{
  s_conv_frente = false;
  conversa_fechar_teclado(&s_c);
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

// Teclado fisico do ExtPort1: o componente de conversa trata.
static void ao_teclar(const char* texto, uint8_t codigo, uint8_t mod)
{
  // Teclado fisico tambem edita o nome do no.
  if (s_pag == PAG_NO && s_editando) {
    if      (codigo == TAB5_TECLA_ENTER)  { cfg_fechar_edicao(true); }
    else if (codigo == TAB5_TECLA_ESC)    { cfg_fechar_edicao(false); }
    else if (codigo == TAB5_TECLA_APAGAR) { cfg_tecla(TECVIRT_APAGAR); }
    else if (texto && texto[0]) {
      // Nome e senha do no sao ASCII: a letra composta pela tecla morta
      // volta a ser as duas teclas apertadas.
      char cru[8];
      tab5_teclado_ascii(texto, cru, sizeof(cru));
      for (const char* c = cru; *c && s_editando; ++c) { cfg_tecla((unsigned char)*c); }
    }
    return;
  }
  if (s_pag != PAG_MSGS || s_conv < 0) { return; }
  conversa_teclar(&s_c, texto, codigo, mod);
}

// TROCA DE PAGINA num lugar so. As listas da pagina que sai param de
// responder ao toque (lista_esconder) - voltam quando forem desenhadas.
static void ir_para_pagina(pagina_t p)
{
  s_pag = p;
  s_no_sel = 0;   // tocar no trilho volta sempre para a tabela
  conversa_fechar_teclado(&s_c);   // devolve a navegacao, se estava travada
  lista_esconder(&s_c.lista);
  lista_esconder(&s_l_conv);
  lista_esconder(&s_l_nos);
  lista_esconder(&s_l_cfg);
  if (s_editando) { s_editando = 0; tecvirt_fechar(&s_kb_cfg); }
  s_diario = false;
  s_redes_pg = false;
  s_grupo = -1;
  s_cfg_msg[0] = 0;
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

// ABRIR UMA CONVERSA (c = -1: nenhuma, "escolha uma") - da coluna, de um no ou
// de um canal disponivel. Conversa abre na mensagem mais recente.
static void abrir_conversa(int c)
{
  s_conv = c;
  s_conv_frente = (c >= 0);   // em pe, a escolhida vem para a frente (deitado, nada muda)
  s_ab = chave_de(c);
  s_conv_ver = tab5_mesh_conversas_versao();
  s_hist_pronto = false;
  atualizar_eh_canal();
  conversa_trocar(&s_c);   // fecha o teclado, vai ao fim, re-mede
  if (c >= 0) { tab5_mesh_marcar_conversa_lida(c); }
  if (s_pag == PAG_MSGS && c >= 0) {
    // Trocando de conversa pela coluna: o teclado (e o rascunho) ficam.
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }
  ir_para_pagina(PAG_MSGS);
}

static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  // Tecla sob o dedo, soltura e rolagem dos baloes: o componente de conversa.
  if (s_pag == PAG_MSGS && s_conv >= 0) { conversa_tocar_nivel(&s_c, x, y, pressionado); }

  // As outras duas listas rolam cada uma na sua janela; a que nao esta na tela
  // (lista_esconder) ignora o toque.
  // Teclado do nome do no: as teclas confirmadas nesta amostra.
  if (s_pag == PAG_NO && s_editando) {
    int acoes[8];
    const int n = tecvirt_nivel(&s_kb_cfg, acoes, 8);
    for (int k = 0; k < n && s_editando; ++k) { cfg_tecla(acoes[k]); }
    return;
  }

  bool rolou = false;
  rolou |= lista_arrastar(&s_l_conv, x, y, pressionado);
  rolou |= lista_arrastar(&s_l_nos,  x, y, pressionado);
  rolou |= lista_arrastar(&s_l_cfg,  x, y, pressionado);
  if (rolou) { sistema_pedir_redesenho(); }
}

static void ao_tocar(int x, int y)
{
  const area_t a = sistema_area();

  // Teclado aberto: a faixa dele (largura inteira) e so dele.
  if (conversa_teclado_aberto(&s_c) && s_pag == PAG_MSGS && y >= a.altura - tecvirt_altura(a)) {
    conversa_tocar(&s_c, x, y);
    return;
  }

  // EM PE, CONVERSA NA FRENTE: nao ha trilho nem coluna embaixo do dedo -
  // so a faixa do VOLTAR e a conversa.
  if (em_pe(a) && s_pag == PAG_MSGS && s_conv_frente && s_conv >= 0) {
    if (y < FRENTE_TOPO_A) {
      if (x >= 16 && x < 16 + FRENTE_VOLTAR_L
       && y >= FRENTE_VOLTAR_Y && y < FRENTE_VOLTAR_Y + FRENTE_VOLTAR_A) {
        voltar_a_lista();
      }
      return;
    }
    conversa_tocar(&s_c, x, y);
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
    // CARTAO DE UM NO aberto: ESTRELA, VOLTAR e CONVERSAR.
    if (s_no_sel) {
      const int fav_l = det_fav_l(a);
      const int fx = a.largura - DET_FAV_DIR - fav_l;
      if (s_no_sel != tab5_mesh_meu_numero()
       && x >= fx && x < fx + fav_l && y >= DET_FAV_Y && y < DET_FAV_Y + DET_FAV_A) {
        tab5_mesh_definir_favorito(s_no_sel, !tab5_mesh_favorito(s_no_sel));
        sistema_pedir_redesenho();
        return;
      }
      const int by = a.altura - DET_BTN_Y_DA_BASE;
      if (y >= by && y < by + DET_BTN_A) {
        const int bx = TRILHO_L + 24;
        const int bl = det_btn_l(a);
        if (x >= bx && x < bx + bl) {
          s_no_sel = 0;
          pintura_pedir(&s_p);
          sistema_pedir_redesenho_completo();
        } else if (x >= bx + bl + 24 && x < bx + 2 * bl + 24
                && s_no_sel != tab5_mesh_meu_numero() && !no_internet(s_no_sel)) {
          // -1 quando o numero nao serve (zero, ou o proprio no).
          const int c = tab5_mesh_conversa_com(s_no_sel);
          s_no_sel = 0;
          if (c >= 0) { abrir_conversa(c); }
        }
      }
      return;
    }

    // A REDE: troca a tabela. E o liga/desliga da internet.
    for (int r = 0; r < 2; ++r) {
      const chip_t& k = s_chip_rede[r];
      if (x < k.x || x >= k.x + k.l || y < k.y || y >= k.y + k.a) { continue; }
      if (s_rede_vista != r) {
        s_rede_vista = (uint8_t)r;
        lista_ao_inicio(&s_l_nos);
        pintura_pedir(&s_p);
        sistema_pedir_redesenho_completo();
      }
      return;
    }
    {
      const chip_t& k = s_chip_internet_liga;
      if (k.l && x >= k.x && x < k.x + k.l && y >= k.y && y < k.y + k.a) {
        ajustes_definir_mesh_rede_internet(!ajustes_mesh_rede_internet());   // o laco do servico sobe/derruba o cliente
        pintura_pedir(&s_p);
        sistema_pedir_redesenho_completo();
        return;
      }
    }

    // UM CANAL: abre a conversa dele (e a traz de volta, se foi apagada).
    for (int i = 0; i < s_n_chips; ++i) {
      const chip_t& k = s_chips[i];
      if (x < k.x || x >= k.x + k.l || y < k.y || y >= k.y + k.a) { continue; }
      const int c = tab5_mesh_conversa_do_canal(k.idx);
      if (c >= 0) { abrir_conversa(c); }
      return;
    }

    // UM NO: o cartao dele. Dali se conversa - antes o toque abria a conversa
    // direto, e nao havia onde ver MAC, tensao ou posicao.
    const int i = lista_item_tocado(&s_l_nos, x, y, nullptr);
    if (i < 0) { return; }
    mesh_no_t no;
    if (!tab5_mesh_no_rede(s_rede_vista, i, &no)) { return; }
    s_no_sel = no.num;
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }

  if (s_pag == PAG_NO) { tocar_config(x, y); return; }
  if (s_pag == PAG_MALHA) { return; }

  if (s_pag != PAG_MSGS) { return; }

  // Coluna de conversas.
  if (x < TRILHO_L + s_col_l) {
    int dy = 0;
    const int i = lista_item_tocado(&s_l_conv, x, y, &dy);
    if (i < 0) { return; }
    if (i == s_conv_div) { dy -= CONV_DIV_A; }   // o traco da internet em cima do item
    dy -= 8;                          // o item comeca pelo respiro (conv_desenhar)
    if (dy < 0 || dy >= ITEM_A) { return; }   // no respiro entre itens: nada

    // O "x" vem ANTES de selecionar: ele esta dentro do item, e tratar a
    // selecao primeiro faria o toque no botao virar toque no item.
    const int ax = TRILHO_L + 1 + apagar_dx();
    if (i == s_conv
     && x >= ax && x < ax + APAGAR_L
     && dy >= APAGAR_DY && dy < APAGAR_DY + APAGAR_A
     && tab5_mesh_esquecer_conversa(i)) {
      // Nenhuma conversa seleciona no lugar: os indices acima deste andaram
      // uma casa, e herdar o indice abriria a conversa VIZINHA - uma que a
      // pessoa nao pediu. Melhor voltar para "escolha uma".
      abrir_conversa(-1);
      return;
    }
    abrir_conversa(i);
    return;
  }

  // Barra de escrita e botao enviar.
  // Coluna da conversa: barra de escrita, ENV e rolagem - o componente.
  if (s_conv >= 0) { conversa_tocar(&s_c, x, y); }
}

static bool ao_abrir(void)
{
  // O ARQUIVO PRIMEIRO. Sem o C6L plugado a lista de conversas nasce vazia -
  // ela so era povoada por mensagem chegando - e o app abria dizendo "sem
  // conversas" com o cartao cheio delas.
  //
  // Vale com o radio no ar tambem: conversas antigas aparecem desde a abertura,
  // e nao so quando alguem volta a falar.
  // Nomes, conversas e o historico vem do cartao PELO WORKER, junto da
  // primeira carga (ver trabalho_carregar) - a tela abre na hora.
  s_carga_cartao = true;
  s_carga_pedida = false;
  s_carga_pronta = false;
  s_recarregar   = false;
  s_hist_chave   = chave_t{ };

  s_hist_pronto = false;
  conversa_abrir(&s_c, &s_p, conv_contar, conv_ler, conv_enviar, nullptr);
  s_c.limite = MESH_PAYLOAD_MAX;   // o teto do protocolo, nao o do buffer (ver tab5_mesh.hpp)
  s_c.cor_enviar = lgfx::color565(103, 234, 148);   // o verde da marca (marca_mesh)
  lista_definir(&s_l_conv, ROLAGEM_INICIO, 0, conv_altura, conv_desenhar, nullptr);
  s_l_conv.topo = 4;   // o 1o item ja traz 8px de respiro: 12 do topo, como sempre foi
  lista_definir(&s_l_nos, ROLAGEM_INICIO, 0, nos_altura, nos_desenhar, nullptr);
  lista_definir(&s_l_cfg, ROLAGEM_INICIO, 0, cfg_altura, cfg_desenhar, nullptr);
  pintura_pedir(&s_p);
  sistema_travar_navegacao(false);
  return true;
}

static void ao_fechar(void)
{
  // Devolve o historico. Sao 38KB de PSRAM que nao fazem falta a ninguem com o
  // app fechado - e o motivo de ele nao ser mais estatico.
  // (O sistema ja esperou o worker antes de chamar isto.)
  if (s_hist)  { heap_caps_free(s_hist);  s_hist  = nullptr; }
  if (s_carga) { heap_caps_free(s_carga); s_carga = nullptr; }
  s_hist_chave = chave_t{ };
  if (s_editando) { s_editando = 0; tecvirt_fechar(&s_kb_cfg); }
  s_diario = false;
  s_redes_pg = false;
  s_grupo = -1;
  conversa_fechar(&s_c);   // devolve o cache dos baloes e a navegacao
  s_n_hist = 0;
  s_hist_pronto = false;
}

static uint32_t periodo_agora(void)
{
  return (s_pag == PAG_NOS || s_pag == PAG_MALHA || s_pag == PAG_NO) ? 1000 : 0;
}

extern const app_t APP_MESH = {
  .nome  = "Mesh",
  .cor   = TFT_GREENYELLOW,
  // O Grove ja e mantido pelo ajuste do sistema. O IMU e para a tela seguir
  // o aparelho.
  .perfil = { .imu = true },
  .usa_sd = true,           // historico das conversas
  // Em pe ou deitado conforme o aparelho; com o teclado encaixado, deitado -
  // a postura natural de quem escreve (ver o LAYOUT no topo do arquivo).
  .orientacao = ORI_SEGUE_IMU,
  .piso_repouso_mhz = 0,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,   // acende a tecla sob o dedo
  .ao_teclar = ao_teclar,             // teclado fisico do ExtPort1
  .periodo_ms = 0,
  // Relogio SO nas paginas que mostram idade (Nos, Malha); nas mensagens o app
  // so acorda por aviso - mensagem nova, envio confirmado, no novo, carga do
  // historico pronta.
  .periodo_agora = periodo_agora,
  .avisos = AVISO_MESH_MSG | AVISO_MESH_ENVIO | AVISO_MESH_NOS | AVISO_TRABALHO,
  .ao_aviso = ao_aviso,
  // O icone da tela inicial e a propria marca do Meshtastic: quem procura este
  // app procura o simbolo dele, nao a letra M.
  .icone = marca_mesh,
  .ao_girar = ao_girar,
};
