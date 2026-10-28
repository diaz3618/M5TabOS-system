#include "conversation.hpp"
#include "virtual_keyboard.hpp"
#include "tab5_ui.hpp"
#include "tab5_screen.hpp"
#include "tab5_keyboard.hpp"
#include "tab5_time.hpp"
#include "tab5_emoji.hpp"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

// ---------------------------------------------------------------------------
// MEDIDAS - as of app Mesh, calibradas em hardware on longo of setembro (ver
// os notices of TAM_MSG/TAM_ESCRITA/LINHA_STEP em app_mesh/app_mesh.cpp). Os
// STEP sao pixels fixos que acompanham o tamanho of fonte: mexeu num, mexa no
// outro ("height of glifo (tamanho*8) + ~4px of respiro").
// ---------------------------------------------------------------------------
static const float TAM_MSG     = 3.8f;
static const float TAM_NOME    = 2.415f;
static const float TAM_META    = 1.955f;
static const float TAM_ESCRITA = 3.2f;
static const int   LINHA_STEP  = 34;    // acompanha TAM_MSG
static const int   DIVISOR_A   = 40;
static const int   VAO_BALAO   = 8;
// A HORA EMBAIXO DO BALAO ("14:32"), of lado of quem falou. So when a
// message tem data of verdade - sem ela (clock nao acertado) nao ha o que
// dizer, e a faixa nem existe.
static const float TAM_HORA    = 1.7f;
static const int   HORA_A      = 22;
static const int   ESCRITA_A   = 80;
static const int   ENV_L       = 110;   // botao of send + margem
static const int   MARCA_L     = 26;    // marca of send_state, a left of bubble
static const int   CAP_MAX     = 256;

struct conversation_bubble_s {
  uint32_t assin;               // text+name+meta+lado+width of ultima measurement (ver medir)
  int16_t  bl, ba, height, name_a, meta_a;
  uint16_t color_name, color_meta;
  bool     minha, start_dia;
  uint8_t  n_lines, send_state;
  uint32_t when;
  char     name[16];
  char     meta[24];
  char     lines[CONVERSA_MAX_LINHAS][CONVERSA_LINHA_BYTES];
};
typedef struct conversation_bubble_s bubble_t;

static uint16_t fundo(lgfx::LovyanGFX& d)            { return d.color565(13, 17, 23); }
static uint16_t color_bubble(lgfx::LovyanGFX& d, bool m) { return m ? d.color565(22, 33, 26) : d.color565(24, 28, 36); }

// ---------------------------------------------------------------------------
// QUEBRA DE LINHAS
// ---------------------------------------------------------------------------
int conversation_break_lines(lgfx::LovyanGFX& d, const char* txt, int width_px,
                            char lines[][CONVERSA_LINHA_BYTES], int max_lines)
{
  int n = 0;
  char current[CONVERSA_LINHA_BYTES] = "";
  const char* p = txt;
  while (*p && n < max_lines) {
    const char* end = p;
    while (*end && *end != ' ') { ++end; }
    size_t size = (size_t)(end - p);
    // Palavra maior que o buffer of uma line: entra so o que couber (sem
    // partir one caractere UTF-8), e o resto iteration pelo loop como "a next
    // palavra" - nada e descartado.
    const bool cortada = size >= CONVERSA_LINHA_BYTES;
    if (cortada) {
      size = CONVERSA_LINHA_BYTES - 1;
      while (size > 0 && ((unsigned char)p[size] & 0xC0) == 0x80) { --size; }
    }
    char palavra[CONVERSA_LINHA_BYTES];
    memcpy(palavra, p, size);
    palavra[size] = 0;

    // O LIMITE E A LARGURA, NUNCA O BUFFER. Antes o teste montava "current +
    // palavra" num buffer of tamanho of uma line: when os BYTES acabavam
    // antes dos pixels (acento e emoji ocupam 2-4 bytes), o snprintf cortava,
    // o text cortado "cabia", e o end of palavra sumia - a line terminava no
    // middle of screen e o resto nao aparecia em lugar nenhum. Agora o que nao
    // cabe no buffer simplesmente vai para a line of bottom.
    const size_t usado = strlen(current);
    const bool cabe_bytes = usado + (usado ? 1 : 0) + size < CONVERSA_LINHA_BYTES;
    char teste[CONVERSA_LINHA_BYTES];
    if (cabe_bytes) {
      if (usado) { snprintf(teste, sizeof(teste), "%s %s", current, palavra); }
      else       { snprintf(teste, sizeof(teste), "%s", palavra); }
    }

    if (!cortada && cabe_bytes && d.textWidth(teste) <= width_px) {
      memcpy(current, teste, strlen(teste) + 1);
      p = end;
      while (*p == ' ') { ++p; }
    } else if (!current[0]) {
      // Linha vazia e nem assim coube: corta por caractere (nunca no middle of
      // one UTF-8) e avanca SO o que entrou - o resto of palavra segue adiante.
      memcpy(current, palavra, size + 1);
      tab5_cortar_on_caber(d, current, width_px);
      size_t consumido = strlen(current);
      if (consumido == 0) {   // nem 1 caractere coube: avanca 1 mesmo assim (nunca trava)
        const unsigned char c0 = (unsigned char)palavra[0];
        consumido = ((c0 & 0xE0) == 0xC0) ? 2 : ((c0 & 0xF0) == 0xE0) ? 3 : ((c0 & 0xF8) == 0xF0) ? 4 : 1;
      }
      p += consumido;
    } else {
      snprintf(lines[n++], CONVERSA_LINHA_BYTES, "%s", current);
      current[0] = 0;
    }
  }
  if (current[0] && n < max_lines) { snprintf(lines[n++], CONVERSA_LINHA_BYTES, "%s", current); }
  return n;
}

