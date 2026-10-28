// APP TAREFAS - a JANELA of monitor of sistema (services/tab5_monitor).
//
// Este app NAO MEDE NADA. Quem mede e o servico, o time
// todo, com o app open ou closed: CPU por tarefa, memoria, picos, e
// eventos disparados por gatilhos. Aqui so se mostra o que ele ja guardou -
// inclusive o que aconteceu enquanto ninguem olhava.
//
// A screen e redesenhada INTEIRA uma vez por segundo. Com o compositor isso nao
// pisca (everything vai para a screen virtual e so o que changed chega on panel), e o
// app nao precisa of none contabilidade of "o que changed".

#include "sdkconfig.h"
#include "app.h"
#include "tab5_screen.hpp"
#include "tab5_ui.hpp"
#include "tab5_monitor.hpp"
#include "tab5_owners.hpp"
#include "list.hpp"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "app.tarefas";

#include "esp_attr.h"
#include "esp_timer.h"
EXT_RAM_BSS_ATTR static mon_foto_t s_f;      // copia of foto of monitor (a screen le daqui)
EXT_RAM_BSS_ATTR static mon_foto_t s_nova;   // a ultima que o monitor entregou
static bool         s_forcar = true;         // next pass redesenha everything

// A TABELA DE TAREFAS ROLA (kernel/list): com muitas tarefas ela
// passava por bottom dos eventos e sumia. Linha of height fixa, lida of foto.
static list_t s_lista;
static const int LIN = 34;

// AGRUPADA POR DONO, como o "user / sistema" dos
// gerenciadores of tarefa: first o que e of APP (o open e os of fundo),
// depois os SERVICOS, e por end o SISTEMA (kernel, drivers, IDF). O dono vem
// declarado por quem criou a tarefa - hal/tab5_owners.hpp -, nao adivinhado
// pelo name. Cada line of lista e one cabecalho of grupo ou uma tarefa.
static const owner_type_t GRUPOS[] = { OWNER_APP, OWNER_BACKGROUND, OWNER_SERVICE, OWNER_SYSTEM };
static const char* const GRUPO_NOME[] = { "SYSTEM", "SERVICES", "OPEN APP", "BACKGROUND APPS" };   // por owner_type_t
static int s_lines[MON_MAX_TAREFAS + 4];   // >= 0: indice em s_f.t; < 0: cabecalho (-1 - owner_type_t)
static int s_nlinhas = 0;

static uint16_t color_grupo(int tipo)
{
  switch (tipo) {
    case OWNER_APP:     return TFT_CYAN;
    case OWNER_BACKGROUND:   return TFT_VIOLET;
    case OWNER_SERVICE: return TFT_GOLD;
    default:           return TFT_DARKGREY;
  }
}

static void montar_lines(void)
{
  s_nlinhas = 0;
  for (owner_type_t g : GRUPOS) {
    int n = 0;
    for (int i = 0; i < s_f.n; ++i) { if (s_f.t[i].type == g) { ++n; } }
    if (!n) { continue; }
    s_lines[s_nlinhas++] = -1 - (int)g;
    for (int i = 0; i < s_f.n; ++i) { if (s_f.t[i].type == g) { s_lines[s_nlinhas++] = i; } }
  }
}
static int  line_height(int, void*);
static void line_draw(lgfx::LovyanGFX& d, int i, int x0, int y, int, void*);
static float        s_cpu[MON_HIST_N];
static mon_evento_t s_ev[6];
static uint32_t     s_flash_usado = 0, s_flash_total = 0;
static bool         s_tem_flash = false;

static const float TAM_CORPO  = 2.6f * 1.5f;   // tamanhos pedidos em 13 e
static const float TAM_TITULO = 2.6f * 1.4f;

// Botao ZERAR PICOS, no rodape (coordenadas relativas a area).
static const int BT_L = 280, BT_A = 56;

static void read_flash_programa(void)
{
  const esp_partition_t* p = esp_ota_get_running_partition();
  if (!p) { return; }
  esp_partition_pos_t pos = { .offset = p->address, .size = p->size };
  esp_image_metadata_t meta;
  if (esp_image_get_metadata(&pos, &meta) != ESP_OK) { return; }
  s_flash_usado = meta.image_len;
  s_flash_total = (uint32_t)p->size;
  s_tem_flash   = true;
}

