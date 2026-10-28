// APP: Arquivos - navegador of LEITURA of cartao.
//
// Tres niveis: pastas dos aplicativos -> arquivos of pasta -> content of
// arquivo. So le. Nao copia, nao move, nao apaga - of proposito, para nao
// existir a possibilidade of perder dado por one touch errado. O dia em que
// erase for necessario, que seja one app separado e explicito.
//
// Este app declara usa_sd = true, entao o SISTEMA monta o cartao antes of
// ao_abrir() e desmonta depois of ao_fechar(). Nao ha chamada of montagem aqui.

#include "app.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes in PSRAM
#include "tab5_screen.hpp"
#include "tab5_ui.hpp"
#include "paint.hpp"
#include "list.hpp"
#include "conversation.hpp"   // conversa_quebrar_linhas: o mesmo quebrador dos baloes
#include "esp_heap_caps.h"
#include <stdlib.h>
#include "work.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app.arquivos";

// Quantos items uma pasta mostra. Era 24 - e a pasta of logs ganha one arquivo
// por dia: no 25o dia, o log of HOJE (o ultimo que o readdir devolve) sumiria
// of lista sem aviso. A tabela mora in PSRAM so com o app open.
#define MAX_ITENS      512
// LEITURA COMO TEXTO CORRIDO. A screen mostrava 28 lines of arquivo cortadas
// em 96 bytes e desenhadas sem medir: line of log ou of CSV maior que a screen
// simplesmente saia pela right, e o end dela nao aparecia em lugar nenhum.
// Agora cada line of arquivo e QUEBRADA in width real (por palavra; o CSV
// ganha one espaco depois of cada separador para break no field), e a
// "pagina" e o que couber in screen - quantas lines of arquivo isso of so se
// sabe desenhando.
#define BLOCO_LINHAS   28    // granularidade of indice of deslocamentos (s_off)
#define LER_LINHAS     48    // lines of arquivo trazidas por leitura - mais of que cabem
#define LARG_LINHA     600   // bytes guardados of UMA line of arquivo; o resto vira "..."

// Mesmo atalho que app_mesh.cpp ja usa: "esta pass repinta everything?".
#define TUDO  paint_complete(&s_p)

typedef enum { NIVEL_PASTAS, NIVEL_ARQUIVOS, NIVEL_CONTEUDO } level_t;

typedef struct {
  char     name[40];
  int      arquivos;      // so para pasta
  uint64_t bytes;
  bool     eh_pasta;      // controla a pastinha/folhinha of icon - ver draw()
} item_t;

// ---- estado ----------------------------------------------------------------
static level_t  s_level = NIVEL_PASTAS;
static item_t*  s_items = nullptr;   // MAX_ITENS, PSRAM, alocada on open
static int      s_n_items = 0;
static int      s_n_achados = 0;      // quantos a pasta TEM (pode passar of MAX_ITENS)
// Caminho relativo a apps/, com subpastas: "Sistema" ou "Sistema/logs".
static char     s_pasta[96]   = "";
static char     s_arquivo[48] = "";

// A LISTA DE PASTAS/ARQUIVOS e uma kernel/list: rolagem,
// recorte, faixa exposta, barra e o "qual item foi touched" sao dela. O
// CONTEUDO of one arquivo open (NIVEL_CONTEUDO) nao rola - pagina por
// botoes, of proposito: arquivo of log tem megabytes, e rolar exigiria ter
// everything measured.
static list_t s_lista;
static int  item_height(int i, void* ctx);
static void item_draw(lgfx::LovyanGFX& d, int i, int x0, int y, int width, void* ctx);

EXT_RAM_BSS_ATTR static char s_lines[LER_LINHAS][LARG_LINHA + 1];
static int      s_n_lines = 0;
static int      s_line = 0;          // a primeira line of arquivo in screen (0 = a primeira)
static int      s_cabem = 1;          // quantas lines of arquivo a ultima pass coube (o passo of DEPOIS)
static int      s_total_lines = 0;
// De onde veio cada screen: o ANTES iteration exatamente on que se viu, mesmo com
// lines of tamanhos diferentes (o passo para tras nao e o mesmo of DEPOIS).
#define VOLTAS_MAX 64
static int      s_voltas[VOLTAS_MAX];
static int      s_n_voltas = 0;
// Tamanho of letra of text: o botao Aa troca entre estes (px nativos).
static const int TAM_LEITURA[] = { 24, 29, 36 };
static int      s_size_leitura = 1;

static uint64_t s_total = 0, s_livre = 0;

// ---- visualizacao of imagem (.bmp) -----------------------------------------
// o app Camera passou a salvar fotos em .bmp, e o
// Arquivos - que so sabia mostrar text/CSV line a line - nao tinha como
// open. So entende o PROPRIO formato que tab5_camera.cpp escreve (16 bits,
// RGB565, BI_BITFIELDS) - nao e one decodificador of bmp generico, e nao
// precisa ser: nenhum outro app grava imagem no cartao.
static bool     s_e_imagem = false;
static uint32_t s_img_width = 0, s_img_height = 0;
static long     s_img_offset_data = 0;
static uint32_t s_img_line_bytes = 0;
static bool     s_img_ok = false;

static bool tem_sufixo(const char* name, const char* sufixo)
{
  const size_t ln = strlen(name), ls = strlen(sufixo);
  return ln >= ls && strcasecmp(name + ln - ls, sufixo) == 0;
}

// Le so o cabecalho (BITMAPFILEHEADER + BITMAPINFOHEADER) - rapido, e o
// bastante para saber tamanho e onde os pixels comecam. A leitura of verdade
// e por LINHA, no drawing, para nao precisar of one buffer of tamanho of foto
// inteira (ver desenhar_imagem()).
static uint32_t read_u32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t read_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1]<<8)); }

static void open_cabecalho_imagem(const char* caminho)
{
  s_img_ok = false;
  FILE* f = fopen(caminho, "rb");
  if (!f) { return; }

  uint8_t cab[54];
  if (fread(cab, 1, sizeof(cab), f) != sizeof(cab) || cab[0] != 'B' || cab[1] != 'M') {
    fclose(f);
    return;
  }
  s_img_offset_data = (long)read_u32(cab + 10);
  s_img_width      = read_u32(cab + 18);
  s_img_height        = read_u32(cab + 22);
  const uint16_t bpp  = read_u16(cab + 28);
  fclose(f);

  if (bpp != 16 || s_img_width == 0 || s_img_height == 0 || s_img_height > 4096) { return; }
  s_img_line_bytes = ((s_img_width * 2 + 3) / 4) * 4;
  s_img_ok = true;
}

// Qual level foi drawn por ultimo. O sistema NAO limpa a area entre
// redesenhos - e of proposito, e o que mata o flickering. So que este app troca
// o layout inteiro on mudar of level, e o anterior ficaria por bottom: foi o que
// deu a screen com dois textos sobrepostos. Entao QUANDO O NIVEL MUDA, e so
// entao, o proprio app limpa a area uma vez.
static int s_level_drawn = -1;
static bool s_abriu_ok = true;

// ---------------------------------------------------------------------------
// LEITURA NO WORKER (kernel/work)
// ---------------------------------------------------------------------------
// Contar as lines (o energia.csv cresce sem limit) e read uma pagina eram
// feitos NA TAREFA DE INTERFACE - com a screen e o touch parados enquanto o
// cartao respondia. Agora o worker faz os dois, SO com o estado abaixo
// (s_c_*) e a tabela of deslocamentos (s_off, que so ele usa); a interface
// copia o resultado no ao_aviso. Caminho e pagina sao FOTOGRAFADOS no request:
// switch of arquivo ou virar pagina durante a leitura nao mistura nada.
static char          s_c_caminho[192];
static int           s_c_line = 0;
static bool          s_c_count = false;
static int           s_c_total = 0;
EXT_RAM_BSS_ATTR static char s_c_lines[LER_LINHAS][LARG_LINHA + 1];
static int           s_c_n = 0;
static bool          s_c_abriu = true;
static volatile bool s_c_pronto = false;
static bool          s_c_ocupado = false;     // ha request em andamento
static bool          s_c_of_novo = false;     // pediram outra leitura no middle
static bool          s_c_count_of_novo = false;

// LEITURA PENDENTE - nao read of cartao dentro of tratador of touch.
//
// O ler_pagina() percorre o arquivo ate a pagina requested e cede CPU a cada 256
// lines. Rodando dentro of ao_tocar(), cada touch bloqueava a tarefa of
// interface ANTES of qualquer drawing; tocando rapido, as leituras se
// enfileiravam e a screen alternava entre a pagina velha e a nova em ritmo
// irregular - que e a piscada on passar paginas depressa.
//
// Agora o touch so ANOTA o que quer, e a leitura acontece no draw(), junto
// of resto of work of screen. Mesmo default of app Mesh com o historico.
static bool s_precisa_read = false;
static paint_t s_p;   // paint por diferenca desta screen