// ---------------------------------------------------------------------------
// DIVISOR DE DIA - "HOJE", "ONTEM", "DD/MM" (no fuso local)
// ---------------------------------------------------------------------------
static int dia_local(uint32_t epoch)
{
  struct tm t;
  tab5_local_time(epoch, &t);
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

static void formatar_dia(uint32_t epoch, char* out, size_t size)
{
  if (const uint32_t now = tab5_time_epoch()) {
    if (dia_local(epoch) == dia_local(now))          { snprintf(out, size, "HOJE");  return; }
    if (dia_local(epoch) == dia_local(now - 86400u)) { snprintf(out, size, "ONTEM"); return; }
  }
  struct tm t;
  tab5_local_time(epoch, &t);
  snprintf(out, size, "%02d/%02d", t.tm_mday, t.tm_mon + 1);
}

static void draw_divisor(lgfx::LovyanGFX& d, int x0, int larg, int y, uint32_t epoch)
{
  char txt[16];
  formatar_dia(epoch, txt, sizeof(txt));
  tab5_text(d, 1.7f);
  const int pl = (int)d.textWidth(txt) + 32;
  const int pa = DIVISOR_A - 10;
  const uint16_t pilula = d.color565(38, 42, 50);
  d.fillRect(x0, y, larg, DIVISOR_A, fundo(d));
  d.fillRoundRect(x0 + (larg - pl) / 2, y + 5, pl, pa, pa / 2, pilula);
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_LIGHTGREY, pilula);
  d.drawString(txt, x0 + larg / 2, y + DIVISOR_A / 2);
  d.setTextDatum(top_left);
}

// Marca of send_state, on lado of bubble: tres pontos (saindo), visto (foi), X (falhou).
static void marca_send_state(lgfx::LovyanGFX& d, int cx, int cy, uint8_t send_state)
{
  d.fillRect(cx - MARCA_L / 2, cy - 12, MARCA_L, 24, fundo(d));
  if (send_state == CONVERSA_ENVIO_PENDENTE) {
    for (int k = -1; k <= 1; ++k) { d.fillCircle(cx + k * 7, cy, 2, TFT_DARKGREY); }
  } else if (send_state == CONVERSA_ENVIO_OK) {
    d.drawLine(cx - 8, cy, cx - 3, cy + 6, TFT_GREENYELLOW);
    d.drawLine(cx - 8, cy + 1, cx - 3, cy + 7, TFT_GREENYELLOW);
    d.drawLine(cx - 3, cy + 6, cx + 8, cy - 7, TFT_GREENYELLOW);
    d.drawLine(cx - 3, cy + 7, cx + 8, cy - 6, TFT_GREENYELLOW);
  } else if (send_state == CONVERSA_ENVIO_OUVIDO) {
    // O mesmo visto, em cinza: exited e a rede ouviu, mas quem devia
    // confirm (o destinatario) ainda nao confirmou.
    d.drawLine(cx - 8, cy, cx - 3, cy + 6, TFT_DARKGREY);
    d.drawLine(cx - 8, cy + 1, cx - 3, cy + 7, TFT_DARKGREY);
    d.drawLine(cx - 3, cy + 6, cx + 8, cy - 7, TFT_DARKGREY);
    d.drawLine(cx - 3, cy + 7, cx + 8, cy - 6, TFT_DARKGREY);
  } else if (send_state == CONVERSA_ENVIO_SEM_RESPOSTA) {
    // Cinza, circulo e "?": nao e falha, e nao saber.
    d.drawCircle(cx, cy, 9, TFT_DARKGREY);
    d.drawCircle(cx, cy, 8, TFT_DARKGREY);
    const auto e = d.getTextStyle();
    d.setFont(tab5_fonte_suave(true, 16));
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_DARKGREY);
    d.drawString("?", cx, cy + 1);
    d.setTextStyle(e);
    tab5_fonte_body(d);
  } else if (send_state == CONVERSA_ENVIO_FALHOU) {
    d.drawLine(cx - 6, cy - 6, cx + 6, cy + 6, TFT_RED);
    d.drawLine(cx - 6, cy + 6, cx + 6, cy - 6, TFT_RED);
    d.drawLine(cx - 5, cy - 6, cx + 7, cy + 6, TFT_RED);
    d.drawLine(cx - 5, cy + 6, cx + 7, cy - 6, TFT_RED);
  }
}

// ---------------------------------------------------------------------------
// MEDIR - uma vez por mudanca, nunca por frame
// ---------------------------------------------------------------------------
// So o que changed e re-medido. Mensagem chegando ou confirmacao of entrega
// (que so troca a marca) re-media TODOS os baloes - ate 256, quebrando line
// palavra a palavra - in tarefa of interface: se caia no middle of digitacao,
// aquele frame atrasava e a key engasgava. Cada bubble guarda a assinatura
// of que o define (text, name, meta, lado, width); igual, so os campos
// baratos (marca, data) sao atualizados. Indice deslocado (historico que
// rolou) of assinatura diferente e e re-medido - nunca reaproveita errado.
//
// Devolve true when a ESTRUTURA changed (count ou height of algum bubble):
// ai a lista inteira se reorganiza. Senao, so os baloes que changed (a marca
// of send_state, tipicamente) sao anotados para repintar.
static int hora_a(const bubble_t* b) { return tab5_time_is_valid(b->when) ? HORA_A : 0; }

static uint32_t assinatura(const conversation_msg_t* m, int larg)
{
  uint32_t h = 2166136261u;   // FNV-1a
  auto mais = [&](const char* s) { for (; *s; ++s) { h = (h ^ (uint8_t)*s) * 16777619u; } h = (h ^ 0xFF) * 16777619u; };
  mais(m->text); mais(m->name); mais(m->meta);
  h = (h ^ (uint32_t)m->minha) * 16777619u;
  h = (h ^ (uint32_t)larg) * 16777619u;
  return h | 1u;   // 0 = nunca measured
}