static bool on_open(void)
{
  read_flash_programa();
  list_set(&s_lista, ROLAGEM_INICIO, 0, line_height, line_draw, nullptr);
  s_forcar = true;
  ESP_LOGI(TAG, "aberto (o monitor ja estava medindo)");
  return true;
}

static void on_close(void) { }

// SRAM of uso geral of P4 = 768 KB menos a parte que o cache L2 toma. Nunca
// menor que o heap measured (se a configuracao mudar of forma inesperada).
static uint32_t sram_fisica(uint32_t heap)
{
  const uint32_t t = 768u * 1024u - (uint32_t)CONFIG_CACHE_L2_CACHE_SIZE;
  return t > heap ? t : heap;
}

static void memory_txt(char* buf, size_t n, uint32_t b)
{
  if      (b >= 1024u * 1024u) { snprintf(buf, n, "%5.1fM", b / (1024.0f * 1024.0f)); }
  else if (b >= 1024u)         { snprintf(buf, n, "%5.1fK", b / 1024.0f); }
  else                         { snprintf(buf, n, "%5uB", (unsigned)b); }
}

// "ha 3m", "ha 2h05" - quanto time atras, a partir of uptime.
static void ha_quanto(char* buf, size_t n, uint32_t now, uint32_t em)
{
  const uint32_t d = (now >= em) ? now - em : 0;
  if      (d < 60)    { snprintf(buf, n, "ha %us", (unsigned)d); }
  else if (d < 3600)  { snprintf(buf, n, "ha %um", (unsigned)(d / 60)); }
  else                { snprintf(buf, n, "ha %uh%02u", (unsigned)(d / 3600), (unsigned)((d % 3600) / 60)); }
}

static uint16_t color_folga(uint32_t livre, uint32_t total)
{
  if (total == 0) { return TFT_DARKGREY; }
  const int pct = (int)((uint64_t)livre * 100 / total);
  return (pct < 10) ? TFT_RED : (pct < 25) ? TFT_YELLOW : TFT_GREENYELLOW;
}

static void body(lgfx::LovyanGFX& d)   { tab5_fonte_body(d); tab5_text(d, TAM_CORPO); }
static void titulo(lgfx::LovyanGFX& d)  { tab5_fonte_body(d); tab5_text(d, TAM_TITULO); }
static void mono(lgfx::LovyanGFX& d)    { d.setFont(&fonts::Font0); d.setTextSize(2); }   // 12px por caractere

// A geometria of screen, a mesma para o drawing e para o touch.
typedef struct { int y_rodape, y_prog, y_psram, y_bloco, y_ram, graf_y0, graf_y1, ev_y0, tab_y0, tab_y1, y_summary, y_cab; } geo_t;
static const int EV_N = 5;
static geo_t geometria(const area_t& a)
{
  geo_t g;
  g.y_rodape = a.y + a.height - BT_A - 8;
  g.y_prog   = g.y_rodape - 30;
  g.y_psram  = g.y_prog - 28;
  g.y_bloco  = g.y_psram - 28;
  g.y_ram    = g.y_bloco - 28;
  g.graf_y1  = g.y_ram - 14;
  g.graf_y0  = g.graf_y1 - 170;
  g.ev_y0    = g.graf_y0 - 14 - 40 - EV_N * 26;
  g.y_summary = a.y + 12;                 // quais apps estao abertos
  g.y_cab    = g.y_summary + LIN + 4;     // cabecalho of tabela
  g.tab_y0   = g.y_cab + LIN + 6;        // abaixo of cabecalho of tabela
  g.tab_y1   = g.ev_y0 - 10;
  return g;
}

static int line_height(int, void*) { return LIN; }

// O TEXTO DE UMA LINHA DA TABELA, exatamente como ela vai para a screen -
// usado para draw e para saber se a line changed since a ultima sample.
typedef struct {
  char name[40], cpu[12], pico[12], nucleo[8], prio[8], stack[12];
  uint16_t color_name, color_pico, color_grupo;
  bool cabecalho;
} line_txt_t;