// ---- geometria -------------------------------------------------------------
// O CABECALHO E O DO AJUSTES: "< VOLTAR" e o titulo in mesma faixa, a
// descricao embaixo e uma line separando of content. O anterior tinha o
// titulo (41 px) em y=72, a descricao (33 px) em y=104 e a fileira of saltos
// em y=128 - os tres se sobrepunham, e a descricao, pintada com fundo preto,
// comia o topo dos botoes. Aqui cada faixa tem height conhecida e a seguinte
// comeca onde ela acaba. Larguras dos botoes: FRACAO of area, nunca pixel
// fixo - a fileira cabe em qualquer width.
static const int MARGEM_X = 16;
static const int TOPO_Y = 16, TOPO_A = 64;           // VOLTAR + titulo
static const int VOLTAR_L = 200;
static const int AA_L = 96;                            // "Aa" (so no content)
static const int DESC_Y = 92;                          // a line of descricao (24 px)
static const int DIVISA_Y = 128;
static const int LISTA_Y = 140;                        // onde a lista comeca
// So no content: a fileira of navigation e a barra of position.
static const int NAV_Y = 140, NAV_A = 64, NAV_G = 10;
static const int BARRA_Y = 220, BARRA_A = 14;          // trilho drawn
static const int BARRA_TOQUE_Y = 204, BARRA_TOQUE_A = 48;   // faixa que aceita o touch
static const int TEXTO_Y = 252;                        // onde o text comeca
static const int ARRASTE_PAGINA = 90;                  // px of arraste vertical que viram a pagina
static const char* NAV_NOME[4] = { "START", "BACK", "FORWARD", "END" };
static int nav_l(int width) { return (width - 2 * MARGEM_X - 3 * NAV_G) / 4; }
static int nav_x(int width, int i) { return MARGEM_X + i * (nav_l(width) + NAV_G); }
// 48 -> 72 (+50%): com 48 px o touch errava o arquivo com frequencia.
static const int LIN_LISTA = 72;

// TRES TAMANHOS: titulo +100%, descricao e lista +60%; o
// content of CSV/text stays no BASE - maior apertaria LIN_TEXTO (30px).
static const float TAM_BASE   = 2.6f;             // +30%
static const float TAM_LISTA  = TAM_BASE * 1.6f;

static void caminho_pasta(char* out, size_t n)
{
  snprintf(out, n, "%s/%s", TAB5_SD_APPS, s_pasta);
}
static void caminho_arquivo(char* out, size_t n)
{
  snprintf(out, n, "%s/%s/%s", TAB5_SD_APPS, s_pasta, s_arquivo);
}

// ---------------------------------------------------------------------------
// Leitura
// ---------------------------------------------------------------------------
// stat() em vez of open/position/close. Listar uma pasta com 50 arquivos
// eram 50 aberturas of arquivo no cartao - lento e desnecessario, o tamanho
// esta in entrada of diretorio.
static uint64_t tamanho_of(const char* caminho)
{
  struct stat st;
  if (stat(caminho, &st) != 0) { return 0; }
  return (st.st_size > 0) ? (uint64_t)st.st_size : 0;
}

// ORDEM: pastas first, em order alfabetica; depois os arquivos of MAIS
// NOVO name para o mais velho - os arquivos of sistema sao datados
// (log_2026-09-23.txt), e o of hoje e o que se procura, nao o of mes passado.
static int comparar_items(const void* pa, const void* pb)
{
  const item_t* a = (const item_t*)pa;
  const item_t* b = (const item_t*)pb;
  if (a->eh_pasta != b->eh_pasta) { return a->eh_pasta ? -1 : 1; }
  return a->eh_pasta ? strcasecmp(a->name, b->name) : strcasecmp(b->name, a->name);
}

// Arquivos e bytes of uma pasta, SUBPASTAS INCLUSIVE - a pasta Sistema quase
// nao tem arquivo released (os logs e a energia moram em Sistema/logs e
// Sistema/energia), e count so o first level diria "3 arq, 2KB".
static void somar_pasta(const char* caminho, int level, int* arquivos, uint64_t* bytes)
{
  DIR* d = opendir(caminho);
  if (!d) { return; }
  struct dirent* e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') { continue; }
    char cheio[224];
    snprintf(cheio, sizeof(cheio), "%s/%s", caminho, e->d_name);
    if (e->d_type == DT_DIR) {
      if (level < 3) { somar_pasta(cheio, level + 1, arquivos, bytes); }
    } else {
      *bytes += tamanho_of(cheio);
      ++*arquivos;
    }
  }
  closedir(d);
}

// Toda listagem nova: items em order, lista re-medida e of iteration on topo.
// Quanto a listagem custou in tarefa of interface (o cartao e lido aqui, no
// touch). Acima of 150 ms o touch ja stays perceptivelmente preso: exit no log
// para sabermos se chegou a hora of mandar a listagem para o worker.
// A LISTAGEM E DO WORKER. Listar uma pasta (e somar o
// tamanho of cada subpasta, tres niveis abaixo) acontecia dentro of touch: a
// screen parava enquanto o cartao respondia - e a raiz, com o Sistema cheio of
// logs, levava centenas of ms. Agora o worker lista para s_carga (o
// buffer que a screen NAO desenha) e o ao_aviso troca os dois, como a leitura
// of content ja fazia.
static item_t*        s_carga = nullptr;          // MAX_ITENS, PSRAM
static volatile bool  s_listando = false;         // request feito, resultado ainda nao entrou
static volatile bool  s_l_pronto = false;
static bool           s_l_pastas = true, s_l_manter = false;
static bool           s_l_of_novo = false, s_l_of_novo_pastas = true, s_l_of_novo_manter = false;
static char           s_l_pasta[128];             // o caminho request (copia: o touch pode mudar s_pasta)
static volatile int   s_l_n = 0, s_l_achados = 0;
static uint64_t       s_l_total = 0, s_l_livre = 0;

static void listar_em(item_t* dst, const char* pasta, bool raiz)
{
  const int64_t t0 = esp_timer_get_time();
  int n = 0, achados = 0;
  if (raiz) { s_l_total = s_l_livre = 0; }
  if (tab5_sd_mounted()) {
    if (raiz) { tab5_sd_space((uint64_t*)&s_l_total, (uint64_t*)&s_l_livre); }
    DIR* d = opendir(pasta);
    if (d) {
      struct dirent* e;
      while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') { continue; }
        if (++achados > MAX_ITENS) { continue; }
        if (system_work_cancelled()) { break; }
        item_t* it = &dst[n];
        snprintf(it->name, sizeof(it->name), "%s", e->d_name);
        it->arquivos = 0;
        it->bytes    = 0;
        it->eh_pasta = (e->d_type == DT_DIR);
        char cheio[224];
        snprintf(cheio, sizeof(cheio), "%s/%s", pasta, e->d_name);
        if (it->eh_pasta) { somar_pasta(cheio, 1, &it->arquivos, &it->bytes); }
        else if (!raiz)   { it->bytes = tamanho_of(cheio); }
        ++n;
      }
      closedir(d);
    }
  }
  if (n > 1) { qsort(dst, n, sizeof(item_t), comparar_items); }
  s_l_n = n;
  s_l_achados = achados;
  const int64_t ms = (esp_timer_get_time() - t0) / 1000;
  if (ms > 150) { ESP_LOGW(TAG, "listagem de %d item(s) levou %lld ms", achados, (long long)ms); }
}

static void work_listar(void*)
{
  tab5_sd_flush_pending();   // tamanhos e arquivos novos: o que ja foi entregue on gravador
  listar_em(s_carga, s_l_pastas ? TAB5_SD_APPS : s_l_pasta, s_l_pastas);
  s_l_pronto = true;
}

// Pede a listagem of level current. 'manter_posicao': voltar of content para a
// mesma pasta nao deve jogar a lista para o topo.
static void request_listagem(bool pastas, bool manter_position)
{
  if (s_listando) {   // uma por vez: a ultima vontade vale
    s_l_of_novo = true; s_l_of_novo_pastas = pastas; s_l_of_novo_manter = manter_position;
    return;
  }
  if (!manter_position) { s_n_items = s_n_achados = 0; list_items_changed(&s_lista, 0); }
  s_l_pastas = pastas;
  s_l_manter = manter_position;
  caminho_pasta(s_l_pasta, sizeof(s_l_pasta));
  s_l_pronto = false;
  s_listando = system_work(work_listar, nullptr);
}