static bool medir(conversation_t* c, lgfx::LovyanGFX& d, int larg)
{
  c->larg = larg;
  c->medir = false;
  const int n_antes = c->n;
  int n = c->count ? c->count(c->ctx) : 0;
  if (n > CAP_MAX) { n = CAP_MAX; }
  if (n > c->cap) {
    const int nova = ((n + 31) / 32) * 32;
    void* p = heap_caps_realloc(c->bal, sizeof(bubble_t) * nova, MALLOC_CAP_SPIRAM);
    if (!p) { c->n = 0; return true; }
    c->bal = (bubble_t*)p;
    c->cap = nova;
  }
  c->n = n;

  const auto estilo = d.getTextStyle();
  tab5_text(d, TAM_NOME);
  const int alt_name = d.fontHeight();
  tab5_text(d, TAM_META);
  const int alt_meta = d.fontHeight();
  const int respiro = LINHA_STEP / 4;

  bool estrutura = (n != n_antes);
  int dia_ant = 0;
  static conversation_msg_t m;   // 300+ bytes: fora of stack of interface
  for (int i = 0; i < n; ++i) {
    bubble_t* b = &c->bal[i];
    memset(&m, 0, sizeof(m));
    if (!c->read(i, &m, c->ctx)) { m.text[0] = 0; }
    const int16_t alt_antes = (i < n_antes) ? b->height : -1;
    if (i < n_antes && (b->send_state != m.send_state || b->when != m.when
                    || b->color_name != m.color_name || b->color_meta != m.color_meta)) { list_item_changed(&c->lista, i); }
    b->send_state = m.send_state;
    b->when = m.when;
    b->color_name = m.color_name;
    b->color_meta = m.color_meta;
    const uint32_t as = assinatura(&m, larg);
    if (i < n_antes && b->assin == as) {
      // Mesmo content: medidas valem. So o divisor of dia, que depende of vizinho.
      b->start_dia = false;
      if (tab5_time_is_valid(b->when)) {
        const int dia = dia_local(b->when);
        b->start_dia = (dia != dia_ant);
        dia_ant = dia;
      }
      b->height = (int16_t)((b->start_dia ? DIVISOR_A : 0) + b->ba + hora_a(b) + VAO_BALAO);
      if (b->height != alt_antes) { estrutura = true; }
      continue;
    }
    if (i < n_antes) { list_item_changed(&c->lista, i); }
    b->assin = as;
    b->minha = m.minha;
    snprintf(b->name, sizeof(b->name), "%s", m.name);
    snprintf(b->meta, sizeof(b->meta), "%s", m.meta);

    // Teto of width: 38% a minha (on lado of marca), 60% a recebida.
    const int pct = (int)(larg * (b->minha ? 0.38f : 0.60f));
    const int teto = (pct < larg - 28 - MARCA_L) ? pct : (larg - 28 - MARCA_L);

    char txt[CONVERSA_TEXTO];
    tab5_emoji_prepare(m.text, txt, sizeof(txt));
    tab5_text(d, TAM_MSG);
    b->n_lines = (uint8_t)conversation_break_lines(d, txt, teto - 24, b->lines, CONVERSA_MAX_LINHAS);

    int bl = 24;
    for (int k = 0; k < b->n_lines; ++k) {
      const int lw = (int)d.textWidth(b->lines[k]) + 24;
      if (lw > bl) { bl = lw; }
    }
    if (b->name[0]) {
      tab5_text(d, TAM_NOME);
      const int lw = (int)d.textWidth(b->name) + 24;
      if (lw > bl) { bl = lw; }
    }
    if (b->meta[0]) {
      tab5_text(d, TAM_META);
      const int lw = (int)d.textWidth(b->meta) + 24;
      if (lw > bl) { bl = lw; }
    }
    if (bl > teto) { bl = teto; }

    b->name_a = b->name[0] ? (int16_t)(alt_name + respiro) : 0;
    b->meta_a = b->meta[0] ? (int16_t)(alt_meta + respiro) : 0;
    b->bl = (int16_t)bl;
    b->ba = (int16_t)(24 + b->n_lines * LINHA_STEP + b->name_a + b->meta_a);

    // Divisor antes of primeira message of cada dia. Sem data (clock nao
    // acertado when ela chegou) nao abre dia - senao virava 31/12/1969.
    b->start_dia = false;
    if (tab5_time_is_valid(b->when)) {
      const int dia = dia_local(b->when);
      b->start_dia = (dia != dia_ant);
      dia_ant = dia;
    }
    b->height = (int16_t)((b->start_dia ? DIVISOR_A : 0) + b->ba + hora_a(b) + VAO_BALAO);
    if (b->height != alt_antes) { estrutura = true; }
  }
  d.setTextStyle(estilo);
  return estrutura;
}

static int bubble_height(int i, void* ctx)
{
  const conversation_t* c = (const conversation_t*)ctx;
  return (c->bal && i < c->n) ? c->bal[i].height : 0;
}