static void line_text(int li, line_txt_t* t)
{
  memset(t, 0, sizeof(*t));   // inclusive o enchimento: a assinatura le os bytes crus
  const int i = s_lines[li];
  if (i < 0) {   // cabecalho of grupo
    const int tipo = -1 - i;
    int n = 0;
    for (int k = 0; k < s_f.n; ++k) { if (s_f.t[k].type == tipo) { ++n; } }
    t->cabecalho = true;
    t->color_grupo = color_grupo(tipo);
    snprintf(t->name, sizeof(t->name), "%s (%d)", GRUPO_NOME[tipo], n);
    return;
  }
  const mon_tarefa_t& m = s_f.t[i];
  t->color_grupo = color_grupo(m.type);
  t->color_name  = m.stack_livre < 512 ? TFT_RED : TFT_WHITE;
  t->color_pico  = m.pct_pico > 60 ? TFT_ORANGE : TFT_LIGHTGREY;
  snprintf(t->name, sizeof(t->name), "%-14.14s", m.name);
  snprintf(t->cpu,  sizeof(t->cpu),  "%5.1f", (double)m.pct);
  snprintf(t->pico, sizeof(t->pico), "%5.1f", (double)m.pct_pico);
  if (m.core == tskNO_AFFINITY) { snprintf(t->nucleo, sizeof(t->nucleo), " --"); }
  else                          { snprintf(t->nucleo, sizeof(t->nucleo), "%3d", (int)m.core + 1); }
  snprintf(t->prio, sizeof(t->prio), "%3u", (unsigned)m.priority);
  memory_txt(t->stack, sizeof(t->stack), m.stack_livre);
}

static uint32_t line_assinatura(int li)
{
  line_txt_t t;
  line_text(li, &t);
  uint32_t h = 2166136261u;   // FNV-1a sobre o text e as cores
  const uint8_t* p = (const uint8_t*)&t;
  for (size_t k = 0; k < sizeof(t); ++k) { h = (h ^ p[k]) * 16777619u; }
  return h;
}

static void line_draw(lgfx::LovyanGFX& d, int li, int x0, int y, int, void*)
{
  if (li >= s_nlinhas) { return; }
  const int x = x0 + 16;
  line_txt_t t;
  line_text(li, &t);
  body(d);
  d.setTextDatum(top_left);
  if (t.cabecalho) {
    d.setTextColor(t.color_grupo, TFT_BLACK);
    d.drawString(t.name, x, y + 2);
    d.drawFastHLine(x + (int)d.textWidth(t.name) + 12, y + LIN / 2, 400, t.color_grupo);
    return;
  }
  d.fillRect(x0 + 4, y + 4, 6, LIN - 8, t.color_grupo);   // a marca of grupo
  d.setTextColor(t.color_name, TFT_BLACK);
  d.drawString(t.name, x, y);
  d.drawString(t.cpu,  x + 240, y);
  d.setTextColor(t.color_pico, TFT_BLACK);
  d.drawString(t.pico, x + 340, y);
  d.setTextColor(t.color_name, TFT_BLACK);
  d.drawString(t.nucleo, x + 465, y);
  d.drawString(t.prio,   x + 540, y);
  d.drawString(t.stack,  x + 590, y);
}

// ---------------------------------------------------------------------------
// CAMPOS - PINTAR SO O QUE MUDOU
// ---------------------------------------------------------------------------
// A window repintava TUDO a cada sample (1 Hz): erase a area inteira e
// redesenhar tabela, eventos, grafico e memoria - ~70 ms of CPU e 1,6 MB of
// screen por segundo, para mudar meia duzia of numeros. Agora cada line of
// text e one CAMPO que guarda o que esta in screen e so e repintado (apaga a
// faixa dele e escreve) when o text ou a color mudam. A tabela faz o mesmo
// por line (lista_item_mudou). O que muda of fato a cada segundo - o
// grafico, os "ha Ns" - e so isso que vai para a screen.
typedef struct { char txt[96]; uint16_t color; bool ok; } field_t;

static field_t  s_c_summary, s_c_ev_tit, s_c_ev[EV_N], s_c_cpu_tit, s_c_cpu_val;
static field_t  s_c_ram, s_c_bloco, s_c_psram, s_c_prog, s_c_rodape;
static uint32_t s_sig[MON_MAX_TAREFAS + 4];
static int      s_sig_n = -1;              // -1 = a tabela inteira of novo
static uint32_t s_graf_uptime = 0;         // sample que o grafico mostra