static void listar_pastas(bool manter_position = false)   { request_listagem(true, manter_position); }
static void listar_arquivos(bool manter_position = false) { request_listagem(false, manter_position); }

// No ao_aviso: o resultado entra in screen.
static void listagem_chegou(void)
{
  s_l_pronto = false;
  s_listando = false;
  item_t* t = s_items; s_items = s_carga; s_carga = t;
  s_n_items = s_l_n;
  s_n_achados = s_l_achados;
  if (s_l_pastas) { s_total = s_l_total; s_livre = s_l_livre; }
  list_items_changed(&s_lista, s_n_items);
  if (!s_l_manter) { list_on_start(&s_lista); }
  if (s_l_of_novo) {
    s_l_of_novo = false;
    request_listagem(s_l_of_novo_pastas, s_l_of_novo_manter);
  }
  system_request_redraw_complete();
}

#define PAG_OFF 256
static long s_off[PAG_OFF];

// PASSO DA TABELA: quantas paginas cada casa cobre.
//
// Era uma casa por pagina, o que cobria as 256 primeiras e deixava o resto no
// caminho lento - pular line a line since o start. Num energia.csv of uma
// semana (~700 paginas) ir on FIM lia o arquivo inteiro, e o botao "FIM" seria
// uma promessa que o app nao cumpre.
//
// Com passo, as mesmas 256 casas cobrem QUALQUER tamanho: a casa k guarda o
// start of pagina k*passo, e chegar numa pagina qualquer custa no maximum
// (passo-1) paginas of leitura sequencial. O custo vira limitado em vez of
// proporcional on arquivo.
static int  s_passo_off = 1;

static void esquecer_offsets(void)
{
  for (int i = 0; i < PAG_OFF; ++i) { s_off[i] = -1; }
  s_off[0] = 0;                 // a primeira pagina comeca no byte zero
  s_passo_off = 1;
}

// Conta as lines of arquivo. Feito UMA vez on open, nao a cada pagina.
static void count_lines(void)
{
  s_c_total = 0;
  FILE* f = fopen(s_c_caminho, "r");
  if (!f) { return; }

  // Em BLOCO, nao byte a byte. O fgetc() num arquivo of 1MB sao one milhao of
  // chamadas; com blocos of 1KB sao mil leituras e one memchr por bloco.
  //
  // Roda no worker of sistema (trabalho_ler). Cede a CPU of tempos em
  // tempos, e SAI se o app fechou (sistema_trabalho_cancelado): close o app
  // espera este work, e one log of varios MB seguraria a interface ate o
  // end of varredura.
  // DUAS PASSADAS SOBRE O MESMO ARQUIVO SERIAM UMA A MAIS.
  //
  // Esta varredura ja le everything para count as lines. Entao ela tambem ANOTA os
  // deslocamentos - o indice exit of graca, junto of contagem que ja acontecia.
  // Montar o indice depois, sob demanda, deixaria o first "FIM" caro; assim
  // ele nasce pronto.
  //
  // O passo so pode ser calculado no end (depende of total), entao a primeira
  // pass anota TODAS as fronteiras of pagina que couberem e a correcao vem
  // depois. Para arquivo of ate 256 paginas isso ja e o indice final.
  static long fronteira[PAG_OFF];
  int n_fronteiras = 0;
  fronteira[n_fronteiras++] = 0;

  static char bloco[1024];
  size_t lidos;
  int voltas = 0;
  long pos = 0;
  char ultimo = '\n';
  while ((lidos = fread(bloco, 1, sizeof(bloco), f)) > 0) {
    ultimo = bloco[lidos - 1];
    const char* p = bloco;
    const char* end = bloco + lidos;
    while ((p = (const char*)memchr(p, '\n', (size_t)(end - p))) != NULL) {
      s_c_total++;
      p++;
      // Fronteira of pagina: o byte seguinte a esta quebra inicia a next.
      if ((s_c_total % BLOCO_LINHAS) == 0 && n_fronteiras < PAG_OFF) {
        fronteira[n_fronteiras++] = pos + (long)(p - bloco);
      }
    }
    pos += (long)lidos;
    if ((++voltas % 64) == 0) {
      if (system_work_cancelled()) { fclose(f); s_c_total = 0; return; }
      vTaskDelay(1);
    }
  }
  fclose(f);
  if (ultimo != '\n') { s_c_total++; }   // a ultima line sem quebra tambem e line

  // Passo: quantas paginas cada casa cobre, para as PAG_OFF casas alcancarem
  // o arquivo inteiro. Arquivo pequeno stays com passo 1 e indice exato.
  const int paginas = (s_c_total > 0) ? (s_c_total + BLOCO_LINHAS - 1) / BLOCO_LINHAS : 1;
  s_passo_off = (paginas + PAG_OFF - 1) / PAG_OFF;
  if (s_passo_off < 1) { s_passo_off = 1; }

  for (int k = 0; k < PAG_OFF; ++k) {
    const int pag = k * s_passo_off;
    s_off[k] = (pag < n_fronteiras) ? fronteira[pag] : -1;
  }
}

// UMA LINHA INTEIRA of arquivo, ate o '\n' - o que passar of 'n' e lido e
// descartado. O fgets(256) of antes partia uma line longa em varias, e as
// partes contavam como lines: o "pular ate a line L" errava para a frente e
// a contagem (que conta '\n') nao batia com o que se via. false = end.
static bool read_line(FILE* f, char* buf, size_t n, bool* cortada)
{
  size_t k = 0;
  bool leu = false;
  *cortada = false;
  int c;
  while ((c = getc(f)) != EOF) {
    leu = true;
    if (c == '\n') { break; }
    if (k + 1 < n) { buf[k++] = (char)c; } else { *cortada = true; }
  }
  if (n) { buf[k] = 0; }
  return leu;
}

// O CSV quebra no field: one espaco depois of cada separador (que nao tenha),
// porque o quebrador of line so quebra em espaco. Sem isto uma line of CSV
// e uma "palavra" so, cortada por caractere no middle of one numero.
static void espacar_csv(const char* of, char* para, size_t n)
{
  size_t k = 0;
  for (const char* p = of; *p && k + 1 < n; ++p) {
    para[k++] = *p;
    if ((*p == ',' || *p == ';') && p[1] != ' ' && p[1] != 0 && k + 1 < n) { para[k++] = ' '; }
  }
  para[k] = 0;
}

// Carrega LER_LINHAS lines a partir of line s_c_linha. Vem mais of que cabe
// of proposito: quantas cabem so o drawing sabe (depende of quebra).
static void read_pagina(void)
{
  s_c_n = 0;
  const char* caminho = s_c_caminho;

  FILE* f = fopen(caminho, "r");
  if (!f) {
    // Distinguir isto of "arquivo vazio" importa: os dois davam a MESMA screen em
    // branco, e one deles e one arquivo of zero byte enquanto o outro e caminho
    // errado, cartao desmontado ou name que o FAT nao aceita. Sem separar, nao
    // ha como saber qual dos dois voce esta olhando.
    s_c_abriu = false;
    ESP_LOGW(TAG, "nao consegui abrir '%s'", caminho);
    return;
  }
  s_c_abriu = true;
  // Buffer of stdio maior que os 128 bytes default: o getc() of line a line
  // nao vira one read() of cartao a cada 128 bytes.
  setvbuf(f, nullptr, _IOFBF, 4096);
  const bool csv = tem_sufixo(s_c_caminho, ".csv");

  // Casa of indice mais next ANTES of line requested, e o resto lido em
  // sequencia (no maximum BLOCO_LINHAS * passo lines).
  const int bloco = s_c_line / BLOCO_LINHAS;
  const int casa = bloco / s_passo_off;
  int pular;
  if (casa < PAG_OFF && s_off[casa] >= 0) {
    fseek(f, s_off[casa], SEEK_SET);
    pular = s_c_line - casa * s_passo_off * BLOCO_LINHAS;
  } else {
    pular = s_c_line;   // sem indice: of start
  }

  static char line[LARG_LINHA + 1];
  bool cortada;
  {
    int voltas = 0;
    while (pular > 0 && read_line(f, line, sizeof(line), &cortada)) {
      pular--;
      if ((++voltas % 256) == 0) { vTaskDelay(1); }   // nao hold o core
    }
  }

  while (s_c_n < LER_LINHAS && read_line(f, line, sizeof(line), &cortada)) {
    const size_t k = strlen(line);
    if (k && line[k - 1] == '\r') { line[k - 1] = 0; }
    char* dst = s_c_lines[s_c_n];
    if (csv) { espacar_csv(line, dst, LARG_LINHA + 1); }
    else     { snprintf(dst, LARG_LINHA + 1, "%s", line); }
    if (cortada) {
      // Linha maior que o buffer: o end avisa que tem mais (sem partir UTF-8).
      size_t m = strlen(dst);
      if (m > LARG_LINHA - 4) { m = LARG_LINHA - 4; }
      while (m > 0 && ((unsigned char)dst[m] & 0xC0) == 0x80) { --m; }
      memcpy(dst + m, " ...", 5);
    }
    s_c_n++;
  }

  fclose(f);
}