static void bubble_draw(lgfx::LovyanGFX& d, int i, int x0, int y, int larg, void* ctx)
{
  const conversation_t* c = (const conversation_t*)ctx;
  if (!c->bal || i >= c->n) { return; }
  const bubble_t* b = &c->bal[i];
  const auto estilo = d.getTextStyle();

  if (b->start_dia) { draw_divisor(d, x0, larg, y, b->when); y += DIVISOR_A; }

  const int bx = b->minha ? (x0 + larg - b->bl - 16) : (x0 + 16);
  const uint16_t color = color_bubble(d, b->minha);
  d.fillRoundRect(bx, y, b->bl, b->ba, 10, color);

  // Recorte no proprio bubble (dentro of of faixa): one emoji no end of line
  // nao pinta o vao ate o next.
  int32_t cx, cy, cl, ca;
  d.getClipRect(&cx, &cy, &cl, &ca);
  const int rx0 = (bx > cx) ? bx : cx;
  const int ry0 = (y > cy) ? y : cy;
  const int rx1 = (bx + b->bl < cx + cl) ? (bx + b->bl) : (cx + cl);
  const int ry1 = (y + b->ba < cy + ca) ? (y + b->ba) : (cy + ca);
  if (rx1 > rx0 && ry1 > ry0) {
    d.setClipRect(rx0, ry0, rx1 - rx0, ry1 - ry0);
    d.setTextDatum(top_left);
    int cur = y + 14;
    if (b->name[0]) {
      tab5_text(d, TAM_NOME);
      d.setTextColor(b->color_name ? b->color_name : (uint16_t)TFT_SKYBLUE, color);
      d.drawString(b->name, bx + 12, cur);
      cur += b->name_a;
    }
    tab5_text(d, TAM_MSG);
    d.setTextColor(TFT_WHITE, color);
    for (int k = 0; k < b->n_lines; ++k) { d.drawString(b->lines[k], bx + 12, cur + k * LINHA_STEP); }
    cur += b->n_lines * LINHA_STEP + LINHA_STEP / 4;
    if (b->meta[0]) {
      tab5_text(d, TAM_META);
      d.setTextColor(b->color_meta ? b->color_meta : (uint16_t)TFT_DARKGREY, color);
      d.drawString(b->meta, bx + 12, cur);
    }
    d.setClipRect(cx, cy, cl, ca);
  }
  if (b->minha) { marca_send_state(d, bx - 8 - MARCA_L / 2, y + b->ba / 2, b->send_state); }

  if (hora_a(b)) {
    struct tm t;
    tab5_local_time(b->when, &t);
    char hora[8];
    snprintf(hora, sizeof(hora), "%02d:%02d", t.tm_hour, t.tm_min);
    const int hy = y + b->ba;
    d.fillRect(x0, hy, larg, HORA_A, fundo(d));
    tab5_text(d, TAM_HORA);
    d.setTextColor(TFT_DARKGREY, fundo(d));
    d.setTextDatum(b->minha ? top_right : top_left);
    d.drawString(hora, b->minha ? bx + b->bl - 6 : bx + 6, hy + 3);
    d.setTextDatum(top_left);
  }
  d.setTextStyle(estilo);
}

// ---------------------------------------------------------------------------
// CICLO DE VIDA
// ---------------------------------------------------------------------------
void conversation_open(conversation_t* c, paint_t* p, conversation_count_t count, conversation_read_t read,
                    conversation_send_t send, void* ctx)
{
  bubble_t* bal = c->bal;   // o cache sobrevive a reabrir (mesmo app)
  const int cap = c->cap;
  memset(c, 0, sizeof(*c));
  c->bal = bal; c->cap = cap;
  c->count = count; c->read = read; c->send = send; c->ctx = ctx; c->p = p;
  c->medir = true;
  virtual_keyboard_phrase(&c->kb, true);   // text corrido: maiuscula no start of phrase
  c->barra_base = c->barra_modo = c->barra_larg = c->barra_cursor = c->barra_topo = c->barra_env = -1;
  list_set(&c->lista, ROLAGEM_FIM, 0, bubble_height, bubble_draw, c);
}

void conversation_close(conversation_t* c)
{
  if (c->bal) { heap_caps_free(c->bal); }
  c->bal = nullptr; c->cap = 0; c->n = 0;
  virtual_keyboard_close(&c->kb);
}

void conversation_changed(conversation_t* c) { c->medir = true; system_request_redraw(); }

bool conversation_keyboard_open(const conversation_t* c) { return virtual_keyboard_is_open(&c->kb); }

void conversation_area_keyboard(conversation_t* c, const area_t& ka) { c->tec = ka; }

// O rectangle of keyboard: o que o app deu, ou o of propria conversa.
static const area_t& area_tec(const conversation_t* c) { return c->tec.width > 0 ? c->tec : c->ret; }

// Reposiciona everything (keyboard abriu/fechou): pass complete, pelas duas vias
// que a regra of repintura requires (paint of app + agenda of sistema).
static void request_complete(conversation_t* c)
{
  if (c->p) { paint_request(c->p); }
  system_request_redraw_complete();
}

static void close_keyboard(conversation_t* c)
{
  if (!virtual_keyboard_is_open(&c->kb)) { return; }
  virtual_keyboard_close(&c->kb);
  request_complete(c);
}

void conversation_close_keyboard(conversation_t* c) { close_keyboard(c); }

void conversation_switch(conversation_t* c)
{
  close_keyboard(c);
  c->medir = true;
  list_on_start(&c->lista);
  request_complete(c);
}

// ---------------------------------------------------------------------------
// EDICAO DO RASCUNHO - sempre no cursor, sempre em fronteira of caractere UTF-8
// ---------------------------------------------------------------------------
static int utf8_anterior(const char* s, int i)
{
  if (i <= 0) { return 0; }
  --i;
  while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) { --i; }
  return i;
}

static int utf8_seguinte(const char* s, int i)
{
  if (!s[i]) { return i; }
  ++i;
  while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80) { ++i; }
  return i;
}

static void cursor_ajustar(conversation_t* c)
{
  const int n = (int)strlen(c->rascunho);
  if (c->cursor > n) { c->cursor = n; }
  if (c->cursor < 0) { c->cursor = 0; }
}

static void inserir(conversation_t* c, const char* t, size_t n)
{
  cursor_ajustar(c);
  const size_t len = strlen(c->rascunho);
  const size_t request = n;   // 't' tem EXATAMENTE n bytes - nao e uma string terminada
  size_t teto = sizeof(c->rascunho) - 1;
  if (c->limit > 0 && (size_t)c->limit < teto) { teto = (size_t)c->limit; }
  if (len >= teto) { return; }
  if (len + n > teto) { n = teto - len; }
  // Nunca corta no middle of one caractere: recua ate a fronteira. So olha
  // dentro dos 'request' bytes recebidos. Antes isto era strlen(t) - e a
  // key of keyboard virtual chega como UM char released in stack, sem o zero
  // no end: strlen lia o lixo depois dele, e when o byte seguinte calhava
  // of parecer continuacao of UTF-8 (0x80-0xBF) a letra era descartada.
  // Dependia of que tinha sobrado in stack - por isso "of nada".
  while (n > 0 && n < request && ((unsigned char)t[n] & 0xC0) == 0x80) { --n; }
  if (!n) { return; }
  memmove(c->rascunho + c->cursor + n, c->rascunho + c->cursor, len - c->cursor + 1);
  memcpy(c->rascunho + c->cursor, t, n);
  c->cursor += (int)n;
}