static void esquecer_campos(void)
{
  field_t* todos[] = { &s_c_summary, &s_c_ev_tit, &s_c_cpu_tit, &s_c_cpu_val,
                       &s_c_ram, &s_c_bloco, &s_c_psram, &s_c_prog, &s_c_rodape };
  for (field_t* c : todos) { c->ok = false; }
  for (int k = 0; k < EV_N; ++k) { s_c_ev[k].ok = false; }
  s_sig_n = -1;
  s_graf_uptime = 0;
}

// 'l' x 'a' e a faixa of field (apagada antes of write). datum
// top_right: x e a borda DIREITA of text e of faixa.
static void field(lgfx::LovyanGFX& d, field_t* c, const char* txt, uint16_t color,
                  int x, int y, int l, int a, bool right = false)
{
  if (c->ok && c->color == color && strcmp(c->txt, txt) == 0) { return; }
  snprintf(c->txt, sizeof(c->txt), "%s", txt);
  c->color = color;
  c->ok = true;
  d.fillRect(right ? x - l : x, y, l, a, TFT_BLACK);
  d.setTextDatum(right ? top_right : top_left);
  d.setTextColor(color, TFT_BLACK);
  d.drawString(txt, x, y);
  d.setTextDatum(top_left);
}

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  const auto estilo = d.getTextStyle();
  const geo_t g = geometria(a);
  const area_t window = { a.x, g.tab_y0, a.width, g.tab_y1 - g.tab_y0 };

  // A ESTRUTURA (titulos, lines, moldura, botao) so when o sistema acabou
  // of limpar a area - open, voltar of sleep. Nesse caso everything o que os
  // campos achavam que estava in screen ja nao esta.
  const bool estrutura = system_area_foi_limpa() || s_forcar;
  if (estrutura) { esquecer_campos(); }

  // SO A ROLAGEM, entre duas amostras: desloca a tabela e desenha a faixa
  // exposta com a MESMA foto (a que ja esta in screen) - senao a faixa nova
  // nao battery com os pixels deslocados. O resto of screen stays como esta.
  const bool tem = tab5_monitor_foto(&s_nova);
  // FOTO VELHA: o monitor sample a 1 Hz; passando of 3 s, a screen diz que o
  // que mostra e antigo (e o rodape tem que ser repintado para dizer isso).
  const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
  const uint32_t atraso = (tem && now_s > s_nova.uptime_s) ? now_s - s_nova.uptime_s : 0;
  if (tem && !estrutura && s_nova.uptime_s == s_f.uptime_s && atraso <= 3) {
    list_draw(&s_lista, d, window, TFT_BLACK, false);
    d.setTextStyle(estilo);
    return;
  }
  s_forcar = false;
  d.setTextDatum(top_left);

  const int x = a.x + 16, larg = a.width - 32, lin = LIN;
  char buf[96], m1[12], m2[12], m3[12], h[16];

  if (!tem) {
    body(d);
    field(d, &s_c_summary, "monitor ainda sem a primeira amostra...", TFT_LIGHTGREY, x, g.y_summary, larg, lin);
    d.setTextStyle(estilo);
    return;
  }
  s_f = s_nova;
  const uint32_t now = s_f.uptime_s;

  const int y_rodape = g.y_rodape, y_prog = g.y_prog, y_psram = g.y_psram;
  const int y_bloco = g.y_bloco, y_ram = g.y_ram, graf_y0 = g.graf_y0, graf_y1 = g.graf_y1;
  const int ev_y0 = g.ev_y0;
  const int gy0 = graf_y0 + 40, gh = graf_y1 - gy0;

  if (estrutura) {
    // Cabecalho of tabela.
    titulo(d); d.setTextColor(TFT_CYAN, TFT_BLACK);
    d.drawString("TAREFA", x, g.y_cab);
    d.drawString("CPU",    x + 250, g.y_cab);
    d.drawString("PICO",   x + 345, g.y_cab);
    d.drawString("NUC",    x + 460, g.y_cab);
    d.drawString("PRI",    x + 535, g.y_cab);
    d.drawString("PILHA",  x + 600, g.y_cab);
    d.drawFastHLine(x, g.y_cab + lin, larg, TFT_DARKGREY);
    // Moldura of grafico e separador of memoria.
    d.drawRect(x, gy0, larg, gh, TFT_DARKGREY);
    d.drawFastHLine(x, y_ram - 8, larg, TFT_DARKGREY);
    tab5_botao(d, a.x + a.width - 16 - BT_L, y_rodape, BT_L, BT_A, "ZERAR PICOS", TFT_ORANGE,
               TFT_BLACK, TAB5_BOTAO_TEXTO_GRANDE);
  }

  // ---- quem esta open: o app in frente e as partes of fundo ------------
  {
    body(d);
    background_info_t fi[4];
    const int nf = system_fundo_listar(fi, 4);
    char summary[96];
    int k = snprintf(summary, sizeof(summary), "EM FUNDO: ");
    if (nf == 0) { snprintf(summary + k, sizeof(summary) - k, "nenhum app"); }
    for (int j = 0; j < nf && k < (int)sizeof(summary); ++j) {
      k += snprintf(summary + k, sizeof(summary) - k, "%s%s", j ? ", " : "", fi[j].name);
    }
    field(d, &s_c_summary, summary, nf ? TFT_VIOLET : TFT_DARKGREY, x, g.y_summary, larg, lin);
  }

  // ---- tabela of tarefas: so as lines que changed ------------------------
  {
    const int n_antes = s_nlinhas;
    int antes[MON_MAX_TAREFAS + 4];
    memcpy(antes, s_lines, sizeof(int) * (size_t)n_antes);
    montar_lines();
    const bool mesma_forma = s_sig_n == s_nlinhas && n_antes == s_nlinhas
                          && memcmp(antes, s_lines, sizeof(int) * (size_t)s_nlinhas) == 0;
    if (!mesma_forma) {
      // Entrou ou exited tarefa, ou a order changed: a lista inteira of novo.
      list_items_changed(&s_lista, s_nlinhas);
      for (int li = 0; li < s_nlinhas; ++li) { s_sig[li] = line_assinatura(li); }
      s_sig_n = s_nlinhas;
      list_draw(&s_lista, d, window, TFT_BLACK, true);
    } else {
      for (int li = 0; li < s_nlinhas; ++li) {
        const uint32_t sg = line_assinatura(li);
        if (sg != s_sig[li]) { s_sig[li] = sg; list_item_changed(&s_lista, li); }
      }
      list_draw(&s_lista, d, window, TFT_BLACK, estrutura);
    }
  }

  // ---- eventos (o que os gatilhos registraram) ----------------------------
  titulo(d);
  snprintf(buf, sizeof(buf), "EVENTOS  (%lu desde o boot)", (unsigned long)tab5_monitor_total_eventos());
  field(d, &s_c_ev_tit, buf, TFT_CYAN, x, ev_y0, larg, 36);
  mono(d);
  const int nev = tab5_monitor_eventos(s_ev, EV_N);
  for (int k = 0; k < EV_N; ++k) {
    const int ey = ev_y0 + 40 + k * 26;
    uint16_t color = TFT_LIGHTGREY;
    if (k >= nev) {
      buf[0] = '\0';
      if (k == 0) { snprintf(buf, sizeof(buf), "nenhum - tudo dentro dos limites"); color = TFT_DARKGREY; }
    } else {
      ha_quanto(h, sizeof(h), now, s_ev[k].uptime_s);
      snprintf(buf, sizeof(buf), "%-8s %.46s", h, s_ev[k].text);
      color = (k == 0) ? TFT_YELLOW : TFT_LIGHTGREY;
    }
    field(d, &s_c_ev[k], buf, color, x, ey, larg, 26);
  }

  // ---- grafico of CPU (5 min) ---------------------------------------------
  {
    titulo(d);
    ha_quanto(h, sizeof(h), now, s_f.cpu_pico_em);
    snprintf(buf, sizeof(buf), "CPU 5 min   pico %.0f%% %s", (double)s_f.cpu_pico, h);
    field(d, &s_c_cpu_tit, buf, TFT_CYAN, x, graf_y0, larg - 120, 36);
    snprintf(buf, sizeof(buf), "%.0f%%", (double)s_f.cpu);
    field(d, &s_c_cpu_val, buf, s_f.cpu > 80 ? TFT_RED : s_f.cpu > 50 ? TFT_YELLOW : TFT_GREENYELLOW,
          x + larg, graf_y0, 120, 36, true);

    // O grafico muda a cada sample (uma nova entra pela right): o MIOLO e
    // repintado, uma vez por sample; a moldura stays.
    if (s_graf_uptime != now) {
      s_graf_uptime = now;
      d.fillRect(x + 1, gy0 + 1, larg - 2, gh - 2, TFT_BLACK);
      for (int k = 1; k < 4; ++k) {
        d.drawFastHLine(x + 1, gy0 + (gh * k) / 4, larg - 2, TAB5_GRAFICO_COR_GRADE);
        d.drawFastVLine(x + (larg * k) / 4, gy0 + 1, gh - 2, TAB5_GRAFICO_COR_GRADE);
      }
      const int n = tab5_monitor_historico(s_cpu, nullptr, nullptr, MON_HIST_N);
      int px0 = 0, py0 = 0;
      for (int k = 0; k < n; ++k) {
        // Alinhado a right: o mais novo sempre in borda, o grafico enche of
        // right para a left nos primeiros 5 minutos.
        const int px = x + 2 + ((MON_HIST_N - n + k) * (larg - 4)) / (MON_HIST_N - 1);
        const int py = gy0 + gh - 2 - (int)(s_cpu[k] * (float)(gh - 4) / 100.0f);
        if (k > 0) { d.drawLine(px0, py0, px, py, TFT_GREENYELLOW); }
        px0 = px; py0 = py;
      }
    }
  }

  // ---- memoria: now, total e o MENOR que ja sobrou, com when ----------
  mono(d);
  const mon_memory_t& r = s_f.ram;
  // O total mostrado e a SRAM FISICA of P4 (768 KB menos o cache L2), nao so o
  // heap: o resto e code e data fixos of sistema. A color of folga continua
  // measurement contra o heap, que e o que of fato pode ser alocado.
  memory_txt(m1, sizeof(m1), r.livre); memory_txt(m2, sizeof(m2), sram_fisica(r.total)); memory_txt(m3, sizeof(m3), r.min_livre);
  ha_quanto(h, sizeof(h), now, r.min_livre_em);
  // COLUNAS FIXAS (Font0 e monoespacada): rotulo 6, descricao 12, valor 14 -
  // o "min" cai in mesma coluna nas tres lines. Antes cada line contava os
  // proprios espacos e o "min" of bloco saia 2 casas fora.
  char v[24];
  snprintf(v, sizeof(v), "%s/%s", m1, m2);
  snprintf(buf, sizeof(buf), "%-6s%-12s%-14s min %s %s", "RAM", "livre", v, m3, h);
  field(d, &s_c_ram, buf, color_folga(r.livre, r.total), x, y_ram, larg, 24);
  memory_txt(m1, sizeof(m1), r.maior_bloco); memory_txt(m3, sizeof(m3), r.min_bloco);
  ha_quanto(h, sizeof(h), now, r.min_bloco_em);
  snprintf(buf, sizeof(buf), "%-6s%-12s%-14s min %s %s", "", "maior bloco", m1, m3, h);
  field(d, &s_c_bloco, buf, r.maior_bloco < 16 * 1024 ? TFT_RED : TFT_LIGHTGREY, x, y_bloco, larg, 24);
  const mon_memory_t& p = s_f.psram;
  memory_txt(m1, sizeof(m1), p.livre); memory_txt(m2, sizeof(m2), p.total); memory_txt(m3, sizeof(m3), p.min_livre);
  ha_quanto(h, sizeof(h), now, p.min_livre_em);
  snprintf(v, sizeof(v), "%s/%s", m1, m2);
  snprintf(buf, sizeof(buf), "%-6s%-12s%-14s min %s %s", "PSRAM", "livre", v, m3, h);
  field(d, &s_c_psram, buf, color_folga(p.livre, p.total), x, y_psram, larg, 24);
  if (s_tem_flash) {
    memory_txt(m1, sizeof(m1), s_flash_usado); memory_txt(m2, sizeof(m2), s_flash_total);
    snprintf(v, sizeof(v), "%s/%s", m1, m2);
    snprintf(buf, sizeof(buf), "%-6s%-12s%-14s", "PROG", "usado", v);
    field(d, &s_c_prog, buf, color_folga(s_flash_total - s_flash_usado, s_flash_total), x, y_prog, larg, 24);
  }

  // ---- rodape: uptime (o botao ZERAR PICOS e estrutura) -------------------
  const uint32_t up = now;
  if      (up < 3600)  { snprintf(h, sizeof(h), "%um%02us", (unsigned)(up / 60), (unsigned)(up % 60)); }
  else if (up < 86400) { snprintf(h, sizeof(h), "%uh%02um", (unsigned)(up / 3600), (unsigned)((up % 3600) / 60)); }
  else                 { snprintf(h, sizeof(h), "%ud%02uh", (unsigned)(up / 86400), (unsigned)((up % 86400) / 3600)); }
  body(d);
  if (atraso > 3) {
    snprintf(buf, sizeof(buf), "monitor with no sample for %us", (unsigned)atraso);
    field(d, &s_c_rodape, buf, TFT_YELLOW, x, y_rodape + 10, larg - BT_L - 16, lin);
  } else {
    snprintf(buf, sizeof(buf), "%d tarefas | no ar %s", s_f.n, h);
    field(d, &s_c_rodape, buf, TFT_LIGHTGREY, x, y_rodape + 10, larg - BT_L - 16, lin);
  }

  d.setTextStyle(estilo);
}