// Roda NO WORKER.
static void work_read(void*)
{
  tab5_sd_flush_pending();   // o que o gravador ainda nao escreveu tem que aparecer aqui
  if (s_c_count) { esquecer_offsets(); count_lines(); }
  read_pagina();
  s_c_pronto = true;
}

// Na interface: fotografa o request e entrega on worker.
static void request_leitura(bool count)
{
  if (s_c_ocupado) {
    s_c_of_novo = true;
    s_c_count_of_novo = s_c_count_of_novo || count;
    return;
  }
  caminho_arquivo(s_c_caminho, sizeof(s_c_caminho));
  s_c_line   = s_line;
  s_c_count  = count;
  s_c_pronto  = false;
  s_c_ocupado = system_work(work_read, nullptr);
  if (!s_c_ocupado) { s_precisa_read = true; }   // queue cheia: tenta in next pass
}

// Na interface: a leitura terminou -> copia o que ela trouxe.
// ---- imagem lida pelo WORKER -----------------------------------------------
// Ler a foto of cartao dentro of draw() segurava a interface: 1,4 MB of
// SD line a line, com uma pausa a cada 8 lines - measured, 2,3 s of screen
// parada on open uma foto, e of novo a cada redraw complete (a foto era
// relida of cartao toda vez). Agora o worker le UMA vez, para one buffer in
// PSRAM ja in order of screen, e o draw() so pinta o buffer.
//
// O buffer e of app (work comum, nao released): close o app espera o
// worker, e so entao o ao_fechar() o libera.
static uint16_t*     s_img_px = nullptr;        // a imagem JA NA ESCALA of screen, of top para bottom
static uint32_t      s_img_px_l = 0, s_img_px_a = 0;   // tamanho of s_img_px
static char          s_img_px_caminho[192] = "";
static volatile bool s_img_lendo = false;       // o worker esta com o buffer
static volatile bool s_img_lida = false;        // o worker terminou (bem ou mal)
static volatile bool s_img_lida_ok = false;

// A IMAGEM INTEIRA, NA PROPORCAO DA TELA. A foto of Camera tem 1200x1600 e a
// area of app ~700x1000: em tamanho real, so one pedaco aparecia. Aqui a escala
// e a MAIOR que faz a imagem caber inteira (width e height), nunca acima of
// 1: imagem menor que a area stays no tamanho dela - aumentar so borraria.
//
// REDUZ LENDO, sem a foto inteira in memoria: o arquivo e lido UMA vez, em
// sequencia (one fseek so - read para tras estourou o watchdog uma vez), e cada
// line of saida e feita of line of origem correspondente com a of bottom,
// cada pixel a media of 2x2 vizinhos - o bastante para nao serrilhar numa
// reducao of ~0,6x. So tres lines of origem existem on mesmo time.
// BMP guarda of BAIXO para CIMA: a primeira line lida e a ultima of screen.
static inline uint16_t media4(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
  const uint32_t r  = ((a >> 11) + (b >> 11) + (c >> 11) + (d >> 11) + 2) >> 2;
  const uint32_t g  = (((a >> 5) & 0x3F) + ((b >> 5) & 0x3F) + ((c >> 5) & 0x3F) + ((d >> 5) & 0x3F) + 2) >> 2;
  const uint32_t bl = ((a & 0x1F) + (b & 0x1F) + (c & 0x1F) + (d & 0x1F) + 2) >> 2;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// LEITURA EM BLOCOS GRANDES, SEM O FILE* DO STDIO. Com fread, a foto of
// 3,8 MB levava ~10 s: o FILE* of newlib tem buffer of 128 bytes
// (CONFIG_FATFS_VFS_FSTAT_BLKSIZE=0), entao cada line virava dezenas of
// read() of 128 bytes e o FatFs buscava o cartao SETOR A SETOR - milhares of
// commands of 512 bytes, cada one com o custo fixo of protocolo of SD.
// Aqui o read() pede 64 KB of uma vez, sempre of uma position of arquivo
// alinhada a setor e para one buffer alinhado a line of cache: o FatFs entrega
// os setores inteiros direto no buffer e o SDMMC le varios setores por
// comando, por DMA, sem passar por buffer intermediario.
#define IMG_BLOCO       (64 * 1024)   // multiplo of 512 (setor)
#define IMG_ALINHAMENTO 128           // line of cache L2 (CONFIG_CACHE_L2_CACHE_LINE_128B)

typedef struct {
  int      fd;
  uint8_t* buf;
  size_t   n;     // bytes validos no buffer
  size_t   pos;   // next byte a entregar
} leitor_img_t;

// Entrega 'qtd' bytes em 'destino' (ou so pula, se destino == nullptr),
// buscando novos blocos of cartao when o current acaba.
static bool leitor_img_pegar(leitor_img_t& r, uint8_t* destino, size_t qtd)
{
  while (qtd > 0) {
    if (r.pos >= r.n) {
      const ssize_t lidos = read(r.fd, r.buf, IMG_BLOCO);
      if (lidos <= 0) { return false; }
      r.n = (size_t)lidos;
      r.pos = 0;
    }
    size_t parte = r.n - r.pos;
    if (parte > qtd) { parte = qtd; }
    if (destino) { memcpy(destino, r.buf + r.pos, parte); destino += parte; }
    r.pos += parte;
    qtd   -= parte;
  }
  return true;
}

static void work_read_imagem(void*)
{
  s_img_lida_ok = false;
  const int64_t t0 = esp_timer_get_time();
  const uint32_t W = s_img_width, H = s_img_height;
  const uint32_t L = s_img_px_l, A = s_img_px_a;
  const bool reduz = (L < W || A < H);
  uint16_t* current = (uint16_t*)heap_caps_malloc((size_t)W * 2, MALLOC_CAP_SPIRAM);
  uint16_t* bottom = (uint16_t*)heap_caps_malloc((size_t)W * 2, MALLOC_CAP_SPIRAM);   // a line of origem logo abaixo
  leitor_img_t r = { -1, nullptr, 0, 0 };
  r.buf = (uint8_t*)heap_caps_aligned_alloc(IMG_ALINHAMENTO, IMG_BLOCO, MALLOC_CAP_SPIRAM);
  if (current && bottom && r.buf) { r.fd = open(s_img_px_caminho, O_RDONLY); }
  int32_t o = (int32_t)A - 1;
  bool ok = false;
  if (r.fd >= 0) {
    // Comeca no start of SETOR onde estao os pixels e descarta o pedaco of
    // cabecalho: assim todo read() seguinte comeca alinhado a setor.
    const off_t start = (off_t)s_img_offset_data & ~(off_t)511;
    ok = lseek(r.fd, start, SEEK_SET) == start &&
         leitor_img_pegar(r, nullptr, (size_t)(s_img_offset_data - start));
    const size_t quer = (size_t)W * 2;
    const size_t pulo = s_img_line_bytes - quer;   // enchimento ate multiplo of 4
    // Linhas of SAIDA of bottom para top, acompanhando a leitura: a saida 'o'
    // nasce of origem (o * H) / A.
    for (uint32_t k = 0; ok && k < H && o >= 0; ++k) {
      const uint32_t origem = H - 1 - k;            // line of screen que acabou of ser lida
      uint16_t* t = bottom; bottom = current; current = t;   // a anterior lida e a of bottom
      if (!leitor_img_pegar(r, (uint8_t*)current, quer) || !leitor_img_pegar(r, nullptr, pulo)) { ok = false; break; }
      while (o >= 0 && ((uint32_t)o * H) / A == origem) {
        uint16_t* destino = s_img_px + (size_t)o * L;
        if (!reduz) {
          memcpy(destino, current, quer);
        } else {
          const uint16_t* of_bottom = (k > 0) ? bottom : current;   // a ultima line nao tem vizinha
          for (uint32_t x = 0; x < L; ++x) {
            const uint32_t sx  = (x * W) / L;
            const uint32_t sx2 = (sx + 1 < W) ? sx + 1 : sx;
            destino[x] = media4(current[sx], current[sx2], of_bottom[sx], of_bottom[sx2]);
          }
        }
        --o;
      }
      if ((k & 31) == 0 && system_work_cancelled()) { ok = false; break; }
    }
    close(r.fd);
  }
  s_img_lida_ok = ok && o < 0;
  ESP_LOGI(TAG, "imagem %ux%u -> %ux%u lida em %d ms (ok=%d)", (unsigned)W, (unsigned)H,
           (unsigned)L, (unsigned)A, (int)((esp_timer_get_time() - t0) / 1000), (int)s_img_lida_ok);
  if (r.buf) { heap_caps_free(r.buf); }
  if (current) { heap_caps_free(current); }
  if (bottom) { heap_caps_free(bottom); }
  s_img_lida = true;
}

static void release_imagem(void)
{
  if (s_img_lendo) { return; }   // o worker esta escrevendo nele: released when ele notify
  if (s_img_px) { heap_caps_free(s_img_px); s_img_px = nullptr; }
  s_img_px_l = s_img_px_a = 0;
  s_img_px_caminho[0] = '\0';
}

static void on_aviso(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_img_lendo && s_img_lida) {
    s_img_lendo = false;
    if (!s_img_lida_ok) { s_img_px_caminho[0] = '\0'; }   // le of novo se request
    if (s_level == NIVEL_CONTEUDO && s_e_imagem) { system_request_redraw_complete(); }
    else                                         { release_imagem(); }   // ja exited of foto
  }
  if ((av & AVISO_TRABALHO) && s_l_pronto) { listagem_chegou(); }
  if (!(av & AVISO_TRABALHO) || !s_c_pronto) { return; }
  s_c_pronto  = false;
  s_c_ocupado = false;

  char current[192];
  caminho_arquivo(current, sizeof(current));
  const bool mesmo_arquivo = (strcmp(current, s_c_caminho) == 0) && s_level == NIVEL_CONTEUDO;
  if (mesmo_arquivo) {
    if (s_c_count) { s_total_lines = s_c_total; }
    memcpy(s_lines, s_c_lines, sizeof(s_lines));
    s_n_lines = s_c_n;
    s_abriu_ok = s_c_abriu;
  }
  // Virou pagina (ou trocou of arquivo) durante a leitura: le of novo.
  if (s_c_of_novo || (mesmo_arquivo && s_c_line != s_line)) {
    const bool count = s_c_count_of_novo;
    s_c_of_novo = false;
    s_c_count_of_novo = false;
    request_leitura(count);
  }
  if (mesmo_arquivo) { system_request_redraw_complete(); }
}