static void erase_antes(conversation_t* c)
{
  cursor_ajustar(c);
  if (!c->cursor) { return; }
  const int a = utf8_anterior(c->rascunho, c->cursor);
  memmove(c->rascunho + a, c->rascunho + c->cursor, strlen(c->rascunho) - c->cursor + 1);
  c->cursor = a;
}

static void send_rascunho(conversation_t* c)
{
  if (!c->rascunho[0] || !c->send) { return; }
  if (c->send(c->rascunho, c->ctx)) {
    c->rascunho[0] = 0;
    c->cursor = 0;
    list_on_start(&c->lista);   // a message nova aparece embaixo: vai ate ela
  }
  system_request_redraw();
}

// ---------------------------------------------------------------------------
// QUEBRA DA BARRA DE ESCRITA, com deslocamentos
// ---------------------------------------------------------------------------
// Diferente of conversa_quebrar_linhas (que devolve textos e engole os
// espacos), esta devolve ONDE cada line comeca no text original: o touch no
// field precisa transformar "x, y" em position of cursor, e o cursor precisa of
// one lugar exato mesmo entre espacos. As lines particionam o text - o
// espaco que sobra no end of uma line pertence a ela.
// ini[k] e o start of line k; ini[n] = end of text. Devolve n (>= 1).
static int break_offsets(lgfx::LovyanGFX& d, const char* s, int width_px, int16_t* ini, int max)
{
  const int len = (int)strlen(s);
  int n = 0;
  int i = 0;
  ini[n++] = 0;
  while (i < len && n < max) {
    // Cresce a line palavra a palavra; 'bom' = end of ultima que coube.
    int bom = i;
    int j = i;
    for (;;) {
      int end = j;
      while (s[end] && s[end] != ' ') { ++end; }
      int prox = end;
      while (s[prox] == ' ') { ++prox; }
      if (end == j && !s[end]) { j = len; break; }
      char teste[CONVERSA_LINHA_BYTES];
      const int size = end - i;
      const bool cabe_bytes = size < CONVERSA_LINHA_BYTES;
      bool cabe = false;
      if (cabe_bytes) {
        memcpy(teste, s + i, size); teste[size] = 0;
        cabe = (int)d.textWidth(teste) <= width_px;
      }
      if (cabe) { bom = prox; j = prox; if (!s[prox]) { break; } }
      else { break; }
    }
    if (bom == i) {
      // Nem a primeira palavra coube: corta por caractere.
      int e = i;
      char teste[CONVERSA_LINHA_BYTES];
      for (;;) {
        const int nx = utf8_seguinte(s, e);
        const int size = nx - i;
        if (nx == e || size >= CONVERSA_LINHA_BYTES) { break; }
        memcpy(teste, s + i, size); teste[size] = 0;
        if ((int)d.textWidth(teste) > width_px && e > i) { break; }
        e = nx;
        if (!s[e]) { break; }
      }
      if (e == i) { e = utf8_seguinte(s, i); }   // nunca trava
      bom = e;
    }
    i = bom;
    if (i < len) { ini[n++] = (int16_t)i; }
  }
  ini[n] = (int16_t)len;
  return n;
}

// ---------------------------------------------------------------------------
// DESENHO
// ---------------------------------------------------------------------------
// BOTAO DE ENVIAR: circulo com o aviao of papel (seta para a right com o
// entalhe atras), como nos mensageiros of celular. Aceso so com rascunho: sem
// text nao ha o que send, e o botao apagado diz isso antes of touch.
// Desenho por primitivas - a fonte of sistema nao tem o glifo.
static void botao_send(lgfx::LovyanGFX& d, int x, int y, int l, int a, uint16_t color,
                         bool aceso, uint16_t fundo)
{
  tab5_screen_clear(x, y, l, a, fundo);
  const int raio = (l < a ? l : a) / 2;
  const int cx = x + l / 2, cy = y + a / 2;
  d.fillCircle(cx, cy, raio, aceso ? color : d.color565(60, 64, 72));
  const uint16_t tinta = aceso ? d.color565(44, 45, 60) : d.color565(120, 126, 138);
  // Proporcoes em fracao of raio; a ponta puxa o drawing 1/12 para a right
  // para ele parecer centrado (o peso visual stays atras, no entalhe).
  const int ox = cx + raio / 12;
  const int px = ox + raio * 11 / 20;               // ponta
  const int tx = ox - raio * 10 / 20;               // cauda (as duas asas)
  const int ty = raio * 10 / 20;                    // meia height das asas
  const int nx = ox - raio * 4 / 20;                // fundo of entalhe
  d.fillTriangle(px, cy, tx, cy - ty, nx, cy, tinta);
  d.fillTriangle(px, cy, nx, cy, tx, cy + ty, tinta);
}

