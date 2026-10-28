// APP: Arquivos - navegador de LEITURA do cartao.
//
// Tres niveis: pastas dos aplicativos -> arquivos da pasta -> conteudo do
// arquivo. So le. Nao copia, nao move, nao apaga - de proposito, para nao
// existir a possibilidade de perder dado por um toque errado. O dia em que
// apagar for necessario, que seja um app separado e explicito.
//
// Este app declara usa_sd = true, entao o SISTEMA monta o cartao antes do
// ao_abrir() e desmonta depois do ao_fechar(). Nao ha chamada de montagem aqui.

#include "app.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes na PSRAM
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

// Quantos itens uma pasta mostra. Era 24 - e a pasta de logs ganha um arquivo
// por dia: no 25o dia, o log de HOJE (o ultimo que o readdir devolve) sumiria
// da lista sem aviso. A tabela mora na PSRAM so com o app aberto.
#define MAX_ITENS      512
// LEITURA COMO TEXTO CORRIDO. A tela mostrava 28 linhas do arquivo cortadas
// em 96 bytes e desenhadas sem medir: linha de log ou de CSV maior que a tela
// simplesmente saia pela direita, e o fim dela nao aparecia em lugar nenhum.
// Agora cada linha do arquivo e QUEBRADA na largura real (por palavra; o CSV
// ganha um espaco depois de cada separador para quebrar no campo), e a
// "pagina" e o que couber na tela - quantas linhas do arquivo isso da so se
// sabe desenhando.
#define BLOCO_LINHAS   28    // granularidade do indice de deslocamentos (s_off)
#define LER_LINHAS     48    // linhas do arquivo trazidas por leitura - mais do que cabem
#define LARG_LINHA     600   // bytes guardados de UMA linha do arquivo; o resto vira "..."

// Mesmo atalho que app_mesh.cpp ja usa: "esta passada repinta tudo?".
#define TUDO  pintura_completa(&s_p)

typedef enum { NIVEL_PASTAS, NIVEL_ARQUIVOS, NIVEL_CONTEUDO } nivel_t;

typedef struct {
  char     name[40];
  int      arquivos;      // so para pasta
  uint64_t bytes;
  bool     eh_pasta;      // controla a pastinha/folhinha do icone - ver desenhar()
} item_t;

// ---- estado ----------------------------------------------------------------
static nivel_t  s_nivel = NIVEL_PASTAS;
static item_t*  s_itens = nullptr;   // MAX_ITENS, PSRAM, alocada ao abrir
static int      s_n_itens = 0;
static int      s_n_achados = 0;      // quantos a pasta TEM (pode passar de MAX_ITENS)
// Caminho relativo a apps/, com subpastas: "Sistema" ou "Sistema/logs".
static char     s_pasta[96]   = "";
static char     s_arquivo[48] = "";

// A LISTA DE PASTAS/ARQUIVOS e uma kernel/list: rolagem,
// recorte, faixa exposta, barra e o "qual item foi tocado" sao dela. O
// CONTEUDO de um arquivo aberto (NIVEL_CONTEUDO) nao rola - pagina por
// botoes, de proposito: arquivo de log tem megabytes, e rolar exigiria ter
// tudo medido.
static lista_t s_lista;
static int  item_altura(int i, void* ctx);
static void item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void* ctx);

EXT_RAM_BSS_ATTR static char s_linhas[LER_LINHAS][LARG_LINHA + 1];
static int      s_n_linhas = 0;
static int      s_linha = 0;          // a primeira linha do arquivo na tela (0 = a primeira)
static int      s_cabem = 1;          // quantas linhas do arquivo a ultima passada coube (o passo do DEPOIS)
static int      s_total_linhas = 0;
// De onde veio cada tela: o ANTES volta exatamente ao que se viu, mesmo com
// linhas de tamanhos diferentes (o passo para tras nao e o mesmo do DEPOIS).
#define VOLTAS_MAX 64
static int      s_voltas[VOLTAS_MAX];
static int      s_n_voltas = 0;
// Tamanho da letra do texto: o botao Aa troca entre estes (px nativos).
static const int TAM_LEITURA[] = { 24, 29, 36 };
static int      s_tam_leitura = 1;

static uint64_t s_total = 0, s_livre = 0;

// ---- visualizacao de imagem (.bmp) -----------------------------------------
// o app Camera passou a salvar fotos em .bmp, e o
// Arquivos - que so sabia mostrar texto/CSV linha a linha - nao tinha como
// abrir. So entende o PROPRIO formato que tab5_camera.cpp escreve (16 bits,
// RGB565, BI_BITFIELDS) - nao e um decodificador de bmp generico, e nao
// precisa ser: nenhum outro app grava imagem no cartao.
static bool     s_e_imagem = false;
static uint32_t s_img_largura = 0, s_img_altura = 0;
static long     s_img_offset_dados = 0;
static uint32_t s_img_linha_bytes = 0;
static bool     s_img_ok = false;

static bool tem_sufixo(const char* nome, const char* sufixo)
{
  const size_t ln = strlen(nome), ls = strlen(sufixo);
  return ln >= ls && strcasecmp(nome + ln - ls, sufixo) == 0;
}

// Le so o cabecalho (BITMAPFILEHEADER + BITMAPINFOHEADER) - rapido, e o
// bastante para saber tamanho e onde os pixels comecam. A leitura de verdade
// e por LINHA, no desenho, para nao precisar de um buffer do tamanho da foto
// inteira (ver desenhar_imagem()).
static uint32_t ler_u32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t ler_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1]<<8)); }

static void abrir_cabecalho_imagem(const char* caminho)
{
  s_img_ok = false;
  FILE* f = fopen(caminho, "rb");
  if (!f) { return; }

  uint8_t cab[54];
  if (fread(cab, 1, sizeof(cab), f) != sizeof(cab) || cab[0] != 'B' || cab[1] != 'M') {
    fclose(f);
    return;
  }
  s_img_offset_dados = (long)ler_u32(cab + 10);
  s_img_largura      = ler_u32(cab + 18);
  s_img_altura        = ler_u32(cab + 22);
  const uint16_t bpp  = ler_u16(cab + 28);
  fclose(f);

  if (bpp != 16 || s_img_largura == 0 || s_img_altura == 0 || s_img_altura > 4096) { return; }
  s_img_linha_bytes = ((s_img_largura * 2 + 3) / 4) * 4;
  s_img_ok = true;
}

// Qual nivel foi desenhado por ultimo. O sistema NAO limpa a area entre
// redesenhos - e de proposito, e o que mata o flickering. So que este app troca
// o layout inteiro ao mudar de nivel, e o anterior ficaria por baixo: foi o que
// deu a tela com dois textos sobrepostos. Entao QUANDO O NIVEL MUDA, e so
// entao, o proprio app limpa a area uma vez.
static int s_nivel_desenhado = -1;
static bool s_abriu_ok = true;

// ---------------------------------------------------------------------------
// LEITURA NO WORKER (kernel/work)
// ---------------------------------------------------------------------------
// Contar as linhas (o energia.csv cresce sem limite) e ler uma pagina eram
// feitos NA TAREFA DE INTERFACE - com a tela e o toque parados enquanto o
// cartao respondia. Agora o worker faz os dois, SO com o estado abaixo
// (s_c_*) e a tabela de deslocamentos (s_off, que so ele usa); a interface
// copia o resultado no ao_aviso. Caminho e pagina sao FOTOGRAFADOS no pedido:
// trocar de arquivo ou virar pagina durante a leitura nao mistura nada.
static char          s_c_caminho[192];
static int           s_c_linha = 0;
static bool          s_c_contar = false;
static int           s_c_total = 0;
EXT_RAM_BSS_ATTR static char s_c_linhas[LER_LINHAS][LARG_LINHA + 1];
static int           s_c_n = 0;
static bool          s_c_abriu = true;
static volatile bool s_c_pronto = false;
static bool          s_c_ocupado = false;     // ha pedido em andamento
static bool          s_c_de_novo = false;     // pediram outra leitura no meio
static bool          s_c_contar_de_novo = false;