static void draw_imagem(lgfx::LovyanGFX& d, const area_t& a, int x, int y)
{
  if (!s_img_ok) {
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.drawString("COULD NOT READ THIS IMAGE", x, y);
    return;
  }

  // A caixa disponivel (mesma margem dos lados) e a escala que faz caber.
  const int caixa_l = a.width - 2 * (x - a.x);
  const int caixa_a = (a.y + a.height - 10) - y;
  if (caixa_l <= 0 || caixa_a <= 0 || s_img_width > 4096) { return; }
  uint32_t L = s_img_width, A = s_img_height;
  if (L > (uint32_t)caixa_l || A > (uint32_t)caixa_a) {
    // Menor das duas razoes, em inteiros: compara caixa_l/W com caixa_a/H.
    if ((uint64_t)caixa_l * s_img_height <= (uint64_t)caixa_a * s_img_width) {
      L = (uint32_t)caixa_l;
      A = (uint32_t)(((uint64_t)s_img_height * caixa_l) / s_img_width);
    } else {
      A = (uint32_t)caixa_a;
      L = (uint32_t)(((uint64_t)s_img_width * caixa_a) / s_img_height);
    }
    if (L == 0) { L = 1; }
    if (A == 0) { A = 1; }
  }
  const int px = a.x + (a.width - (int)L) / 2;   // centralizada in caixa
  const int py = y + (caixa_a - (int)A) / 2;

  char caminho[192];
  caminho_arquivo(caminho, sizeof(caminho));

  // PRONTA: o buffer e desta foto, deste tamanho, e o worker terminou bem.
  if (!s_img_lendo && s_img_px && s_img_px_l == L && s_img_px_a == A && strcmp(s_img_px_caminho, caminho) == 0) {
    // "efeito arco-iris" - MESMO aviso of app Camera (ver app_camera.cpp): o
    // LovyanGFX espera a order of byte trocada em relacao on RGB565 cru que
    // esta gravado no arquivo. Estado global of display - salva e devolve.
    const bool troca_ant = d.getSwapBytes();
    // As margens em iteration of imagem centralizada: limpa o que estiver la (o
    // "loading imagem..." ficava aparecendo acima dela). Cada faixa uma vez,
    // sem paint duas vezes a area of imagem.
    const int cx0 = a.x, cx1 = a.x + a.width, cy1 = y + caixa_a;
    if (py > y)                  { d.fillRect(cx0, y, cx1 - cx0, py - y, TFT_BLACK); }
    if (py + (int)A < cy1)       { d.fillRect(cx0, py + (int)A, cx1 - cx0, cy1 - (py + (int)A), TFT_BLACK); }
    if (px > cx0)                { d.fillRect(cx0, py, px - cx0, (int)A, TFT_BLACK); }
    if (px + (int)L < cx1)       { d.fillRect(px + (int)L, py, cx1 - (px + (int)L), (int)A, TFT_BLACK); }
    d.setSwapBytes(true);
    d.pushImage(px, py, (int)L, (int)A, s_img_px);
    d.setSwapBytes(troca_ant);
    return;
  }

  // AINDA NAO: pede on worker (uma vez) e mostra que esta vindo.
  if (!s_img_lendo) {
    release_imagem();
    s_img_px = (uint16_t*)heap_caps_malloc((size_t)L * A * 2, MALLOC_CAP_SPIRAM);
    if (!s_img_px) {
      d.setTextColor(TFT_RED, TFT_BLACK);
      d.drawString("NOT ENOUGH MEMORY FOR THE IMAGE", x, y);
      return;
    }
    s_img_px_l = L;
    s_img_px_a = A;
    snprintf(s_img_px_caminho, sizeof(s_img_px_caminho), "%s", caminho);
    s_img_lida = false;
    s_img_lendo = system_work(work_read_imagem, nullptr);
    if (!s_img_lendo) { release_imagem(); }
  }
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString("carregando imagem...", x, y);
}

// ---------------------------------------------------------------------------
// Ciclo of vida
// ---------------------------------------------------------------------------
static bool on_open(void)
{
  esquecer_offsets();
  if (!s_items) { s_items = (item_t*)heap_caps_calloc(MAX_ITENS, sizeof(item_t), MALLOC_CAP_SPIRAM); }
  if (!s_carga) { s_carga = (item_t*)heap_caps_calloc(MAX_ITENS, sizeof(item_t), MALLOC_CAP_SPIRAM); }
  if (!s_items || !s_carga) { return false; }
  s_listando = false; s_l_pronto = false; s_l_of_novo = false;
  s_n_items = s_n_achados = 0;
  list_set(&s_lista, ROLAGEM_INICIO, 0, item_height, item_draw, nullptr);
  s_level = NIVEL_PASTAS;
  s_pasta[0] = s_arquivo[0] = '\0';
  s_line = 0;
  s_n_voltas = 0;
  listar_pastas();
  return true;
}

static void on_close(void)
{
  s_n_items = 0;
  if (s_items) { heap_caps_free(s_items); s_items = nullptr; }
  if (s_carga) { heap_caps_free(s_carga); s_carga = nullptr; }   // o fechamento ja esperou o worker
  s_n_lines = 0;
  s_e_imagem = false;
  s_img_ok = false;
  s_img_lendo = false;   // close o app ja esperou o worker
  release_imagem();
}

// ---------------------------------------------------------------------------
// Desenho
// ---------------------------------------------------------------------------
static void tamanho_txt(char* buf, size_t n, uint64_t b)
{
  if      (b >= 1024ULL*1024*1024) { snprintf(buf, n, "%6.2fGB", b / (1024.0*1024*1024)); }
  else if (b >= 1024ULL*1024)      { snprintf(buf, n, "%6.1fMB", b / (1024.0*1024)); }
  else if (b >= 1024ULL)           { snprintf(buf, n, "%6.1fKB", b / 1024.0); }
  else                             { snprintf(buf, n, "%6llu B", (unsigned long long)b); }
}