void conversation_draw(conversation_t* c, lgfx::LovyanGFX& d, const area_t& r,
                       bool complete, bool pode_write, const char* reason)
{
  // O keyboard fisico pode ser encaixado com este ja open. Deixar os dois
  // conserva uma window pequena sem necessidade e, pior, mantem a faixa of
  // navigation presa on keyboard virtual. O rascunho e o cursor continuam.
  if (tab5_keyboard_present() && virtual_keyboard_is_open(&c->kb)) {
    virtual_keyboard_close(&c->kb);
  }
  c->ret = r;
  c->pode_write = pode_write;
  if (!pode_write) { close_keyboard(c); }

  const int base = r.y + r.height - ESCRITA_A - (virtual_keyboard_is_open(&c->kb) ? virtual_keyboard_height(area_tec(c)) : 0);
  const uint16_t FUNDO = fundo(d);
  if (complete) { tab5_screen_clear(r.x, r.y, r.width, base - r.y, FUNDO); }

  if (c->medir || r.width != c->larg) {
    if (medir(c, d, r.width)) { list_items_changed(&c->lista, c->n); }
  }

  const area_t window = { r.x, r.y, r.width, base - r.y };
  list_draw(&c->lista, d, window, FUNDO, complete);
  if (c->n == 0 && complete) {
    tab5_text(d, 3.0f);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_DARKGREY, FUNDO);
    d.drawString("nenhuma mensagem ainda", r.x + r.width / 2, r.y + (base - r.y) / 2);
    d.setTextDatum(top_left);
  }

  // ---- barra of escrita: so repinta o que changed ----------------------------
  const char* fonte = !pode_write ? (reason ? reason : "")
                    : c->rascunho[0] ? c->rascunho
                    // A dica diz a verdade of caso: com keyboard encaixado o touch
                    // nao abre nada (nao rouba meia screen).
                    : (tab5_keyboard_present() ? "digite no teclado; ENTER envia" : "toque para escrever");
  const int field_l = r.width - 24 - ENV_L;
  const bool eh_rascunho = pode_write && c->rascunho[0];
  c->disp = &d;
  cursor_ajustar(c);

  // A QUEBRA SO E REFEITA QUANDO O TEXTO OU A LARGURA MUDAM. Antes cada
  // pass re-quebrava e re-media o rascunho inteiro - e uma pass acontece
  // on lower o finger in key, on release e a cada letra, ou seja, tres vezes
  // por letra digitada.
  const paint_t* p = c->p;
  const bool m_txt  = paint_changed_s(p, c->barra_txt, sizeof(c->barra_txt), fonte);
  const bool m_larg = paint_changed_i(p, &c->barra_larg, field_l);
  tab5_text(d, TAM_ESCRITA);
  if (m_txt || m_larg) {
    c->bar_n = break_offsets(d, fonte, field_l - 24, c->bar_ini, CONVERSA_MAX_LINHAS);
  }
  c->bar_rascunho = eh_rascunho;

  // Linha of cursor e window of DUAS lines que a contains. A window so anda
  // when o cursor exit dela - edit no middle of text nao faz a barra pular.
  int lc = c->bar_n - 1;
  if (eh_rascunho) {
    lc = 0;
    while (lc + 1 < c->bar_n && c->bar_ini[lc + 1] <= c->cursor) { ++lc; }
  }
  int topo = c->bar_topo;
  if (!eh_rascunho) { topo = 0; }
  else if (lc < topo) { topo = lc; }
  else if (lc > topo + 1) { topo = lc - 1; }
  if (topo > c->bar_n - 2) { topo = c->bar_n - 2; }
  if (topo < 0) { topo = 0; }
  c->bar_topo = topo;
  const int nl = (c->bar_n - topo) > 2 ? 2 : (c->bar_n - topo);
  c->bar_nl = nl;

  const bool m1 = paint_changed_i(p, &c->barra_base, base);
  const bool m2 = paint_changed_i(p, &c->barra_modo, (int)pode_write);
  const bool m6 = paint_changed_i(p, &c->barra_cursor, eh_rascunho ? c->cursor : -1);
  const bool m7 = paint_changed_i(p, &c->barra_topo, topo);
  const bool m8 = paint_changed_i(p, &c->barra_env, eh_rascunho);
  if (m1 || m2 || m_txt || m_larg || m6 || m7 || m8 || complete) {
    // O botao of send so e refeito when a barra se mexe ou when o
    // rascunho passa of vazio a cheio (ou o contrario) - nao a cada letra.
    const bool everything = m1 || m2 || complete;
    const bool env  = everything || m8;
    if (everything) { tab5_screen_clear(r.x, base, r.width, ESCRITA_A, FUNDO); }
    else      { tab5_screen_clear(r.x, base, r.width - ENV_L, ESCRITA_A, FUNDO); }
    d.fillRoundRect(r.x + 12, base + 6, field_l, ESCRITA_A - 12, 10, pode_write ? TFT_DARKGREY : FUNDO);
    if (!pode_write) { d.drawRoundRect(r.x + 12, base + 6, field_l, ESCRITA_A - 12, 10, TFT_DARKGREY); }
    tab5_text(d, TAM_ESCRITA);
    d.setTextDatum(top_left);
    d.setTextColor(pode_write ? TFT_WHITE : TFT_DARKGREY, pode_write ? TFT_DARKGREY : FUNDO);
    // Linhas centradas no field pela height REAL of fonte (uma line so stays
    // no middle; duas dividem o field) - o passo fixo antigo deixava a segunda
    // encostada in borda of bottom.
    const int fa = d.fontHeight();
    const int passo = fa + 4;
    const int y0 = base + 6 + (ESCRITA_A - 12 - (nl * passo - 4)) / 2;
    c->bar_x0 = r.x + 24;
    c->bar_y0 = y0;
    c->bar_passo = passo;
    char lin[CONVERSA_LINHA_BYTES];
    for (int k = 0; k < nl; ++k) {
      const int ini = c->bar_ini[topo + k], end = c->bar_ini[topo + k + 1];
      int size = end - ini;
      if (size >= CONVERSA_LINHA_BYTES) { size = CONVERSA_LINHA_BYTES - 1; }
      memcpy(lin, fonte + ini, size); lin[size] = 0;
      d.drawString(lin, r.x + 24, y0 + k * passo);
    }
    if (eh_rascunho) {
      const int k = lc - topo;
      const int ini = c->bar_ini[lc];
      char pre[CONVERSA_LINHA_BYTES];
      int size = c->cursor - ini;
      if (size >= CONVERSA_LINHA_BYTES) { size = CONVERSA_LINHA_BYTES - 1; }
      if (size < 0) { size = 0; }
      memcpy(pre, fonte + ini, size); pre[size] = 0;
      int cx = r.x + 24 + (int)d.textWidth(pre) + 2;
      if (cx > r.x + 12 + field_l - 6) { cx = r.x + 12 + field_l - 6; }
      d.fillRect(cx, y0 + k * passo, 3, fa, TFT_GREENYELLOW);
    }
    // Sem botao of send when nao of para send: one botao que nao funciona e pior que
    // a ausencia dele.
    if (pode_write && env) {
      botao_send(d, r.x + r.width - ENV_L, base + 6, ENV_L - 14, ESCRITA_A - 12,
                   c->color_send ? c->color_send : (uint16_t)TFT_GREENYELLOW, eh_rascunho, FUNDO);
    }
  }

  // ---- keyboard virtual (of sistema: ele sabe o que repintar) ---------------
  virtual_keyboard_position(&c->kb, area_tec(c));
  virtual_keyboard_paint(&c->kb, d, complete);
}