// LEITURA PENDENTE - nao ler do cartao dentro do tratador de toque.
//
// O ler_pagina() percorre o arquivo ate a pagina pedida e cede CPU a cada 256
// linhas. Rodando dentro do ao_tocar(), cada toque bloqueava a tarefa de
// interface ANTES de qualquer desenho; tocando rapido, as leituras se
// enfileiravam e a tela alternava entre a pagina velha e a nova em ritmo
// irregular - que e a piscada ao passar paginas depressa.
//
// Agora o toque so ANOTA o que quer, e a leitura acontece no desenhar(), junto
// do resto do trabalho de tela. Mesmo padrao do app Mesh com o historico.
static bool s_precisa_ler = false;
static pintura_t s_p;   // pintura por diferenca desta tela

// ---- geometria -------------------------------------------------------------
// O CABECALHO E O DO AJUSTES: "< VOLTAR" e o titulo na mesma faixa, a
// descricao embaixo e uma linha separando do conteudo. O anterior tinha o
// titulo (41 px) em y=72, a descricao (33 px) em y=104 e a fileira de saltos
// em y=128 - os tres se sobrepunham, e a descricao, pintada com fundo preto,
// comia o topo dos botoes. Aqui cada faixa tem altura conhecida e a seguinte
// comeca onde ela acaba. Larguras dos botoes: FRACAO da area, nunca pixel
// fixo - a fileira cabe em qualquer largura.
static const int MARGEM_X = 16;
static const int TOPO_Y = 16, TOPO_A = 64;           // VOLTAR + titulo
static const int VOLTAR_L = 200;
static const int AA_L = 96;                            // "Aa" (so no conteudo)
static const int DESC_Y = 92;                          // a linha de descricao (24 px)
static const int DIVISA_Y = 128;
static const int LISTA_Y = 140;                        // onde a lista comeca
// So no conteudo: a fileira de navegacao e a barra de posicao.
static const int NAV_Y = 140, NAV_A = 64, NAV_G = 10;
static const int BARRA_Y = 220, BARRA_A = 14;          // trilho desenhado
static const int BARRA_TOQUE_Y = 204, BARRA_TOQUE_A = 48;   // faixa que aceita o toque
static const int TEXTO_Y = 252;                        // onde o texto comeca
static const int ARRASTE_PAGINA = 90;                  // px de arraste vertical que viram a pagina
static const char* NAV_NOME[4] = { "INÍCIO", "ANTES", "DEPOIS", "FIM" };
static int nav_l(int largura) { return (largura - 2 * MARGEM_X - 3 * NAV_G) / 4; }
static int nav_x(int largura, int i) { return MARGEM_X + i * (nav_l(largura) + NAV_G); }
// 48 -> 72 (+50%): com 48 px o toque errava o arquivo com frequencia.
static const int LIN_LISTA = 72;

// TRES TAMANHOS: titulo +100%, descricao e lista +60%; o
// conteudo de CSV/texto fica no BASE - maior apertaria LIN_TEXTO (30px).
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
// stat() em vez de abrir/posicionar/fechar. Listar uma pasta com 50 arquivos
// eram 50 aberturas de arquivo no cartao - lento e desnecessario, o tamanho
// esta na entrada de diretorio.
static uint64_t tamanho_de(const char* caminho)
{
  struct stat st;
  if (stat(caminho, &st) != 0) { return 0; }
  return (st.st_size > 0) ? (uint64_t)st.st_size : 0;
}

// ORDEM: pastas primeiro, em ordem alfabetica; depois os arquivos do MAIS
// NOVO nome para o mais velho - os arquivos do sistema sao datados
// (log_2026-09-23.txt), e o de hoje e o que se procura, nao o do mes passado.
static int comparar_itens(const void* pa, const void* pb)
{
  const item_t* a = (const item_t*)pa;
  const item_t* b = (const item_t*)pb;
  if (a->eh_pasta != b->eh_pasta) { return a->eh_pasta ? -1 : 1; }
  return a->eh_pasta ? strcasecmp(a->name, b->name) : strcasecmp(b->name, a->name);
}

// Arquivos e bytes de uma pasta, SUBPASTAS INCLUSIVE - a pasta Sistema quase
// nao tem arquivo solto (os logs e a energia moram em Sistema/logs e
// Sistema/energia), e contar so o primeiro nivel diria "3 arq, 2KB".
static void somar_pasta(const char* caminho, int nivel, int* arquivos, uint64_t* bytes)
{
  DIR* d = opendir(caminho);
  if (!d) { return; }
  struct dirent* e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') { continue; }
    char cheio[224];
    snprintf(cheio, sizeof(cheio), "%s/%s", caminho, e->d_name);
    if (e->d_type == DT_DIR) {
      if (nivel < 3) { somar_pasta(cheio, nivel + 1, arquivos, bytes); }
    } else {
      *bytes += tamanho_de(cheio);
      ++*arquivos;
    }
  }
  closedir(d);
}

// Toda listagem nova: itens em ordem, lista re-medida e de volta ao topo.
// Quanto a listagem custou na tarefa de interface (o cartao e lido aqui, no
// toque). Acima de 150 ms o toque ja fica perceptivelmente preso: sai no log
// para sabermos se chegou a hora de mandar a listagem para o worker.
// A LISTAGEM E DO WORKER. Listar uma pasta (e somar o
// tamanho de cada subpasta, tres niveis abaixo) acontecia dentro do toque: a
// tela parava enquanto o cartao respondia - e a raiz, com o Sistema cheio de
// logs, levava centenas de ms. Agora o worker lista para s_carga (o
// buffer que a tela NAO desenha) e o ao_aviso troca os dois, como a leitura
// de conteudo ja fazia.
static item_t*        s_carga = nullptr;          // MAX_ITENS, PSRAM
static volatile bool  s_listando = false;         // pedido feito, resultado ainda nao entrou
static volatile bool  s_l_pronto = false;
static bool           s_l_pastas = true, s_l_manter = false;
static bool           s_l_de_novo = false, s_l_de_novo_pastas = true, s_l_de_novo_manter = false;
static char           s_l_pasta[128];             // o caminho pedido (copia: o toque pode mudar s_pasta)
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
        if (sistema_trabalho_cancelado()) { break; }
        item_t* it = &dst[n];
        snprintf(it->name, sizeof(it->name), "%s", e->d_name);
        it->arquivos = 0;
        it->bytes    = 0;
        it->eh_pasta = (e->d_type == DT_DIR);
        char cheio[224];
        snprintf(cheio, sizeof(cheio), "%s/%s", pasta, e->d_name);
        if (it->eh_pasta) { somar_pasta(cheio, 1, &it->arquivos, &it->bytes); }
        else if (!raiz)   { it->bytes = tamanho_de(cheio); }
        ++n;
      }
      closedir(d);
    }
  }
  if (n > 1) { qsort(dst, n, sizeof(item_t), comparar_itens); }
  s_l_n = n;
  s_l_achados = achados;
  const int64_t ms = (esp_timer_get_time() - t0) / 1000;
  if (ms > 150) { ESP_LOGW(TAG, "listagem de %d item(s) levou %lld ms", achados, (long long)ms); }
}

static void trabalho_listar(void*)
{
  tab5_sd_flush_pending();   // tamanhos e arquivos novos: o que ja foi entregue ao gravador
  listar_em(s_carga, s_l_pastas ? TAB5_SD_APPS : s_l_pasta, s_l_pastas);
  s_l_pronto = true;
}

// Pede a listagem do nivel atual. 'manter_posicao': voltar do conteudo para a
// mesma pasta nao deve jogar a lista para o topo.
static void pedir_listagem(bool pastas, bool manter_posicao)
{
  if (s_listando) {   // uma por vez: a ultima vontade vale
    s_l_de_novo = true; s_l_de_novo_pastas = pastas; s_l_de_novo_manter = manter_posicao;
    return;
  }
  if (!manter_posicao) { s_n_itens = s_n_achados = 0; lista_itens_mudaram(&s_lista, 0); }
  s_l_pastas = pastas;
  s_l_manter = manter_posicao;
  caminho_pasta(s_l_pasta, sizeof(s_l_pasta));
  s_l_pronto = false;
  s_listando = sistema_trabalho(trabalho_listar, nullptr);
}

static void listar_pastas(bool manter_posicao = false)   { pedir_listagem(true, manter_posicao); }
static void listar_arquivos(bool manter_posicao = false) { pedir_listagem(false, manter_posicao); }