// Pastinha PEQUENA on lado of name of uma pasta, para distinguir pasta of
// arquivo in mesma lista.
// Mesmas cores/order of drawing of icon grande of screen inicial
// (icone_arquivos, mais abaixo: aba first, body por top cobrindo a
// junta), so sem o fundo quadrado - aqui ela senta direto no preto of line,
// nao numa lade of app.
static void icon_pasta_pequena(lgfx::LovyanGFX& d, int x, int y, int w, int h)
{
  const uint16_t pasta = d.color565(240, 190, 90);
  const uint16_t aba   = d.color565(205, 155, 60);
  const int aba_l = (int)(0.5f  * w);
  const int aba_a = (int)(0.28f * h);
  d.fillRoundRect(x, y,                 aba_l, aba_a,          2, aba);
  d.fillRoundRect(x, y + aba_a - 1,     w,     h - aba_a + 1,  3, pasta);
}

// Folha PEQUENA para arquivo comum: sem ela a coluna of icon ficaria vazia
// nas lines of arquivo (a pastinha so aparece when eh_pasta). Pagina
// clara com o canto top direito dobrado (leitura universal of "isto e
// one arquivo") + duas lines finas representando text generico dentro dela -
// a MESMA ideia of camada of pastinha (recorte, depois a dobra por top).
static void icon_arquivo_pequeno(lgfx::LovyanGFX& d, int x, int y, int w, int h)
{
  const uint16_t pagina = TFT_LIGHTGREY;
  const uint16_t dobra  = TFT_DARKGREY;

  const int pl = (int)(0.55f * w);
  const int pa = (int)(0.85f * h);
  const int px = x + (w - pl) / 2;
  const int py = y + (h - pa) / 2;
  const int canto = (pl < pa ? pl : pa) / 3;

  d.fillRoundRect(px, py, pl, pa, 2, pagina);
  // RECORTE of canto: apaga o quadradinho top direito of iteration on preto
  // of line, deixando so o triangulo diagonal of pagina.
  d.fillTriangle(px + pl - canto, py, px + pl, py, px + pl, py + canto, TFT_BLACK);
  // DOBRA: preenche o mesmo canto so que com o triangulo espelhado - a
  // "pontinha" que sobraria dobrada para dentro of folha.
  d.fillTriangle(px + pl - canto, py, px + pl, py + canto, px + pl - canto, py + canto, dobra);

  const int line_x = px + (int)(0.18f * pl);
  const int line_l = (int)(0.64f * pl);
  d.fillRect(line_x, py + (int)(0.55f * pa), line_l, 2, dobra);
  d.fillRect(line_x, py + (int)(0.72f * pa), line_l, 2, dobra);
}

// ---------------------------------------------------------------------------
// UM ITEM DA LISTA (pasta ou arquivo)
// ---------------------------------------------------------------------------
static int item_height(int, void*) { return LIN_LISTA; }

// x = borda left of area; width = ate a coluna of barra. Colunas em X
// proporcional a width (a area muda com a rotation), nao pixel chutado.
static void item_draw(lgfx::LovyanGFX& d, int i, int x0, int y, int width, void*)
{
  const item_t& it = s_items[i];
  const int MARGEM = 16;
  const int x = x0 + MARGEM;
  const int util = width - MARGEM;
  const int contagem_x = x + (int)(util * 0.55f);
  const int tamanho_x  = x + (int)(util * 0.78f);

  tab5_text(d, TAM_LISTA);
  d.setTextDatum(top_left);

  // ICONE ANCORADO NO TEXTO: mesma height e mesmo (x,y) que o
  // name usa - d.fontHeight(), nunca one numero chutado. Toda line ganha
  // icon (pasta ou folha), entao o name sempre comeca in mesma coluna.
  const int icon_l = 40;
  const int icon_a = d.fontHeight();
  if (it.eh_pasta) { icon_pasta_pequena(d, x, y, icon_l, icon_a); }
  else             { icon_arquivo_pequeno(d, x, y, icon_l, icon_a); }

  // Nome cortado MEDINDO (d.textWidth), para nao invadir a coluna seguinte.
  const int name_x = x + icon_l + 14;
  const int name_max_l = contagem_x - name_x - 12;
  char name[40];
  snprintf(name, sizeof(name), "%s", it.name);
  tab5_cortar_on_caber(d, name, name_max_l);   // por caractere: name of pasta com acento
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(name, name_x, y);

  char buf[24];
  if (it.eh_pasta) {
    snprintf(buf, sizeof(buf), "%d arq", it.arquivos);
    d.drawString(buf, contagem_x, y);
  }
  tamanho_txt(buf, sizeof(buf), it.bytes);
  d.drawString(buf, tamanho_x, y);

  // Linha fina entre items - nao depois
  // of ultimo.
  if (i + 1 < s_n_items) {
    d.drawFastHLine(x0 + MARGEM, y + LIN_LISTA - 16, width - MARGEM, TFT_DARKGREY);
  }
}

// Texto numa line so, cortado MEDINDO para caber em 'width' (com "..."),
// sem partir one caractere UTF-8.
static void text_cabendo(lgfx::LovyanGFX& d, const char* txt, int x, int y, int width)
{
  char buf[192];
  snprintf(buf, sizeof(buf), "%s", txt);
  tab5_cortar_com_reticencias(d, buf, sizeof(buf), width);
  d.drawString(buf, x, y);
}

// "1.234" - numero grande of lines se le melhor com milhar.
static void milhar(char* out, size_t n, int v)
{
  if (v >= 1000000) { snprintf(out, n, "%d.%03d.%03d", v / 1000000, (v / 1000) % 1000, v % 1000); }
  else if (v >= 1000) { snprintf(out, n, "%d.%03d", v / 1000, v % 1000); }
  else { snprintf(out, n, "%d", v); }
}