// Arrastar in tabela rola; o resto of screen nao rola.
static void on_touch_level(int x, int y, bool pressed)
{
  if (list_drag(&s_lista, x, y, pressed)) { system_request_redraw(); }
}

static void on_touch(int x, int y)
{
  if (list_item_touched(&s_lista, x, y, nullptr) == LISTA_ARRASTE) { return; }   // end of arraste nao e click
  const area_t a = system_area();
  const int bx = a.width - 16 - BT_L;
  const int by = a.height - BT_A - 8;
  if (x >= bx && x < bx + BT_L && y >= by && y < by + BT_A) {
    tab5_monitor_reset_picos();
    system_request_redraw();
  }
}

// ---------------------------------------------------------------------------
// ICONE DA TELA INICIAL
// ---------------------------------------------------------------------------
// Primitivas e fracao of lado, como a marca of Mesh: exit limpo em qualquer
// tamanho, nao gasta flash e nao serrilha. A letra grande que estava aqui era
// a fonte embutida ampliada 6x - cada pixel dela virava one quadrado of 6.
static void icon_tarefas(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(22, 40, 26);
  const uint16_t barra = TFT_GREENYELLOW;

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  // Barras of height diferente: o app e one monitor, e monitor se reconhece pelo
  // profile irregular. Barras iguais leriam como "lista", que e outra coisa.
  const float alt[4] = { 0.30f, 0.52f, 0.22f, 0.42f };
  const int larg = lado / 9;
  const int vao  = lado / 22;
  const int base = y + (int)(0.76f * lado);
  int bx = x + (int)(0.20f * lado);

  for (int i = 0; i < 4; ++i) {
    const int h = (int)(alt[i] * lado);
    // A ultima em laranja: o app existe para achar quem esta gastando, e o
    // icon ja aponta para isso.
    d.fillRoundRect(bx, base - h, larg, h, larg / 3,
                    (i == 1) ? TFT_ORANGE : barra);
    bx += larg + vao;
  }

  // Linha of base: of chao as barras e fecha a leitura of grafico.
  d.fillRect(x + (int)(0.16f * lado), base + lado / 40,
             (int)(0.68f * lado), lado / 40, d.color565(90, 130, 95));
}

extern const app_t APP_TASKS = {
  .name  = "Tasks",
  .color   = TFT_GREENYELLOW,
  .profile = { },
  .piso_sleep_mhz = 0,
  .on_open  = on_open,
  .on_close = on_close,
  .draw  = draw,
  .on_touch  = on_touch,
  .on_touch_level = on_touch_level,
  .periodo_ms = 1000,   // o monitor sample a 1Hz; a window acompanha
  .icon = icon_tarefas,
};