// ---------------------------------------------------------------------------
// TECLAS
// ---------------------------------------------------------------------------
// O keyboard olha o text antes of cursor para armar a maiuscula of start of
// phrase: chamado sempre que o text ou o cursor mudam com ele open.
static void context(conversation_t* c) { virtual_keyboard_context(&c->kb, c->rascunho, c->cursor); }

static void aplicar_key(conversation_t* c, int acao)
{
  if (acao > 0) {
    char u[4];   // codepoint ja com o shift (o keyboard aplica)
    inserir(c, u, (size_t)virtual_keyboard_utf8(acao, u));
    context(c);
    system_request_redraw();
    return;
  }
  if (acao == TECVIRT_APAGAR) { erase_antes(c); context(c); system_request_redraw(); return; }
  if (acao == TECVIRT_ENVIAR) { send_rascunho(c); }
  if (acao == TECVIRT_ENVIAR || acao == TECVIRT_FECHAR) { close_keyboard(c); }
}

// O touch em coordenadas of TELA, e relativo on rectangle (o que o keyboard usa).
static void para_screen(int x, int y, int* xs, int* ys)
{
  const area_t a = system_area();
  *xs = a.x + x;
  *ys = a.y + y;
}

// O keyboard (fingers, return of key, soltura) e of sistema: aqui so se
// aplicam as teclas que ele confirmou nesta sample.
void conversation_touch_level(conversation_t* c, int x, int y, bool pressed)
{
  int actions[8];
  const int n = virtual_keyboard_level(&c->kb, actions, 8);
  for (int i = 0; i < n && virtual_keyboard_is_open(&c->kb); ++i) { aplicar_key(c, actions[i]); }

  // Rolagem dos baloes (a window of lista ja termina acima of barra e of keyboard).
  if (list_drag(&c->lista, x, y, pressed)) { system_request_redraw(); }
}

// TOQUE NO TEXTO: leva o cursor para o caractere mais next of finger, sem
// erase nada. Usa as posicoes of ULTIMA paint of barra (bar_x0/bar_y0/
// bar_passo e as lines visiveis) e mede com a mesma fonte dela.
static void cursor_no_touch(conversation_t* c, int xs, int ys)
{
  if (!c->bar_rascunho || !c->disp || c->bar_nl <= 0 || c->bar_passo <= 0) { return; }
  int k = (ys - c->bar_y0) / c->bar_passo;
  if (ys < c->bar_y0) { k = 0; }
  if (k >= c->bar_nl) { k = c->bar_nl - 1; }
  const int lin = c->bar_topo + k;
  const int ini = c->bar_ini[lin];
  int end = c->bar_ini[lin + 1];
  // Nas lines que nao sao a ultima, o espaco of quebra e of line mas o
  // cursor depois dele seria drawn in line of bottom: para no ultimo
  // caractere visivel.
  if (lin + 1 < c->bar_n) { while (end > ini && c->rascunho[end - 1] == ' ') { --end; } }

  lgfx::LovyanGFX& d = *c->disp;
  tab5_text(d, TAM_ESCRITA);
  const int alvo = xs - c->bar_x0;
  char pre[CONVERSA_LINHA_BYTES];
  int melhor = ini, melhor_dist = 1 << 30;
  for (int i = ini;; i = utf8_seguinte(c->rascunho, i)) {
    int size = i - ini;
    if (size >= CONVERSA_LINHA_BYTES) { break; }
    memcpy(pre, c->rascunho + ini, size); pre[size] = 0;
    const int dist = abs((int)d.textWidth(pre) - alvo);
    if (dist < melhor_dist) { melhor_dist = dist; melhor = i; }
    if (i >= end || !c->rascunho[i]) { break; }
  }
  c->cursor = melhor;
  context(c);
  system_request_redraw();
}

void conversation_touch(conversation_t* c, int x, int y)
{
  int xs, ys;
  para_screen(x, y, &xs, &ys);
  const area_t& r = c->ret;

  // Teclado open: a faixa dele e so dele. O click aqui e so a rede para one
  // touch rapido demais para o level ter visto (senao daria letra dupla).
  if (virtual_keyboard_contains(&c->kb, xs, ys)) {
    const int ac = virtual_keyboard_click(&c->kb, xs, ys);
    if (ac) { aplicar_key(c, ac); }
    return;
  }

  // O end of uma rolagem nao e click em nada.
  if (list_item_touched(&c->lista, x, y, nullptr) == LISTA_ARRASTE) { return; }

  // Barra of escrita: o botao of seta envia; o field abre o keyboard virtual (so sem o
  // fisico encaixado - com ele, nao roubamos meia screen).
  const int base = r.y + r.height - ESCRITA_A - (virtual_keyboard_is_open(&c->kb) ? virtual_keyboard_height(area_tec(c)) : 0);
  if (ys >= base && ys < base + ESCRITA_A && c->pode_write) {
    if (xs >= r.x + r.width - ENV_L) {
      send_rascunho(c);
      close_keyboard(c);
    } else if (virtual_keyboard_is_open(&c->kb)) {
      cursor_no_touch(c, xs, ys);
    } else if (!tab5_keyboard_present()) {
      virtual_keyboard_open(&c->kb);            // trava o INICIO; o finger que abriu nao digita
      cursor_no_touch(c, xs, ys);       // o mesmo touch que abre ja posiciona o cursor
      context(c);                      // rascunho vazio: ja abre com a maiuscula armada
      request_complete(c);
    }
  }
}