// O TEXTO: cada line of arquivo quebrada in width; continuacao recuada,
// e one respiro entre uma line of arquivo e a seguinte - assim se ve onde uma
// line longa acaba. Para when a next nao cabe inteira (a nao ser a
// primeira, que aparece no que couber: uma line maior que a screen nao pode
// lock a leitura). Devolve quantas lines of arquivo couberam.
static int draw_text(lgfx::LovyanGFX& d, const area_t& a, int y)
{
  const int px = TAM_LEITURA[s_size_leitura];
  d.setFont(tab5_fonte_suave(false, px));
  d.setTextSize(1);
  d.setTextDatum(top_left);
  const int alt_line = px + px / 4;
  const int respiro = px / 3;
  const int recuo = px;
  const int x = a.x + MARGEM_X;
  const int width = a.width - 2 * MARGEM_X;
  const int fundo = a.y + a.height - 10;

  static char vis[CONVERSA_MAX_LINHAS][CONVERSA_LINHA_BYTES];
  int cabem = 0;
  for (int i = 0; i < s_n_lines; ++i) {
    const char* txt = s_lines[i][0] ? s_lines[i] : " ";
    // Primeira line visual in width toda, as seguintes recuadas: quebra
    // first in width menor (o recuo) e redesenha a primeira sem ele.
    const int n = conversation_break_lines(d, txt, width - recuo, vis, CONVERSA_MAX_LINHAS);
    const int height = n * alt_line;
    if (y + height > fundo && cabem > 0) { break; }
    for (int k = 0; k < n; ++k) {
      if (y + alt_line > fundo) { break; }
      d.setTextColor(k == 0 ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
      d.drawString(vis[k], k == 0 ? x : x + recuo, y);
      y += alt_line;
    }
    y += respiro;
    ++cabem;
    if (y >= fundo) { break; }
  }
  if (s_n_lines == 0) {
    d.setTextColor(s_abriu_ok ? TFT_LIGHTGREY : TFT_RED, TFT_BLACK);
    d.drawString(s_abriu_ok ? "(empty file or end of content)"
                             : "COULD NOT OPEN - check the path in the log", x, y);
    y += alt_line;
    if (!s_abriu_ok) {
      char cam[192];
      caminho_arquivo(cam, sizeof(cam));
      d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      text_cabendo(d, cam, x, y, width);
      y += alt_line;
    }
  }
  // Apaga o resto, para screen curta nao deixar resto of anterior.
  if (y < fundo) { tab5_screen_clear(a.x, y, a.width, a.y + a.height - y, TFT_BLACK); }
  tab5_fonte_body(d);
  return cabem > 0 ? cabem : 1;
}

// Barra of position: onde a screen esta no arquivo. Tocar nela pula para la.
static void draw_barra(lgfx::LovyanGFX& d, const area_t& a)
{
  const int x = a.x + MARGEM_X, l = a.width - 2 * MARGEM_X;
  const int y = a.y + BARRA_Y;
  d.fillRoundRect(x, y, l, BARRA_A, BARRA_A / 2, d.color565(50, 50, 62));
  if (s_total_lines <= 0) { return; }
  int ini = (int)((int64_t)l * s_line / s_total_lines);
  int end = (int)((int64_t)l * (s_line + s_cabem) / s_total_lines);
  if (end > l) { end = l; }
  if (end - ini < BARRA_A) { ini = (end - BARRA_A > 0) ? end - BARRA_A : 0; end = ini + BARRA_A; }
  d.fillRoundRect(x + ini, y, end - ini, BARRA_A, BARRA_A / 2, TFT_SKYBLUE);
}

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  char line[128], t1[16], t2[16], t3[16];
  const int x = a.x + MARGEM_X;

  // Datum e tamanho SEMPRE: sao estado global of display e a barra of status
  // desenha entre duas passes daqui. Ver o aviso no app_config.cpp.
  tab5_text(d, TAM_BASE);
  d.setTextDatum(top_left);

  paint_start(&s_p);

  // A leitura of cartao acontece AQUI, nao no touch. Ver s_precisa_ler.
  if (s_precisa_read) { s_precisa_read = false; request_leitura(false); }

  s_level_drawn = (int)s_level;

  if (!tab5_sd_mounted()) {
    tab5_text(d, 3.625f);
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.drawString("CARD NOT MOUNTED        ", x, a.y + 24);
    tab5_text(d, 2);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString("Check that it is seated and formatted FAT32.", x, a.y + 90);
    d.drawString("O motivo exato sai no log (sd).  ", x, a.y + 120);
    return;
  }

  const bool text = (s_level == NIVEL_CONTEUDO && !s_e_imagem);

  // ---- cabecalho: [< VOLTAR] titulo [Aa] ------------------------------------
  // Sempre numa pass complete (toda troca of level/pagina pede uma): o
  // sistema ja limpou a area, nada aqui precisa width fixa.
  int titulo_x = x;
  int titulo_end = a.x + a.width - MARGEM_X;
  if (s_level != NIVEL_PASTAS) {
    tab5_botao(d, x, a.y + TOPO_Y, VOLTAR_L, TOPO_A, "< VOLTAR", TFT_SKYBLUE);
    titulo_x = x + VOLTAR_L + 20;
  }
  if (text) {
    tab5_botao(d, titulo_end - AA_L, a.y + TOPO_Y, AA_L, TOPO_A, "Aa", d.color565(70, 70, 86), TFT_WHITE);
    titulo_end -= AA_L + 16;
  }
  tab5_titulo(d, 36);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  text_cabendo(d, (s_level == NIVEL_PASTAS) ? (tab5_sd_name()[0] ? tab5_sd_name() : "Card") : (s_level == NIVEL_ARQUIVOS) ? s_pasta : s_arquivo,
                titulo_x, a.y + TOPO_Y + TOPO_A / 2, titulo_end - titulo_x);
  d.setTextDatum(top_left);

  // ---- descricao ------------------------------------------------------------
  line[0] = '\0';
  if (s_level == NIVEL_PASTAS) {
    tamanho_txt(t1, sizeof(t1), (s_total > s_livre) ? (s_total - s_livre) : 0);
    tamanho_txt(t2, sizeof(t2), s_total);
    snprintf(line, sizeof(line), "%s | usado %s de %s", tab5_sd_description(), t1, t2);
  } else if (s_level == NIVEL_ARQUIVOS) {
    if (s_n_achados > s_n_items) {
      snprintf(line, sizeof(line), "%d de %d item(s) - toque para abrir", s_n_items, s_n_achados);
    } else {
      snprintf(line, sizeof(line), "%d item(s) - toque para abrir", s_n_items);
    }
  } else if (s_e_imagem) {
    if (s_img_ok) { snprintf(line, sizeof(line), "imagem %ux%u", (unsigned)s_img_width, (unsigned)s_img_height); }
    else          { snprintf(line, sizeof(line), "could not read this image"); }
  }
  // (a descricao of text depende of quantas lines couberam: exit depois dele)
  d.setFont(tab5_fonte_suave(false, 24));
  d.setTextSize(1);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (!text) { text_cabendo(d, line, x, a.y + DESC_Y, a.width - 2 * MARGEM_X); }
  d.drawFastHLine(a.x, a.y + DIVISA_Y, a.width, d.color565(60, 60, 72));
  tab5_text(d, TAM_BASE);

  // ---- body ----------------------------------------------------------------
  if (s_level == NIVEL_CONTEUDO) { list_hide(&s_lista); }   // o content nao usa a lista
  if (s_level == NIVEL_CONTEUDO && s_e_imagem) {
    draw_imagem(d, a, x, a.y + LISTA_Y);
    return;
  }

  if (text) {
    // O text first: e ele que diz quantas lines couberam - e disso
    // depende o DEPOIS estar active e o tamanho of marca in barra.
    s_cabem = draw_text(d, a, a.y + TEXTO_Y);
    const int L = a.width;
    for (int i = 0; i < 4; ++i) {
      const bool active = (i < 2) ? s_line > 0 : s_line + s_cabem < s_total_lines;
      tab5_botao(d, a.x + nav_x(L, i), a.y + NAV_Y, nav_l(L), NAV_A, NAV_NOME[i],
                 active ? ((i == 0 || i == 3) ? TFT_DARKCYAN : TFT_SKYBLUE) : d.color565(60, 60, 72),
                 active ? TFT_BLACK : TFT_DARKGREY);
    }
    draw_barra(d, a);
    // Descricao: que lines estao in screen, of quantas.
    milhar(t1, sizeof(t1), s_line + 1);
    int ult = s_line + s_cabem;
    if (ult > s_total_lines) { ult = s_total_lines; }
    milhar(t2, sizeof(t2), ult);
    milhar(t3, sizeof(t3), s_total_lines);
    const int pct = s_total_lines > 0 ? (int)((int64_t)ult * 100 / s_total_lines) : 100;
    if (s_total_lines <= 0) { snprintf(line, sizeof(line), s_c_ocupado ? "lendo..." : "vazio"); }
    else { snprintf(line, sizeof(line), "linhas %s-%s de %s  (%d%%)", t1, t2, t3, pct); }
    d.setFont(tab5_fonte_suave(false, 24));
    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    text_cabendo(d, line, x, a.y + DESC_Y, a.width - 2 * MARGEM_X);
    tab5_text(d, TAM_BASE);
    return;
  }

  tab5_text(d, TAM_LISTA);   // +60%
  const int y = a.y + LISTA_Y;

  if (s_n_items == 0) {
    list_hide(&s_lista);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(s_listando ? "reading the card...      " : "(empty)                  ", x, y);
    return;
  }

  // A LISTA: everything o que sobra abaixo of cabecalho, ate 10px of rodape.
  const area_t window = { a.x, y, a.width, (a.y + a.height - 10) - y };
  list_draw(&s_lista, d, window, TFT_BLACK, TUDO);
}

// ---------------------------------------------------------------------------
// Toque
// ---------------------------------------------------------------------------
// Vai para a line 'nova' of arquivo. 'lembrar': guardar a screen current para o
// ANTES voltar a ela (so o DEPOIS lembra; saltos zeram o caminho).
static void ir_para(int nova, bool lembrar)
{
  if (nova > s_total_lines - 1) { nova = s_total_lines - 1; }
  if (nova < 0) { nova = 0; }
  if (nova == s_line) { return; }
  if (lembrar) {
    if (s_n_voltas == VOLTAS_MAX) { memmove(s_voltas, s_voltas + 1, sizeof(int) * (VOLTAS_MAX - 1)); --s_n_voltas; }
    s_voltas[s_n_voltas++] = s_line;
  } else {
    s_n_voltas = 0;
  }
  s_line = nova;
  s_precisa_read = true;
  system_request_redraw_complete();
}

static void pagina_seguinte(void)
{
  if (s_line + s_cabem < s_total_lines) { ir_para(s_line + s_cabem, true); }
}

static void pagina_anterior(void)
{
  if (s_line <= 0) { return; }
  int iteration;
  if (s_n_voltas > 0) { iteration = s_voltas[--s_n_voltas]; }
  else                { iteration = s_line - s_cabem; }   // chegou por salto: o mesmo tanto para tras
  if (iteration < 0) { iteration = 0; }
  s_line = iteration;
  s_precisa_read = true;
  system_request_redraw_complete();
}