// No ao_aviso: o resultado entra na tela.
static void listagem_chegou(void)
{
  s_l_pronto = false;
  s_listando = false;
  item_t* t = s_itens; s_itens = s_carga; s_carga = t;
  s_n_itens = s_l_n;
  s_n_achados = s_l_achados;
  if (s_l_pastas) { s_total = s_l_total; s_livre = s_l_livre; }
  lista_itens_mudaram(&s_lista, s_n_itens);
  if (!s_l_manter) { lista_ao_inicio(&s_lista); }
  if (s_l_de_novo) {
    s_l_de_novo = false;
    pedir_listagem(s_l_de_novo_pastas, s_l_de_novo_manter);
  }
  sistema_pedir_redesenho_completo();
}

#define PAG_OFF 256
static long s_off[PAG_OFF];

// PASSO DA TABELA: quantas paginas cada casa cobre.
//
// Era uma casa por pagina, o que cobria as 256 primeiras e deixava o resto no
// caminho lento - pular linha a linha desde o inicio. Num energia.csv de uma
// semana (~700 paginas) ir ao FIM lia o arquivo inteiro, e o botao "FIM" seria
// uma promessa que o app nao cumpre.
//
// Com passo, as mesmas 256 casas cobrem QUALQUER tamanho: a casa k guarda o
// inicio da pagina k*passo, e chegar numa pagina qualquer custa no maximo
// (passo-1) paginas de leitura sequencial. O custo vira limitado em vez de
// proporcional ao arquivo.
static int  s_passo_off = 1;

static void esquecer_offsets(void)
{
  for (int i = 0; i < PAG_OFF; ++i) { s_off[i] = -1; }
  s_off[0] = 0;                 // a primeira pagina comeca no byte zero
  s_passo_off = 1;
}

// Conta as linhas do arquivo. Feito UMA vez ao abrir, nao a cada pagina.
static void contar_linhas(void)
{
  s_c_total = 0;
  FILE* f = fopen(s_c_caminho, "r");
  if (!f) { return; }

  // Em BLOCO, nao byte a byte. O fgetc() num arquivo de 1MB sao um milhao de
  // chamadas; com blocos de 1KB sao mil leituras e um memchr por bloco.
  //
  // Roda no worker do sistema (trabalho_ler). Cede a CPU de tempos em
  // tempos, e SAI se o app fechou (sistema_trabalho_cancelado): fechar o app
  // espera este trabalho, e um log de varios MB seguraria a interface ate o
  // fim da varredura.
  // DUAS PASSADAS SOBRE O MESMO ARQUIVO SERIAM UMA A MAIS.
  //
  // Esta varredura ja le tudo para contar as linhas. Entao ela tambem ANOTA os
  // deslocamentos - o indice sai de graca, junto da contagem que ja acontecia.
  // Montar o indice depois, sob demanda, deixaria o primeiro "FIM" caro; assim
  // ele nasce pronto.
  //
  // O passo so pode ser calculado no fim (depende do total), entao a primeira
  // passada anota TODAS as fronteiras de pagina que couberem e a correcao vem
  // depois. Para arquivo de ate 256 paginas isso ja e o indice final.
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
    const char* fim = bloco + lidos;
    while ((p = (const char*)memchr(p, '\n', (size_t)(fim - p))) != NULL) {
      s_c_total++;
      p++;
      // Fronteira de pagina: o byte seguinte a esta quebra inicia a proxima.
      if ((s_c_total % BLOCO_LINHAS) == 0 && n_fronteiras < PAG_OFF) {
        fronteira[n_fronteiras++] = pos + (long)(p - bloco);
      }
    }
    pos += (long)lidos;
    if ((++voltas % 64) == 0) {
      if (sistema_trabalho_cancelado()) { fclose(f); s_c_total = 0; return; }
      vTaskDelay(1);
    }
  }
  fclose(f);
  if (ultimo != '\n') { s_c_total++; }   // a ultima linha sem quebra tambem e linha

  // Passo: quantas paginas cada casa cobre, para as PAG_OFF casas alcancarem
  // o arquivo inteiro. Arquivo pequeno fica com passo 1 e indice exato.
  const int paginas = (s_c_total > 0) ? (s_c_total + BLOCO_LINHAS - 1) / BLOCO_LINHAS : 1;
  s_passo_off = (paginas + PAG_OFF - 1) / PAG_OFF;
  if (s_passo_off < 1) { s_passo_off = 1; }

  for (int k = 0; k < PAG_OFF; ++k) {
    const int pag = k * s_passo_off;
    s_off[k] = (pag < n_fronteiras) ? fronteira[pag] : -1;
  }
}

// UMA LINHA INTEIRA do arquivo, ate o '\n' - o que passar de 'n' e lido e
// descartado. O fgets(256) de antes partia uma linha longa em varias, e as
// partes contavam como linhas: o "pular ate a linha L" errava para a frente e
// a contagem (que conta '\n') nao batia com o que se via. false = fim.
static bool ler_linha(FILE* f, char* buf, size_t n, bool* cortada)
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

// O CSV quebra no campo: um espaco depois de cada separador (que nao tenha),
// porque o quebrador de linha so quebra em espaco. Sem isto uma linha de CSV
// e uma "palavra" so, cortada por caractere no meio de um numero.
static void espacar_csv(const char* de, char* para, size_t n)
{
  size_t k = 0;
  for (const char* p = de; *p && k + 1 < n; ++p) {
    para[k++] = *p;
    if ((*p == ',' || *p == ';') && p[1] != ' ' && p[1] != 0 && k + 1 < n) { para[k++] = ' '; }
  }
  para[k] = 0;
}