// ---------------------------------------------------------------------------
// CURSOR PELO TECLADO FISICO
// ---------------------------------------------------------------------------
// Del: apaga o caractere DEPOIS of cursor (o Backspace apaga o of antes).
static void erase_depois(conversation_t* c)
{
  cursor_ajustar(c);
  if (!c->rascunho[c->cursor]) { return; }
  const int b = utf8_seguinte(c->rascunho, c->cursor);
  memmove(c->rascunho + c->cursor, c->rascunho + b, strlen(c->rascunho) - b + 1);
}

// Ctrl + seta: pula uma palavra (os espacos e depois a palavra, como em
// qualquer editor).
static void palavra(conversation_t* c, bool frente)
{
  const char* r = c->rascunho;
  int i = c->cursor;
  if (frente) {
    while (r[i] == ' ') { i = utf8_seguinte(r, i); }
    while (r[i] && r[i] != ' ') { i = utf8_seguinte(r, i); }
  } else {
    while (i > 0 && r[utf8_anterior(r, i)] == ' ') { i = utf8_anterior(r, i); }
    while (i > 0 && r[utf8_anterior(r, i)] != ' ') { i = utf8_anterior(r, i); }
  }
  c->cursor = i;
}

// Quantos CARACTERES (nao bytes) ha of 'ini' ate 'end'.
static int n_caracteres(const char* s, int ini, int end)
{
  int n = 0;
  for (int i = ini; i < end && s[i]; i = utf8_seguinte(s, i)) { ++n; }
  return n;
}

// Seta para top/bottom: a line of top/bottom of barra, in mesma coluna (em
// caracteres). Usa a quebra of ULTIMA paint (bar_ini) - a barra e repintada
// a cada key, entao ela e a of text of now. Ja in primeira line, top
// vai on start; in ultima, bottom vai on end.
static void line_vizinha(conversation_t* c, bool bottom)
{
  cursor_ajustar(c);
  const int len = (int)strlen(c->rascunho);
  const int n = c->bar_n;
  if (!c->bar_rascunho || n <= 0 || c->bar_ini[n] != len) {   // quebra velha ou ausente
    c->cursor = bottom ? len : 0;
    return;
  }
  int k = 0;
  while (k + 1 < n && c->cursor >= c->bar_ini[k + 1]) { ++k; }
  const int alvo = bottom ? k + 1 : k - 1;
  if (alvo < 0)  { c->cursor = 0;   return; }
  if (alvo >= n) { c->cursor = len; return; }
  const int col = n_caracteres(c->rascunho, c->bar_ini[k], c->cursor);
  int i = c->bar_ini[alvo];
  int end = c->bar_ini[alvo + 1];
  // O espaco of quebra pertence a line mas o cursor depois dele cairia in
  // line of bottom: para no ultimo caractere visivel (como no touch).
  if (alvo + 1 < n) { while (end > i && c->rascunho[end - 1] == ' ') { --end; } }
  for (int k2 = 0; k2 < col && i < end; ++k2) { i = utf8_seguinte(c->rascunho, i); }
  c->cursor = i;
}

void conversation_type(conversation_t* c, const char* text, uint8_t code, uint8_t mod)
{
  if (!c->pode_write) { return; }
  const bool ctrl = (mod & TAB5_KEY_CTRL) != 0;
  switch (code) {
    case TAB5_KEY_ENTER:  send_rascunho(c); close_keyboard(c); return;
    case TAB5_KEY_BACKSPACE: erase_antes(c);  system_request_redraw(); return;
    case TAB5_KEY_DELETE:    erase_depois(c); system_request_redraw(); return;
    // Esc limpa o rascunho inteiro (o cursor iteration on comeco).
    case TAB5_KEY_ESC:    c->rascunho[0] = 0; c->cursor = 0; system_request_redraw(); return;
    case TAB5_KEY_LEFT:
      cursor_ajustar(c);
      if (ctrl) { palavra(c, false); } else { c->cursor = utf8_anterior(c->rascunho, c->cursor); }
      system_request_redraw();
      return;
    case TAB5_KEY_RIGHT:
      cursor_ajustar(c);
      if (ctrl) { palavra(c, true); } else { c->cursor = utf8_seguinte(c->rascunho, c->cursor); }
      system_request_redraw();
      return;
    // Ctrl + top/bottom: comeco/end of text of uma vez.
    case TAB5_KEY_UP:
      if (ctrl) { c->cursor = 0; } else { line_vizinha(c, false); }
      system_request_redraw();
      return;
    case TAB5_KEY_DOWN:
      if (ctrl) { c->cursor = (int)strlen(c->rascunho); } else { line_vizinha(c, true); }
      system_request_redraw();
      return;
    case TAB5_KEY_TAB:    return;   // nada a fazer numa message
    default: break;
  }
  // Com Ctrl ou Alt e atalho, nao text: one "Ctrl+c" nao pode virar one "c".
  if (mod & (TAB5_KEY_CTRL | TAB5_KEY_ALT)) { return; }
  // Controle nao vira caractere. Uma sequencia acentuada vem com mais of one
  // byte e entra inteira (ou nao entra) - inserir() nunca corta no middle dela.
  char limpo[16];
  size_t n = 0;
  for (const char* ch = text; ch && *ch && n < sizeof(limpo) - 1; ++ch) {
    if ((unsigned char)*ch < 0x20) { continue; }
    limpo[n++] = *ch;
  }
  limpo[n] = 0;
  inserir(c, limpo, n);
  system_request_redraw();
}