// ARRASTE: in lista (pastas/arquivos) rola; no text, drag para top
// vira para a pagina seguinte, para bottom iteration - como folhear.
static int  s_arr_y0 = -1;
static bool s_arr_valeu = false;
static void on_touch_level(int x, int y, bool pressed)
{
  if (s_level == NIVEL_CONTEUDO) {
    if (s_e_imagem) { return; }
    if (pressed) {
      if (s_arr_y0 < 0) { s_arr_y0 = (y >= TEXTO_Y) ? y : -2; s_arr_valeu = false; }   // so arraste que comeca no text
      else if (s_arr_y0 >= 0 && !s_arr_valeu) {
        const int dy = y - s_arr_y0;
        // Um arraste vira UMA pagina; a soltura dele nao vira click.
        if (dy <= -ARRASTE_PAGINA) { s_arr_valeu = true; system_consume_touch(); pagina_seguinte(); }
        else if (dy >= ARRASTE_PAGINA) { s_arr_valeu = true; system_consume_touch(); pagina_anterior(); }
      }
    } else {
      s_arr_y0 = -1;
    }
    return;
  }
  if (list_drag(&s_lista, x, y, pressed)) { system_request_redraw(); }
}

static void on_touch(int x, int y)
{
  const area_t a = system_area();
  // ---- VOLTAR ---------------------------------------------------------------
  if (s_level != NIVEL_PASTAS && tab5_dentro(x, y, MARGEM_X, TOPO_Y, VOLTAR_L, TOPO_A)) {
    if (s_level == NIVEL_CONTEUDO) {
      // De iteration a pasta NO MESMO PONTO of lista: quem abriu o 40o log e
      // voltou quer ver o 41o, nao o topo.
      s_level = NIVEL_ARQUIVOS;
      release_imagem();   // 1,4 MB of foto nao ficam presos in lista
      listar_arquivos(true);
    } else if (char* barra = strrchr(s_pasta, '/')) {
      // Dentro of uma subpasta: VOLTAR sobe one level of pasta, nao para a raiz.
      *barra = '\0';
      listar_arquivos();
    } else {
      s_level = NIVEL_PASTAS;
      listar_pastas();
    }
    system_request_redraw_complete();
    return;
  }

  if (s_level == NIVEL_CONTEUDO && s_e_imagem) { return; }   // so o VOLTAR existe numa imagem

  if (s_level == NIVEL_CONTEUDO) {
    // Aa: o next tamanho of letra. A line of topo continua a mesma.
    if (tab5_dentro(x, y, a.width - MARGEM_X - AA_L, TOPO_Y, AA_L, TOPO_A)) {
      s_size_leitura = (s_size_leitura + 1) % (int)(sizeof(TAM_LEITURA) / sizeof(TAM_LEITURA[0]));
      system_request_redraw_complete();
      return;
    }
    if (y >= NAV_Y && y < NAV_Y + NAV_A) {
      for (int i = 0; i < 4; ++i) {
        if (x < nav_x(a.width, i) || x >= nav_x(a.width, i) + nav_l(a.width)) { continue; }
        switch (i) {
          case 0: ir_para(0, false); break;
          case 1: pagina_anterior(); break;
          case 2: pagina_seguinte(); break;
          // FIM: a ultima screen - aproximada pelo tanto que cabe now.
          default: ir_para(s_total_lines - (s_cabem > 1 ? s_cabem : 1), false); break;
        }
        return;
      }
      return;
    }
    // Barra: a position of finger vira a line.
    if (y >= BARRA_TOQUE_Y && y < BARRA_TOQUE_Y + BARRA_TOQUE_A && s_total_lines > 0) {
      const int l = a.width - 2 * MARGEM_X;
      int px = x - MARGEM_X;
      if (px < 0) { px = 0; }
      if (px > l) { px = l; }
      ir_para((int)((int64_t)s_total_lines * px / l), false);
      return;
    }
    return;   // no text, touch released nao faz nada (drag vira a pagina)
  }

  // ---- lines of lista ----------------------------------------------------
  // A lista responde "qual item" com a MESMA geometria of drawing, rolagem
  // incluida, e devolve LISTA_ARRASTE se o touch foi o end of uma rolagem.
  if (s_listando) { return; }   // a lista in screen esta para ser trocada
  const int i = list_item_touched(&s_lista, x, y, nullptr);
  if (i < 0) { return; }

  if (s_level == NIVEL_PASTAS) {
    snprintf(s_pasta, sizeof(s_pasta), "%s", s_items[i].name);
    s_level = NIVEL_ARQUIVOS;
    listar_arquivos();
  } else if (s_items[i].eh_pasta) {
    // SUBPASTA (Sistema/logs, Sistema/energia): entra nela e
    // continua no mesmo level of listagem; o VOLTAR sobe of iteration.
    const size_t n = strlen(s_pasta);
    if (n + 1 + strlen(s_items[i].name) < sizeof(s_pasta)) {
      snprintf(s_pasta + n, sizeof(s_pasta) - n, "/%s", s_items[i].name);
      listar_arquivos();
    }
  } else {
    snprintf(s_arquivo, sizeof(s_arquivo), "%s", s_items[i].name);
    s_level = NIVEL_CONTEUDO;
    s_line = 0;
    s_cabem = 1;
    s_n_voltas = 0;
    s_arr_y0 = -1;
    s_arr_valeu = false;
    s_e_imagem = tem_sufixo(s_arquivo, ".bmp");
    if (s_e_imagem) {
      char caminho[192];
      caminho_arquivo(caminho, sizeof(caminho));
      open_cabecalho_imagem(caminho);
      s_total_lines = 0;
      ESP_LOGI(TAG, "abrindo imagem %s/%s (%ux%u, ok=%d)", s_pasta, s_arquivo,
               (unsigned)s_img_width, (unsigned)s_img_height, (int)s_img_ok);
    } else {
      // ARQUIVO NOVO, TABELA NOVA. Sem isto os deslocamentos of arquivo
      // anterior seriam usados neste - e o fseek levaria on middle of outra
      // coisa, mostrando content errado sem nenhum erro in screen.
      // (A tabela of deslocamentos e refeita pelo worker, junto of
      // contagem - ver trabalho_ler.)
      s_total_lines = 0;
      s_n_lines = 0;
      request_leitura(true);
      ESP_LOGI(TAG, "abrindo %s/%s", s_pasta, s_arquivo);
    }
  }
  system_request_redraw_complete();
}

// 'extern' e OBRIGATORIO: em C++ one const no escopo of arquivo tem ligacao
// interna, e o apps.cpp nao enxergaria este simbolo.
// ---------------------------------------------------------------------------
// ICONE DA TELA INICIAL
// ---------------------------------------------------------------------------
// Primitivas e fracao of lado, como a marca of Mesh: exit limpo em qualquer
// tamanho, nao gasta flash e nao serrilha. A letra grande que estava aqui era
// a fonte embutida ampliada 6x - cada pixel dela virava one quadrado of 6.
static void icon_arquivos(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(30, 48, 70);
  const uint16_t pasta = d.color565(240, 190, 90);
  const uint16_t aba   = d.color565(205, 155, 60);

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  #define IX(f) (x + (int)((f) * lado))
  #define IY(f) (y + (int)((f) * lado))

  // ORDEM IMPORTA: aba, folha, body. Cada camada cobre a junta of anterior,
  // e a pasta exit como peca unica em vez of retangulos encostados. A folha no
  // middle e o que diz que ha coisa DENTRO - desenhada por ULTIMO ela viraria
  // uma etiqueta colada por top, que e outra leitura.
  d.fillRoundRect(IX(0.16f), IY(0.26f), (int)(0.34f * lado), (int)(0.14f * lado),
                  lado / 22, aba);
  d.fillRoundRect(IX(0.26f), IY(0.30f), (int)(0.48f * lado), (int)(0.10f * lado),
                  lado / 30, d.color565(245, 245, 245));
  d.fillRoundRect(IX(0.16f), IY(0.36f), (int)(0.68f * lado), (int)(0.40f * lado),
                  lado / 16, pasta);

  #undef IX
  #undef IY
}

extern const app_t APP_FILES = {
  .name  = "Files",
  .color   = TFT_SKYBLUE,
  .profile = { },
  .uses_sd = true,
  .requires_sd = true,         // sem cartao nao ha o que fazer (ver app.h)
  .piso_sleep_mhz = 0,
  .on_open  = on_open,
  .on_close = on_close,
  .draw  = draw,
  .on_touch  = on_touch,
  .on_touch_level = on_touch_level,
  .periodo_ms = 0,
  .notices = AVISO_TRABALHO,
  .on_aviso = on_aviso,
  .icon = icon_arquivos,
};