// Carrega LER_LINHAS linhas a partir da linha s_c_linha. Vem mais do que cabe
// de proposito: quantas cabem so o desenho sabe (depende da quebra).
static void ler_pagina(void)
{
  s_c_n = 0;
  const char* caminho = s_c_caminho;

  FILE* f = fopen(caminho, "r");
  if (!f) {
    // Distinguir isto de "arquivo vazio" importa: os dois davam a MESMA tela em
    // branco, e um deles e um arquivo de zero byte enquanto o outro e caminho
    // errado, cartao desmontado ou nome que o FAT nao aceita. Sem separar, nao
    // ha como saber qual dos dois voce esta olhando.
    s_c_abriu = false;
    ESP_LOGW(TAG, "nao consegui abrir '%s'", caminho);
    return;
  }
  s_c_abriu = true;
  // Buffer do stdio maior que os 128 bytes padrao: o getc() de linha a linha
  // nao vira um read() do cartao a cada 128 bytes.
  setvbuf(f, nullptr, _IOFBF, 4096);
  const bool csv = tem_sufixo(s_c_caminho, ".csv");

  // Casa do indice mais proxima ANTES da linha pedida, e o resto lido em
  // sequencia (no maximo BLOCO_LINHAS * passo linhas).
  const int bloco = s_c_linha / BLOCO_LINHAS;
  const int casa = bloco / s_passo_off;
  int pular;
  if (casa < PAG_OFF && s_off[casa] >= 0) {
    fseek(f, s_off[casa], SEEK_SET);
    pular = s_c_linha - casa * s_passo_off * BLOCO_LINHAS;
  } else {
    pular = s_c_linha;   // sem indice: do inicio
  }

  static char linha[LARG_LINHA + 1];
  bool cortada;
  {
    int voltas = 0;
    while (pular > 0 && ler_linha(f, linha, sizeof(linha), &cortada)) {
      pular--;
      if ((++voltas % 256) == 0) { vTaskDelay(1); }   // nao segurar o core
    }
  }

  while (s_c_n < LER_LINHAS && ler_linha(f, linha, sizeof(linha), &cortada)) {
    const size_t k = strlen(linha);
    if (k && linha[k - 1] == '\r') { linha[k - 1] = 0; }
    char* dst = s_c_linhas[s_c_n];
    if (csv) { espacar_csv(linha, dst, LARG_LINHA + 1); }
    else     { snprintf(dst, LARG_LINHA + 1, "%s", linha); }
    if (cortada) {
      // Linha maior que o buffer: o fim avisa que tem mais (sem partir UTF-8).
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
static void trabalho_ler(void*)
{
  tab5_sd_flush_pending();   // o que o gravador ainda nao escreveu tem que aparecer aqui
  if (s_c_contar) { esquecer_offsets(); contar_linhas(); }
  ler_pagina();
  s_c_pronto = true;
}

// Na interface: fotografa o pedido e entrega ao worker.
static void pedir_leitura(bool contar)
{
  if (s_c_ocupado) {
    s_c_de_novo = true;
    s_c_contar_de_novo = s_c_contar_de_novo || contar;
    return;
  }
  caminho_arquivo(s_c_caminho, sizeof(s_c_caminho));
  s_c_linha   = s_linha;
  s_c_contar  = contar;
  s_c_pronto  = false;
  s_c_ocupado = sistema_trabalho(trabalho_ler, nullptr);
  if (!s_c_ocupado) { s_precisa_ler = true; }   // queue cheia: tenta na proxima passada
}

// Na interface: a leitura terminou -> copia o que ela trouxe.
// ---- imagem lida pelo WORKER -----------------------------------------------
// Ler a foto do cartao dentro do desenhar() segurava a interface: 1,4 MB de
// SD linha a linha, com uma pausa a cada 8 linhas - medido, 2,3 s de tela
// parada ao abrir uma foto, e de novo a cada redesenho completo (a foto era
// relida do cartao toda vez). Agora o worker le UMA vez, para um buffer na
// PSRAM ja na ordem da tela, e o desenhar() so pinta o buffer.
//
// O buffer e do app (trabalho comum, nao solto): fechar o app espera o
// worker, e so entao o ao_fechar() o libera.
static uint16_t*     s_img_px = nullptr;        // a imagem JA NA ESCALA da tela, de cima para baixo
static uint32_t      s_img_px_l = 0, s_img_px_a = 0;   // tamanho de s_img_px
static char          s_img_px_caminho[192] = "";
static volatile bool s_img_lendo = false;       // o worker esta com o buffer
static volatile bool s_img_lida = false;        // o worker terminou (bem ou mal)
static volatile bool s_img_lida_ok = false;

// A IMAGEM INTEIRA, NA PROPORCAO DA TELA. A foto do Camera tem 1200x1600 e a
// area do app ~700x1000: em tamanho real, so um pedaco aparecia. Aqui a escala
// e a MAIOR que faz a imagem caber inteira (largura e altura), nunca acima de
// 1: imagem menor que a area fica no tamanho dela - aumentar so borraria.
//
// REDUZ LENDO, sem a foto inteira na memoria: o arquivo e lido UMA vez, em
// sequencia (um fseek so - ler para tras estourou o watchdog uma vez), e cada
// linha de saida e feita da linha de origem correspondente com a de baixo,
// cada pixel a media de 2x2 vizinhos - o bastante para nao serrilhar numa
// reducao de ~0,6x. So tres linhas da origem existem ao mesmo tempo.
// BMP guarda de BAIXO para CIMA: a primeira linha lida e a ultima da tela.
static inline uint16_t media4(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
  const uint32_t r  = ((a >> 11) + (b >> 11) + (c >> 11) + (d >> 11) + 2) >> 2;
  const uint32_t g  = (((a >> 5) & 0x3F) + ((b >> 5) & 0x3F) + ((c >> 5) & 0x3F) + ((d >> 5) & 0x3F) + 2) >> 2;
  const uint32_t bl = ((a & 0x1F) + (b & 0x1F) + (c & 0x1F) + (d & 0x1F) + 2) >> 2;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// LEITURA EM BLOCOS GRANDES, SEM O FILE* DO STDIO. Com fread, a foto de
// 3,8 MB levava ~10 s: o FILE* do newlib tem buffer de 128 bytes
// (CONFIG_FATFS_VFS_FSTAT_BLKSIZE=0), entao cada linha virava dezenas de
// read() de 128 bytes e o FatFs buscava o cartao SETOR A SETOR - milhares de
// comandos de 512 bytes, cada um com o custo fixo do protocolo do SD.
// Aqui o read() pede 64 KB de uma vez, sempre de uma posicao do arquivo
// alinhada a setor e para um buffer alinhado a linha de cache: o FatFs entrega
// os setores inteiros direto no buffer e o SDMMC le varios setores por
// comando, por DMA, sem passar por buffer intermediario.
#define IMG_BLOCO       (64 * 1024)   // multiplo de 512 (setor)
#define IMG_ALINHAMENTO 128           // linha de cache L2 (CONFIG_CACHE_L2_CACHE_LINE_128B)

typedef struct {
  int      fd;
  uint8_t* buf;
  size_t   n;     // bytes validos no buffer
  size_t   pos;   // proximo byte a entregar
} leitor_img_t;

// Entrega 'qtd' bytes em 'destino' (ou so pula, se destino == nullptr),
// buscando novos blocos do cartao quando o atual acaba.
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

static void trabalho_ler_imagem(void*)
{
  s_img_lida_ok = false;
  const int64_t t0 = esp_timer_get_time();
  const uint32_t W = s_img_largura, H = s_img_altura;
  const uint32_t L = s_img_px_l, A = s_img_px_a;
  const bool reduz = (L < W || A < H);
  uint16_t* atual = (uint16_t*)heap_caps_malloc((size_t)W * 2, MALLOC_CAP_SPIRAM);
  uint16_t* baixo = (uint16_t*)heap_caps_malloc((size_t)W * 2, MALLOC_CAP_SPIRAM);   // a linha de origem logo abaixo
  leitor_img_t r = { -1, nullptr, 0, 0 };
  r.buf = (uint8_t*)heap_caps_aligned_alloc(IMG_ALINHAMENTO, IMG_BLOCO, MALLOC_CAP_SPIRAM);
  if (atual && baixo && r.buf) { r.fd = open(s_img_px_caminho, O_RDONLY); }
  int32_t o = (int32_t)A - 1;
  bool ok = false;
  if (r.fd >= 0) {
    // Comeca no inicio do SETOR onde estao os pixels e descarta o pedaco do
    // cabecalho: assim todo read() seguinte comeca alinhado a setor.
    const off_t inicio = (off_t)s_img_offset_dados & ~(off_t)511;
    ok = lseek(r.fd, inicio, SEEK_SET) == inicio &&
         leitor_img_pegar(r, nullptr, (size_t)(s_img_offset_dados - inicio));
    const size_t quer = (size_t)W * 2;
    const size_t pulo = s_img_linha_bytes - quer;   // enchimento ate multiplo de 4
    // Linhas de SAIDA de baixo para cima, acompanhando a leitura: a saida 'o'
    // nasce da origem (o * H) / A.
    for (uint32_t k = 0; ok && k < H && o >= 0; ++k) {
      const uint32_t origem = H - 1 - k;            // linha de tela que acabou de ser lida
      uint16_t* t = baixo; baixo = atual; atual = t;   // a anterior lida e a de baixo
      if (!leitor_img_pegar(r, (uint8_t*)atual, quer) || !leitor_img_pegar(r, nullptr, pulo)) { ok = false; break; }
      while (o >= 0 && ((uint32_t)o * H) / A == origem) {
        uint16_t* destino = s_img_px + (size_t)o * L;
        if (!reduz) {
          memcpy(destino, atual, quer);
        } else {
          const uint16_t* de_baixo = (k > 0) ? baixo : atual;   // a ultima linha nao tem vizinha
          for (uint32_t x = 0; x < L; ++x) {
            const uint32_t sx  = (x * W) / L;
            const uint32_t sx2 = (sx + 1 < W) ? sx + 1 : sx;
            destino[x] = media4(atual[sx], atual[sx2], de_baixo[sx], de_baixo[sx2]);
          }
        }
        --o;
      }
      if ((k & 31) == 0 && sistema_trabalho_cancelado()) { ok = false; break; }
    }
    close(r.fd);
  }
  s_img_lida_ok = ok && o < 0;
  ESP_LOGI(TAG, "imagem %ux%u -> %ux%u lida em %d ms (ok=%d)", (unsigned)W, (unsigned)H,
           (unsigned)L, (unsigned)A, (int)((esp_timer_get_time() - t0) / 1000), (int)s_img_lida_ok);
  if (r.buf) { heap_caps_free(r.buf); }
  if (atual) { heap_caps_free(atual); }
  if (baixo) { heap_caps_free(baixo); }
  s_img_lida = true;
}

static void soltar_imagem(void)
{
  if (s_img_lendo) { return; }   // o worker esta escrevendo nele: solta quando ele avisar
  if (s_img_px) { heap_caps_free(s_img_px); s_img_px = nullptr; }
  s_img_px_l = s_img_px_a = 0;
  s_img_px_caminho[0] = '\0';
}

static void ao_aviso(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_img_lendo && s_img_lida) {
    s_img_lendo = false;
    if (!s_img_lida_ok) { s_img_px_caminho[0] = '\0'; }   // le de novo se pedir
    if (s_nivel == NIVEL_CONTEUDO && s_e_imagem) { sistema_pedir_redesenho_completo(); }
    else                                         { soltar_imagem(); }   // ja saiu da foto
  }
  if ((av & AVISO_TRABALHO) && s_l_pronto) { listagem_chegou(); }
  if (!(av & AVISO_TRABALHO) || !s_c_pronto) { return; }
  s_c_pronto  = false;
  s_c_ocupado = false;

  char atual[192];
  caminho_arquivo(atual, sizeof(atual));
  const bool mesmo_arquivo = (strcmp(atual, s_c_caminho) == 0) && s_nivel == NIVEL_CONTEUDO;
  if (mesmo_arquivo) {
    if (s_c_contar) { s_total_linhas = s_c_total; }
    memcpy(s_linhas, s_c_linhas, sizeof(s_linhas));
    s_n_linhas = s_c_n;
    s_abriu_ok = s_c_abriu;
  }
  // Virou pagina (ou trocou de arquivo) durante a leitura: le de novo.
  if (s_c_de_novo || (mesmo_arquivo && s_c_linha != s_linha)) {
    const bool contar = s_c_contar_de_novo;
    s_c_de_novo = false;
    s_c_contar_de_novo = false;
    pedir_leitura(contar);
  }
  if (mesmo_arquivo) { sistema_pedir_redesenho_completo(); }
}

static void desenhar_imagem(lgfx::LovyanGFX& d, const area_t& a, int x, int y)
{
  if (!s_img_ok) {
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.drawString("NÃO CONSEGUI LER ESTA IMAGEM", x, y);
    return;
  }

  // A caixa disponivel (mesma margem dos lados) e a escala que faz caber.
  const int caixa_l = a.largura - 2 * (x - a.x);
  const int caixa_a = (a.y + a.altura - 10) - y;
  if (caixa_l <= 0 || caixa_a <= 0 || s_img_largura > 4096) { return; }
  uint32_t L = s_img_largura, A = s_img_altura;
  if (L > (uint32_t)caixa_l || A > (uint32_t)caixa_a) {
    // Menor das duas razoes, em inteiros: compara caixa_l/W com caixa_a/H.
    if ((uint64_t)caixa_l * s_img_altura <= (uint64_t)caixa_a * s_img_largura) {
      L = (uint32_t)caixa_l;
      A = (uint32_t)(((uint64_t)s_img_altura * caixa_l) / s_img_largura);
    } else {
      A = (uint32_t)caixa_a;
      L = (uint32_t)(((uint64_t)s_img_largura * caixa_a) / s_img_altura);
    }
    if (L == 0) { L = 1; }
    if (A == 0) { A = 1; }
  }
  const int px = a.x + (a.largura - (int)L) / 2;   // centralizada na caixa
  const int py = y + (caixa_a - (int)A) / 2;

  char caminho[192];
  caminho_arquivo(caminho, sizeof(caminho));

  // PRONTA: o buffer e desta foto, deste tamanho, e o worker terminou bem.
  if (!s_img_lendo && s_img_px && s_img_px_l == L && s_img_px_a == A && strcmp(s_img_px_caminho, caminho) == 0) {
    // "efeito arco-iris" - MESMO aviso do app Camera (ver app_camera.cpp): o
    // LovyanGFX espera a ordem de byte trocada em relacao ao RGB565 cru que
    // esta gravado no arquivo. Estado global do display - salva e devolve.
    const bool troca_ant = d.getSwapBytes();
    // As margens em volta da imagem centralizada: limpa o que estiver la (o
    // "carregando imagem..." ficava aparecendo acima dela). Cada faixa uma vez,
    // sem pintar duas vezes a area da imagem.
    const int cx0 = a.x, cx1 = a.x + a.largura, cy1 = y + caixa_a;
    if (py > y)                  { d.fillRect(cx0, y, cx1 - cx0, py - y, TFT_BLACK); }
    if (py + (int)A < cy1)       { d.fillRect(cx0, py + (int)A, cx1 - cx0, cy1 - (py + (int)A), TFT_BLACK); }
    if (px > cx0)                { d.fillRect(cx0, py, px - cx0, (int)A, TFT_BLACK); }
    if (px + (int)L < cx1)       { d.fillRect(px + (int)L, py, cx1 - (px + (int)L), (int)A, TFT_BLACK); }
    d.setSwapBytes(true);
    d.pushImage(px, py, (int)L, (int)A, s_img_px);
    d.setSwapBytes(troca_ant);
    return;
  }

  // AINDA NAO: pede ao worker (uma vez) e mostra que esta vindo.
  if (!s_img_lendo) {
    soltar_imagem();
    s_img_px = (uint16_t*)heap_caps_malloc((size_t)L * A * 2, MALLOC_CAP_SPIRAM);
    if (!s_img_px) {
      d.setTextColor(TFT_RED, TFT_BLACK);
      d.drawString("SEM MEMÓRIA PARA A IMAGEM", x, y);
      return;
    }
    s_img_px_l = L;
    s_img_px_a = A;
    snprintf(s_img_px_caminho, sizeof(s_img_px_caminho), "%s", caminho);
    s_img_lida = false;
    s_img_lendo = sistema_trabalho(trabalho_ler_imagem, nullptr);
    if (!s_img_lendo) { soltar_imagem(); }
  }
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString("carregando imagem...", x, y);
}

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------
static bool ao_abrir(void)
{
  esquecer_offsets();
  if (!s_itens) { s_itens = (item_t*)heap_caps_calloc(MAX_ITENS, sizeof(item_t), MALLOC_CAP_SPIRAM); }
  if (!s_carga) { s_carga = (item_t*)heap_caps_calloc(MAX_ITENS, sizeof(item_t), MALLOC_CAP_SPIRAM); }
  if (!s_itens || !s_carga) { return false; }
  s_listando = false; s_l_pronto = false; s_l_de_novo = false;
  s_n_itens = s_n_achados = 0;
  lista_definir(&s_lista, ROLAGEM_INICIO, 0, item_altura, item_desenhar, nullptr);
  s_nivel = NIVEL_PASTAS;
  s_pasta[0] = s_arquivo[0] = '\0';
  s_linha = 0;
  s_n_voltas = 0;
  listar_pastas();
  return true;
}

static void ao_fechar(void)
{
  s_n_itens = 0;
  if (s_itens) { heap_caps_free(s_itens); s_itens = nullptr; }
  if (s_carga) { heap_caps_free(s_carga); s_carga = nullptr; }   // o fechamento ja esperou o worker
  s_n_linhas = 0;
  s_e_imagem = false;
  s_img_ok = false;
  s_img_lendo = false;   // fechar o app ja esperou o worker
  soltar_imagem();
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

// Pastinha PEQUENA ao lado do nome de uma pasta, para distinguir pasta de
// arquivo na mesma lista.
// Mesmas cores/ordem de desenho do icone grande da tela inicial
// (icone_arquivos, mais abaixo: aba primeiro, corpo por cima cobrindo a
// junta), so sem o fundo quadrado - aqui ela senta direto no preto da linha,
// nao numa lade de app.
static void icone_pasta_pequena(lgfx::LovyanGFX& d, int x, int y, int w, int h)
{
  const uint16_t pasta = d.color565(240, 190, 90);
  const uint16_t aba   = d.color565(205, 155, 60);
  const int aba_l = (int)(0.5f  * w);
  const int aba_a = (int)(0.28f * h);
  d.fillRoundRect(x, y,                 aba_l, aba_a,          2, aba);
  d.fillRoundRect(x, y + aba_a - 1,     w,     h - aba_a + 1,  3, pasta);
}

// Folha PEQUENA para arquivo comum: sem ela a coluna do icone ficaria vazia
// nas linhas de arquivo (a pastinha so aparece quando eh_pasta). Pagina
// clara com o canto superior direito dobrado (leitura universal de "isto e
// um arquivo") + duas linhas finas representando texto generico dentro dela -
// a MESMA ideia de camada da pastinha (recorte, depois a dobra por cima).
static void icone_arquivo_pequeno(lgfx::LovyanGFX& d, int x, int y, int w, int h)
{
  const uint16_t pagina = TFT_LIGHTGREY;
  const uint16_t dobra  = TFT_DARKGREY;

  const int pl = (int)(0.55f * w);
  const int pa = (int)(0.85f * h);
  const int px = x + (w - pl) / 2;
  const int py = y + (h - pa) / 2;
  const int canto = (pl < pa ? pl : pa) / 3;

  d.fillRoundRect(px, py, pl, pa, 2, pagina);
  // RECORTE do canto: apaga o quadradinho superior direito de volta ao preto
  // da linha, deixando so o triangulo diagonal da pagina.
  d.fillTriangle(px + pl - canto, py, px + pl, py, px + pl, py + canto, TFT_BLACK);
  // DOBRA: preenche o mesmo canto so que com o triangulo espelhado - a
  // "pontinha" que sobraria dobrada para dentro da folha.
  d.fillTriangle(px + pl - canto, py, px + pl, py + canto, px + pl - canto, py + canto, dobra);

  const int linha_x = px + (int)(0.18f * pl);
  const int linha_l = (int)(0.64f * pl);
  d.fillRect(linha_x, py + (int)(0.55f * pa), linha_l, 2, dobra);
  d.fillRect(linha_x, py + (int)(0.72f * pa), linha_l, 2, dobra);
}

// ---------------------------------------------------------------------------
// UM ITEM DA LISTA (pasta ou arquivo)
// ---------------------------------------------------------------------------
static int item_altura(int, void*) { return LIN_LISTA; }

// x = borda esquerda da area; largura = ate a coluna da barra. Colunas em X
// proporcional a largura (a area muda com a rotacao), nao pixel chutado.
static void item_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  const item_t& it = s_itens[i];
  const int MARGEM = 16;
  const int x = x0 + MARGEM;
  const int util = largura - MARGEM;
  const int contagem_x = x + (int)(util * 0.55f);
  const int tamanho_x  = x + (int)(util * 0.78f);

  tab5_texto(d, TAM_LISTA);
  d.setTextDatum(top_left);

  // ICONE ANCORADO NO TEXTO: mesma altura e mesmo (x,y) que o
  // nome usa - d.fontHeight(), nunca um numero chutado. Toda linha ganha
  // icone (pasta ou folha), entao o nome sempre comeca na mesma coluna.
  const int icone_l = 40;
  const int icone_a = d.fontHeight();
  if (it.eh_pasta) { icone_pasta_pequena(d, x, y, icone_l, icone_a); }
  else             { icone_arquivo_pequeno(d, x, y, icone_l, icone_a); }

  // Nome cortado MEDINDO (d.textWidth), para nao invadir a coluna seguinte.
  const int nome_x = x + icone_l + 14;
  const int nome_max_l = contagem_x - nome_x - 12;
  char nome[40];
  snprintf(nome, sizeof(nome), "%s", it.name);
  tab5_cortar_ao_caber(d, nome, nome_max_l);   // por caractere: nome de pasta com acento
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(nome, nome_x, y);

  char buf[24];
  if (it.eh_pasta) {
    snprintf(buf, sizeof(buf), "%d arq", it.arquivos);
    d.drawString(buf, contagem_x, y);
  }
  tamanho_txt(buf, sizeof(buf), it.bytes);
  d.drawString(buf, tamanho_x, y);

  // Linha fina entre itens - nao depois
  // do ultimo.
  if (i + 1 < s_n_itens) {
    d.drawFastHLine(x0 + MARGEM, y + LIN_LISTA - 16, largura - MARGEM, TFT_DARKGREY);
  }
}

// Texto numa linha so, cortado MEDINDO para caber em 'largura' (com "..."),
// sem partir um caractere UTF-8.
static void texto_cabendo(lgfx::LovyanGFX& d, const char* txt, int x, int y, int largura)
{
  char buf[192];
  snprintf(buf, sizeof(buf), "%s", txt);
  tab5_cortar_com_reticencias(d, buf, sizeof(buf), largura);
  d.drawString(buf, x, y);
}

// "1.234" - numero grande de linhas se le melhor com milhar.
static void milhar(char* out, size_t n, int v)
{
  if (v >= 1000000) { snprintf(out, n, "%d.%03d.%03d", v / 1000000, (v / 1000) % 1000, v % 1000); }
  else if (v >= 1000) { snprintf(out, n, "%d.%03d", v / 1000, v % 1000); }
  else { snprintf(out, n, "%d", v); }
}

// O TEXTO: cada linha do arquivo quebrada na largura; continuacao recuada,
// e um respiro entre uma linha do arquivo e a seguinte - assim se ve onde uma
// linha longa acaba. Para quando a proxima nao cabe inteira (a nao ser a
// primeira, que aparece no que couber: uma linha maior que a tela nao pode
// travar a leitura). Devolve quantas linhas do arquivo couberam.
static int desenhar_texto(lgfx::LovyanGFX& d, const area_t& a, int y)
{
  const int px = TAM_LEITURA[s_tam_leitura];
  d.setFont(tab5_fonte_suave(false, px));
  d.setTextSize(1);
  d.setTextDatum(top_left);
  const int alt_linha = px + px / 4;
  const int respiro = px / 3;
  const int recuo = px;
  const int x = a.x + MARGEM_X;
  const int largura = a.largura - 2 * MARGEM_X;
  const int fundo = a.y + a.altura - 10;

  static char vis[CONVERSA_MAX_LINHAS][CONVERSA_LINHA_BYTES];
  int cabem = 0;
  for (int i = 0; i < s_n_linhas; ++i) {
    const char* txt = s_linhas[i][0] ? s_linhas[i] : " ";
    // Primeira linha visual na largura toda, as seguintes recuadas: quebra
    // primeiro na largura menor (o recuo) e redesenha a primeira sem ele.
    const int n = conversa_quebrar_linhas(d, txt, largura - recuo, vis, CONVERSA_MAX_LINHAS);
    const int altura = n * alt_linha;
    if (y + altura > fundo && cabem > 0) { break; }
    for (int k = 0; k < n; ++k) {
      if (y + alt_linha > fundo) { break; }
      d.setTextColor(k == 0 ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
      d.drawString(vis[k], k == 0 ? x : x + recuo, y);
      y += alt_linha;
    }
    y += respiro;
    ++cabem;
    if (y >= fundo) { break; }
  }
  if (s_n_linhas == 0) {
    d.setTextColor(s_abriu_ok ? TFT_LIGHTGREY : TFT_RED, TFT_BLACK);
    d.drawString(s_abriu_ok ? "(arquivo vazio ou fim do conteúdo)"
                             : "NÃO CONSEGUI ABRIR - veja o caminho no log", x, y);
    y += alt_linha;
    if (!s_abriu_ok) {
      char cam[192];
      caminho_arquivo(cam, sizeof(cam));
      d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      texto_cabendo(d, cam, x, y, largura);
      y += alt_linha;
    }
  }
  // Apaga o resto, para tela curta nao deixar resto da anterior.
  if (y < fundo) { tab5_screen_clear(a.x, y, a.largura, a.y + a.altura - y, TFT_BLACK); }
  tab5_fonte_corpo(d);
  return cabem > 0 ? cabem : 1;
}

// Barra de posicao: onde a tela esta no arquivo. Tocar nela pula para la.
static void desenhar_barra(lgfx::LovyanGFX& d, const area_t& a)
{
  const int x = a.x + MARGEM_X, l = a.largura - 2 * MARGEM_X;
  const int y = a.y + BARRA_Y;
  d.fillRoundRect(x, y, l, BARRA_A, BARRA_A / 2, d.color565(50, 50, 62));
  if (s_total_linhas <= 0) { return; }
  int ini = (int)((int64_t)l * s_linha / s_total_linhas);
  int fim = (int)((int64_t)l * (s_linha + s_cabem) / s_total_linhas);
  if (fim > l) { fim = l; }
  if (fim - ini < BARRA_A) { ini = (fim - BARRA_A > 0) ? fim - BARRA_A : 0; fim = ini + BARRA_A; }
  d.fillRoundRect(x + ini, y, fim - ini, BARRA_A, BARRA_A / 2, TFT_SKYBLUE);
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  char linha[128], t1[16], t2[16], t3[16];
  const int x = a.x + MARGEM_X;

  // Datum e tamanho SEMPRE: sao estado global do display e a barra de status
  // desenha entre duas passadas daqui. Ver o aviso no app_config.cpp.
  tab5_texto(d, TAM_BASE);
  d.setTextDatum(top_left);

  pintura_iniciar(&s_p);

  // A leitura do cartao acontece AQUI, nao no toque. Ver s_precisa_ler.
  if (s_precisa_ler) { s_precisa_ler = false; pedir_leitura(false); }

  s_nivel_desenhado = (int)s_nivel;

  if (!tab5_sd_mounted()) {
    tab5_texto(d, 3.625f);
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.drawString("CARTÃO NÃO MONTADO      ", x, a.y + 24);
    tab5_texto(d, 2);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString("Confira o encaixe e se está em FAT32.", x, a.y + 90);
    d.drawString("O motivo exato sai no log (sd).  ", x, a.y + 120);
    return;
  }

  const bool texto = (s_nivel == NIVEL_CONTEUDO && !s_e_imagem);

  // ---- cabecalho: [< VOLTAR] titulo [Aa] ------------------------------------
  // Sempre numa passada completa (toda troca de nivel/pagina pede uma): o
  // sistema ja limpou a area, nada aqui precisa largura fixa.
  int titulo_x = x;
  int titulo_fim = a.x + a.largura - MARGEM_X;
  if (s_nivel != NIVEL_PASTAS) {
    tab5_botao(d, x, a.y + TOPO_Y, VOLTAR_L, TOPO_A, "< VOLTAR", TFT_SKYBLUE);
    titulo_x = x + VOLTAR_L + 20;
  }
  if (texto) {
    tab5_botao(d, titulo_fim - AA_L, a.y + TOPO_Y, AA_L, TOPO_A, "Aa", d.color565(70, 70, 86), TFT_WHITE);
    titulo_fim -= AA_L + 16;
  }
  tab5_titulo(d, 36);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  texto_cabendo(d, (s_nivel == NIVEL_PASTAS) ? (tab5_sd_name()[0] ? tab5_sd_name() : "Cartão") : (s_nivel == NIVEL_ARQUIVOS) ? s_pasta : s_arquivo,
                titulo_x, a.y + TOPO_Y + TOPO_A / 2, titulo_fim - titulo_x);
  d.setTextDatum(top_left);

  // ---- descricao ------------------------------------------------------------
  linha[0] = '\0';
  if (s_nivel == NIVEL_PASTAS) {
    tamanho_txt(t1, sizeof(t1), (s_total > s_livre) ? (s_total - s_livre) : 0);
    tamanho_txt(t2, sizeof(t2), s_total);
    snprintf(linha, sizeof(linha), "%s | usado %s de %s", tab5_sd_description(), t1, t2);
  } else if (s_nivel == NIVEL_ARQUIVOS) {
    if (s_n_achados > s_n_itens) {
      snprintf(linha, sizeof(linha), "%d de %d item(s) - toque para abrir", s_n_itens, s_n_achados);
    } else {
      snprintf(linha, sizeof(linha), "%d item(s) - toque para abrir", s_n_itens);
    }
  } else if (s_e_imagem) {
    if (s_img_ok) { snprintf(linha, sizeof(linha), "imagem %ux%u", (unsigned)s_img_largura, (unsigned)s_img_altura); }
    else          { snprintf(linha, sizeof(linha), "não consegui ler esta imagem"); }
  }
  // (a descricao do texto depende de quantas linhas couberam: sai depois dele)
  d.setFont(tab5_fonte_suave(false, 24));
  d.setTextSize(1);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (!texto) { texto_cabendo(d, linha, x, a.y + DESC_Y, a.largura - 2 * MARGEM_X); }
  d.drawFastHLine(a.x, a.y + DIVISA_Y, a.largura, d.color565(60, 60, 72));
  tab5_texto(d, TAM_BASE);

  // ---- corpo ----------------------------------------------------------------
  if (s_nivel == NIVEL_CONTEUDO) { lista_esconder(&s_lista); }   // o conteudo nao usa a lista
  if (s_nivel == NIVEL_CONTEUDO && s_e_imagem) {
    desenhar_imagem(d, a, x, a.y + LISTA_Y);
    return;
  }

  if (texto) {
    // O texto primeiro: e ele que diz quantas linhas couberam - e disso
    // depende o DEPOIS estar ativo e o tamanho da marca na barra.
    s_cabem = desenhar_texto(d, a, a.y + TEXTO_Y);
    const int L = a.largura;
    for (int i = 0; i < 4; ++i) {
      const bool ativo = (i < 2) ? s_linha > 0 : s_linha + s_cabem < s_total_linhas;
      tab5_botao(d, a.x + nav_x(L, i), a.y + NAV_Y, nav_l(L), NAV_A, NAV_NOME[i],
                 ativo ? ((i == 0 || i == 3) ? TFT_DARKCYAN : TFT_SKYBLUE) : d.color565(60, 60, 72),
                 ativo ? TFT_BLACK : TFT_DARKGREY);
    }
    desenhar_barra(d, a);
    // Descricao: que linhas estao na tela, de quantas.
    milhar(t1, sizeof(t1), s_linha + 1);
    int ult = s_linha + s_cabem;
    if (ult > s_total_linhas) { ult = s_total_linhas; }
    milhar(t2, sizeof(t2), ult);
    milhar(t3, sizeof(t3), s_total_linhas);
    const int pct = s_total_linhas > 0 ? (int)((int64_t)ult * 100 / s_total_linhas) : 100;
    if (s_total_linhas <= 0) { snprintf(linha, sizeof(linha), s_c_ocupado ? "lendo..." : "vazio"); }
    else { snprintf(linha, sizeof(linha), "linhas %s-%s de %s  (%d%%)", t1, t2, t3, pct); }
    d.setFont(tab5_fonte_suave(false, 24));
    d.setTextSize(1);
    d.setTextDatum(top_left);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    texto_cabendo(d, linha, x, a.y + DESC_Y, a.largura - 2 * MARGEM_X);
    tab5_texto(d, TAM_BASE);
    return;
  }

  tab5_texto(d, TAM_LISTA);   // +60%
  const int y = a.y + LISTA_Y;

  if (s_n_itens == 0) {
    lista_esconder(&s_lista);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(s_listando ? "lendo o cartão...        " : "(vazio)                  ", x, y);
    return;
  }

  // A LISTA: tudo o que sobra abaixo do cabecalho, ate 10px do rodape.
  const area_t janela = { a.x, y, a.largura, (a.y + a.altura - 10) - y };
  lista_desenhar(&s_lista, d, janela, TFT_BLACK, TUDO);
}

// ---------------------------------------------------------------------------
// Toque
// ---------------------------------------------------------------------------
// Vai para a linha 'nova' do arquivo. 'lembrar': guardar a tela atual para o
// ANTES voltar a ela (so o DEPOIS lembra; saltos zeram o caminho).
static void ir_para(int nova, bool lembrar)
{
  if (nova > s_total_linhas - 1) { nova = s_total_linhas - 1; }
  if (nova < 0) { nova = 0; }
  if (nova == s_linha) { return; }
  if (lembrar) {
    if (s_n_voltas == VOLTAS_MAX) { memmove(s_voltas, s_voltas + 1, sizeof(int) * (VOLTAS_MAX - 1)); --s_n_voltas; }
    s_voltas[s_n_voltas++] = s_linha;
  } else {
    s_n_voltas = 0;
  }
  s_linha = nova;
  s_precisa_ler = true;
  sistema_pedir_redesenho_completo();
}

static void pagina_seguinte(void)
{
  if (s_linha + s_cabem < s_total_linhas) { ir_para(s_linha + s_cabem, true); }
}

static void pagina_anterior(void)
{
  if (s_linha <= 0) { return; }
  int volta;
  if (s_n_voltas > 0) { volta = s_voltas[--s_n_voltas]; }
  else                { volta = s_linha - s_cabem; }   // chegou por salto: o mesmo tanto para tras
  if (volta < 0) { volta = 0; }
  s_linha = volta;
  s_precisa_ler = true;
  sistema_pedir_redesenho_completo();
}

// ARRASTE: na lista (pastas/arquivos) rola; no texto, arrastar para cima
// vira para a pagina seguinte, para baixo volta - como folhear.
static int  s_arr_y0 = -1;
static bool s_arr_valeu = false;
static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (s_nivel == NIVEL_CONTEUDO) {
    if (s_e_imagem) { return; }
    if (pressionado) {
      if (s_arr_y0 < 0) { s_arr_y0 = (y >= TEXTO_Y) ? y : -2; s_arr_valeu = false; }   // so arraste que comeca no texto
      else if (s_arr_y0 >= 0 && !s_arr_valeu) {
        const int dy = y - s_arr_y0;
        // Um arraste vira UMA pagina; a soltura dele nao vira clique.
        if (dy <= -ARRASTE_PAGINA) { s_arr_valeu = true; sistema_consumir_toque(); pagina_seguinte(); }
        else if (dy >= ARRASTE_PAGINA) { s_arr_valeu = true; sistema_consumir_toque(); pagina_anterior(); }
      }
    } else {
      s_arr_y0 = -1;
    }
    return;
  }
  if (lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

static void ao_tocar(int x, int y)
{
  const area_t a = sistema_area();
  // ---- VOLTAR ---------------------------------------------------------------
  if (s_nivel != NIVEL_PASTAS && tab5_dentro(x, y, MARGEM_X, TOPO_Y, VOLTAR_L, TOPO_A)) {
    if (s_nivel == NIVEL_CONTEUDO) {
      // De volta a pasta NO MESMO PONTO da lista: quem abriu o 40o log e
      // voltou quer ver o 41o, nao o topo.
      s_nivel = NIVEL_ARQUIVOS;
      soltar_imagem();   // 1,4 MB da foto nao ficam presos na lista
      listar_arquivos(true);
    } else if (char* barra = strrchr(s_pasta, '/')) {
      // Dentro de uma subpasta: VOLTAR sobe um nivel de pasta, nao para a raiz.
      *barra = '\0';
      listar_arquivos();
    } else {
      s_nivel = NIVEL_PASTAS;
      listar_pastas();
    }
    sistema_pedir_redesenho_completo();
    return;
  }

  if (s_nivel == NIVEL_CONTEUDO && s_e_imagem) { return; }   // so o VOLTAR existe numa imagem

  if (s_nivel == NIVEL_CONTEUDO) {
    // Aa: o proximo tamanho de letra. A linha do topo continua a mesma.
    if (tab5_dentro(x, y, a.largura - MARGEM_X - AA_L, TOPO_Y, AA_L, TOPO_A)) {
      s_tam_leitura = (s_tam_leitura + 1) % (int)(sizeof(TAM_LEITURA) / sizeof(TAM_LEITURA[0]));
      sistema_pedir_redesenho_completo();
      return;
    }
    if (y >= NAV_Y && y < NAV_Y + NAV_A) {
      for (int i = 0; i < 4; ++i) {
        if (x < nav_x(a.largura, i) || x >= nav_x(a.largura, i) + nav_l(a.largura)) { continue; }
        switch (i) {
          case 0: ir_para(0, false); break;
          case 1: pagina_anterior(); break;
          case 2: pagina_seguinte(); break;
          // FIM: a ultima tela - aproximada pelo tanto que cabe agora.
          default: ir_para(s_total_linhas - (s_cabem > 1 ? s_cabem : 1), false); break;
        }
        return;
      }
      return;
    }
    // Barra: a posicao do dedo vira a linha.
    if (y >= BARRA_TOQUE_Y && y < BARRA_TOQUE_Y + BARRA_TOQUE_A && s_total_linhas > 0) {
      const int l = a.largura - 2 * MARGEM_X;
      int px = x - MARGEM_X;
      if (px < 0) { px = 0; }
      if (px > l) { px = l; }
      ir_para((int)((int64_t)s_total_linhas * px / l), false);
      return;
    }
    return;   // no texto, toque solto nao faz nada (arrastar vira a pagina)
  }

  // ---- linhas da lista ----------------------------------------------------
  // A lista responde "qual item" com a MESMA geometria do desenho, rolagem
  // incluida, e devolve LISTA_ARRASTE se o toque foi o fim de uma rolagem.
  if (s_listando) { return; }   // a lista na tela esta para ser trocada
  const int i = lista_item_tocado(&s_lista, x, y, nullptr);
  if (i < 0) { return; }

  if (s_nivel == NIVEL_PASTAS) {
    snprintf(s_pasta, sizeof(s_pasta), "%s", s_itens[i].name);
    s_nivel = NIVEL_ARQUIVOS;
    listar_arquivos();
  } else if (s_itens[i].eh_pasta) {
    // SUBPASTA (Sistema/logs, Sistema/energia): entra nela e
    // continua no mesmo nivel de listagem; o VOLTAR sobe de volta.
    const size_t n = strlen(s_pasta);
    if (n + 1 + strlen(s_itens[i].name) < sizeof(s_pasta)) {
      snprintf(s_pasta + n, sizeof(s_pasta) - n, "/%s", s_itens[i].name);
      listar_arquivos();
    }
  } else {
    snprintf(s_arquivo, sizeof(s_arquivo), "%s", s_itens[i].name);
    s_nivel = NIVEL_CONTEUDO;
    s_linha = 0;
    s_cabem = 1;
    s_n_voltas = 0;
    s_arr_y0 = -1;
    s_arr_valeu = false;
    s_e_imagem = tem_sufixo(s_arquivo, ".bmp");
    if (s_e_imagem) {
      char caminho[192];
      caminho_arquivo(caminho, sizeof(caminho));
      abrir_cabecalho_imagem(caminho);
      s_total_linhas = 0;
      ESP_LOGI(TAG, "abrindo imagem %s/%s (%ux%u, ok=%d)", s_pasta, s_arquivo,
               (unsigned)s_img_largura, (unsigned)s_img_altura, (int)s_img_ok);
    } else {
      // ARQUIVO NOVO, TABELA NOVA. Sem isto os deslocamentos do arquivo
      // anterior seriam usados neste - e o fseek levaria ao meio de outra
      // coisa, mostrando conteudo errado sem nenhum erro na tela.
      // (A tabela de deslocamentos e refeita pelo worker, junto da
      // contagem - ver trabalho_ler.)
      s_total_linhas = 0;
      s_n_linhas = 0;
      pedir_leitura(true);
      ESP_LOGI(TAG, "abrindo %s/%s", s_pasta, s_arquivo);
    }
  }
  sistema_pedir_redesenho_completo();
}

// 'extern' e OBRIGATORIO: em C++ um const no escopo de arquivo tem ligacao
// interna, e o apps.cpp nao enxergaria este simbolo.
// ---------------------------------------------------------------------------
// ICONE DA TELA INICIAL
// ---------------------------------------------------------------------------
// Primitivas e fracao do lado, como a marca do Mesh: sai limpo em qualquer
// tamanho, nao gasta flash e nao serrilha. A letra grande que estava aqui era
// a fonte embutida ampliada 6x - cada pixel dela virava um quadrado de 6.
static void icone_arquivos(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(30, 48, 70);
  const uint16_t pasta = d.color565(240, 190, 90);
  const uint16_t aba   = d.color565(205, 155, 60);

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  #define IX(f) (x + (int)((f) * lado))
  #define IY(f) (y + (int)((f) * lado))

  // ORDEM IMPORTA: aba, folha, corpo. Cada camada cobre a junta da anterior,
  // e a pasta sai como peca unica em vez de retangulos encostados. A folha no
  // meio e o que diz que ha coisa DENTRO - desenhada por ULTIMO ela viraria
  // uma etiqueta colada por cima, que e outra leitura.
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
  .name  = "Arquivos",
  .cor   = TFT_SKYBLUE,
  .perfil = { },
  .usa_sd = true,
  .exige_sd = true,         // sem cartao nao ha o que fazer (ver app.h)
  .piso_repouso_mhz = 0,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .periodo_ms = 0,
  .avisos = AVISO_TRABALHO,
  .ao_aviso = ao_aviso,
  .icone = icone_arquivos,
};
