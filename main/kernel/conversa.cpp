#include "conversa.hpp"
#include "teclado_virtual.hpp"
#include "tab5_ui.hpp"
#include "tab5_tela.hpp"
#include "tab5_teclado.hpp"
#include "tab5_hora.hpp"
#include "tab5_emoji.hpp"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

// ---------------------------------------------------------------------------
// MEDIDAS - as do app Mesh, calibradas em hardware ao longo de setembro (ver
// os avisos de TAM_MSG/TAM_ESCRITA/LINHA_STEP em app_mesh/app_mesh.cpp). Os
// STEP sao pixels fixos que acompanham o tamanho da fonte: mexeu num, mexa no
// outro ("altura do glifo (tamanho*8) + ~4px de respiro").
// ---------------------------------------------------------------------------
static const float TAM_MSG     = 3.8f;
static const float TAM_NOME    = 2.415f;
static const float TAM_META    = 1.955f;
static const float TAM_ESCRITA = 3.2f;
static const int   LINHA_STEP  = 34;    // acompanha TAM_MSG
static const int   DIVISOR_A   = 40;
static const int   VAO_BALAO   = 8;
// A HORA EMBAIXO DO BALAO ("14:32"), do lado de quem falou. So quando a
// mensagem tem data de verdade - sem ela (relogio nao acertado) nao ha o que
// dizer, e a faixa nem existe.
static const float TAM_HORA    = 1.7f;
static const int   HORA_A      = 22;
static const int   ESCRITA_A   = 80;
static const int   ENV_L       = 110;   // botao de enviar + margem
static const int   MARCA_L     = 26;    // marca de envio, a esquerda do balao
static const int   CAP_MAX     = 256;

struct conversa_balao_s {
  uint32_t assin;               // texto+nome+meta+lado+largura da ultima medida (ver medir)
  int16_t  bl, ba, altura, nome_a, meta_a;
  uint16_t cor_nome, cor_meta;
  bool     minha, inicio_dia;
  uint8_t  n_linhas, envio;
  uint32_t quando;
  char     nome[16];
  char     meta[24];
  char     linhas[CONVERSA_MAX_LINHAS][CONVERSA_LINHA_BYTES];
};
typedef struct conversa_balao_s balao_t;

static uint16_t fundo(lgfx::LovyanGFX& d)            { return d.color565(13, 17, 23); }
static uint16_t cor_balao(lgfx::LovyanGFX& d, bool m) { return m ? d.color565(22, 33, 26) : d.color565(24, 28, 36); }

// ---------------------------------------------------------------------------
// QUEBRA DE LINHAS
// ---------------------------------------------------------------------------
int conversa_quebrar_linhas(lgfx::LovyanGFX& d, const char* txt, int largura_px,
                            char linhas[][CONVERSA_LINHA_BYTES], int max_linhas)
{
  int n = 0;
  char atual[CONVERSA_LINHA_BYTES] = "";
  const char* p = txt;
  while (*p && n < max_linhas) {
    const char* fim = p;
    while (*fim && *fim != ' ') { ++fim; }
    size_t tam = (size_t)(fim - p);
    // Palavra maior que o buffer de uma linha: entra so o que couber (sem
    // partir um caractere UTF-8), e o resto volta pelo loop como "a proxima
    // palavra" - nada e descartado.
    const bool cortada = tam >= CONVERSA_LINHA_BYTES;
    if (cortada) {
      tam = CONVERSA_LINHA_BYTES - 1;
      while (tam > 0 && ((unsigned char)p[tam] & 0xC0) == 0x80) { --tam; }
    }
    char palavra[CONVERSA_LINHA_BYTES];
    memcpy(palavra, p, tam);
    palavra[tam] = 0;

    // O LIMITE E A LARGURA, NUNCA O BUFFER. Antes o teste montava "atual +
    // palavra" num buffer do tamanho de uma linha: quando os BYTES acabavam
    // antes dos pixels (acento e emoji ocupam 2-4 bytes), o snprintf cortava,
    // o texto cortado "cabia", e o fim da palavra sumia - a linha terminava no
    // meio da tela e o resto nao aparecia em lugar nenhum. Agora o que nao
    // cabe no buffer simplesmente vai para a linha de baixo.
    const size_t usado = strlen(atual);
    const bool cabe_bytes = usado + (usado ? 1 : 0) + tam < CONVERSA_LINHA_BYTES;
    char teste[CONVERSA_LINHA_BYTES];
    if (cabe_bytes) {
      if (usado) { snprintf(teste, sizeof(teste), "%s %s", atual, palavra); }
      else       { snprintf(teste, sizeof(teste), "%s", palavra); }
    }

    if (!cortada && cabe_bytes && d.textWidth(teste) <= largura_px) {
      memcpy(atual, teste, strlen(teste) + 1);
      p = fim;
      while (*p == ' ') { ++p; }
    } else if (!atual[0]) {
      // Linha vazia e nem assim coube: corta por caractere (nunca no meio de
      // um UTF-8) e avanca SO o que entrou - o resto da palavra segue adiante.
      memcpy(atual, palavra, tam + 1);
      tab5_cortar_ao_caber(d, atual, largura_px);
      size_t consumido = strlen(atual);
      if (consumido == 0) {   // nem 1 caractere coube: avanca 1 mesmo assim (nunca trava)
        const unsigned char c0 = (unsigned char)palavra[0];
        consumido = ((c0 & 0xE0) == 0xC0) ? 2 : ((c0 & 0xF0) == 0xE0) ? 3 : ((c0 & 0xF8) == 0xF0) ? 4 : 1;
      }
      p += consumido;
    } else {
      snprintf(linhas[n++], CONVERSA_LINHA_BYTES, "%s", atual);
      atual[0] = 0;
    }
  }
  if (atual[0] && n < max_linhas) { snprintf(linhas[n++], CONVERSA_LINHA_BYTES, "%s", atual); }
  return n;
}

// ---------------------------------------------------------------------------
// DIVISOR DE DIA - "HOJE", "ONTEM", "DD/MM" (no fuso local)
// ---------------------------------------------------------------------------
static int dia_local(uint32_t epoch)
{
  struct tm t;
  tab5_hora_local(epoch, &t);
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

static void formatar_dia(uint32_t epoch, char* out, size_t tam)
{
  if (const uint32_t agora = tab5_hora_epoch()) {
    if (dia_local(epoch) == dia_local(agora))          { snprintf(out, tam, "HOJE");  return; }
    if (dia_local(epoch) == dia_local(agora - 86400u)) { snprintf(out, tam, "ONTEM"); return; }
  }
  struct tm t;
  tab5_hora_local(epoch, &t);
  snprintf(out, tam, "%02d/%02d", t.tm_mday, t.tm_mon + 1);
}

static void desenhar_divisor(lgfx::LovyanGFX& d, int x0, int larg, int y, uint32_t epoch)
{
  char txt[16];
  formatar_dia(epoch, txt, sizeof(txt));
  tab5_texto(d, 1.7f);
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

// Marca de envio, ao lado do balao: tres pontos (saindo), visto (foi), X (falhou).
static void marca_envio(lgfx::LovyanGFX& d, int cx, int cy, uint8_t envio)
{
  d.fillRect(cx - MARCA_L / 2, cy - 12, MARCA_L, 24, fundo(d));
  if (envio == CONVERSA_ENVIO_PENDENTE) {
    for (int k = -1; k <= 1; ++k) { d.fillCircle(cx + k * 7, cy, 2, TFT_DARKGREY); }
  } else if (envio == CONVERSA_ENVIO_OK) {
    d.drawLine(cx - 8, cy, cx - 3, cy + 6, TFT_GREENYELLOW);
    d.drawLine(cx - 8, cy + 1, cx - 3, cy + 7, TFT_GREENYELLOW);
    d.drawLine(cx - 3, cy + 6, cx + 8, cy - 7, TFT_GREENYELLOW);
    d.drawLine(cx - 3, cy + 7, cx + 8, cy - 6, TFT_GREENYELLOW);
  } else if (envio == CONVERSA_ENVIO_OUVIDO) {
    // O mesmo visto, em cinza: saiu e a rede ouviu, mas quem devia
    // confirmar (o destinatario) ainda nao confirmou.
    d.drawLine(cx - 8, cy, cx - 3, cy + 6, TFT_DARKGREY);
    d.drawLine(cx - 8, cy + 1, cx - 3, cy + 7, TFT_DARKGREY);
    d.drawLine(cx - 3, cy + 6, cx + 8, cy - 7, TFT_DARKGREY);
    d.drawLine(cx - 3, cy + 7, cx + 8, cy - 6, TFT_DARKGREY);
  } else if (envio == CONVERSA_ENVIO_SEM_RESPOSTA) {
    // Cinza, circulo e "?": nao e falha, e nao saber.
    d.drawCircle(cx, cy, 9, TFT_DARKGREY);
    d.drawCircle(cx, cy, 8, TFT_DARKGREY);
    const auto e = d.getTextStyle();
    d.setFont(tab5_fonte_suave(true, 16));
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_DARKGREY);
    d.drawString("?", cx, cy + 1);
    d.setTextStyle(e);
    tab5_fonte_corpo(d);
  } else if (envio == CONVERSA_ENVIO_FALHOU) {
    d.drawLine(cx - 6, cy - 6, cx + 6, cy + 6, TFT_RED);
    d.drawLine(cx - 6, cy + 6, cx + 6, cy - 6, TFT_RED);
    d.drawLine(cx - 5, cy - 6, cx + 7, cy + 6, TFT_RED);
    d.drawLine(cx - 5, cy + 6, cx + 7, cy - 6, TFT_RED);
  }
}

// ---------------------------------------------------------------------------
// MEDIR - uma vez por mudanca, nunca por frame
// ---------------------------------------------------------------------------
// So o que mudou e re-medido. Mensagem chegando ou confirmacao de entrega
// (que so troca a marca) re-media TODOS os baloes - ate 256, quebrando linha
// palavra a palavra - na tarefa de interface: se caia no meio da digitacao,
// aquele quadro atrasava e a tecla engasgava. Cada balao guarda a assinatura
// do que o define (texto, nome, meta, lado, largura); igual, so os campos
// baratos (marca, data) sao atualizados. Indice deslocado (historico que
// rolou) da assinatura diferente e e re-medido - nunca reaproveita errado.
//
// Devolve true quando a ESTRUTURA mudou (quantidade ou altura de algum balao):
// ai a lista inteira se reorganiza. Senao, so os baloes que mudaram (a marca
// de envio, tipicamente) sao anotados para repintar.
static int hora_a(const balao_t* b) { return tab5_hora_valida(b->quando) ? HORA_A : 0; }

static uint32_t assinatura(const conversa_msg_t* m, int larg)
{
  uint32_t h = 2166136261u;   // FNV-1a
  auto mais = [&](const char* s) { for (; *s; ++s) { h = (h ^ (uint8_t)*s) * 16777619u; } h = (h ^ 0xFF) * 16777619u; };
  mais(m->texto); mais(m->nome); mais(m->meta);
  h = (h ^ (uint32_t)m->minha) * 16777619u;
  h = (h ^ (uint32_t)larg) * 16777619u;
  return h | 1u;   // 0 = nunca medido
}

static bool medir(conversa_t* c, lgfx::LovyanGFX& d, int larg)
{
  c->larg = larg;
  c->medir = false;
  const int n_antes = c->n;
  int n = c->contar ? c->contar(c->ctx) : 0;
  if (n > CAP_MAX) { n = CAP_MAX; }
  if (n > c->cap) {
    const int nova = ((n + 31) / 32) * 32;
    void* p = heap_caps_realloc(c->bal, sizeof(balao_t) * nova, MALLOC_CAP_SPIRAM);
    if (!p) { c->n = 0; return true; }
    c->bal = (balao_t*)p;
    c->cap = nova;
  }
  c->n = n;

  const auto estilo = d.getTextStyle();
  tab5_texto(d, TAM_NOME);
  const int alt_nome = d.fontHeight();
  tab5_texto(d, TAM_META);
  const int alt_meta = d.fontHeight();
  const int respiro = LINHA_STEP / 4;

  bool estrutura = (n != n_antes);
  int dia_ant = 0;
  static conversa_msg_t m;   // 300+ bytes: fora da stack da interface
  for (int i = 0; i < n; ++i) {
    balao_t* b = &c->bal[i];
    memset(&m, 0, sizeof(m));
    if (!c->ler(i, &m, c->ctx)) { m.texto[0] = 0; }
    const int16_t alt_antes = (i < n_antes) ? b->altura : -1;
    if (i < n_antes && (b->envio != m.envio || b->quando != m.quando
                    || b->cor_nome != m.cor_nome || b->cor_meta != m.cor_meta)) { lista_item_mudou(&c->lista, i); }
    b->envio = m.envio;
    b->quando = m.quando;
    b->cor_nome = m.cor_nome;
    b->cor_meta = m.cor_meta;
    const uint32_t as = assinatura(&m, larg);
    if (i < n_antes && b->assin == as) {
      // Mesmo conteudo: medidas valem. So o divisor de dia, que depende do vizinho.
      b->inicio_dia = false;
      if (tab5_hora_valida(b->quando)) {
        const int dia = dia_local(b->quando);
        b->inicio_dia = (dia != dia_ant);
        dia_ant = dia;
      }
      b->altura = (int16_t)((b->inicio_dia ? DIVISOR_A : 0) + b->ba + hora_a(b) + VAO_BALAO);
      if (b->altura != alt_antes) { estrutura = true; }
      continue;
    }
    if (i < n_antes) { lista_item_mudou(&c->lista, i); }
    b->assin = as;
    b->minha = m.minha;
    snprintf(b->nome, sizeof(b->nome), "%s", m.nome);
    snprintf(b->meta, sizeof(b->meta), "%s", m.meta);

    // Teto de largura: 38% a minha (ao lado da marca), 60% a recebida.
    const int pct = (int)(larg * (b->minha ? 0.38f : 0.60f));
    const int teto = (pct < larg - 28 - MARCA_L) ? pct : (larg - 28 - MARCA_L);

    char txt[CONVERSA_TEXTO];
    tab5_emoji_preparar(m.texto, txt, sizeof(txt));
    tab5_texto(d, TAM_MSG);
    b->n_linhas = (uint8_t)conversa_quebrar_linhas(d, txt, teto - 24, b->linhas, CONVERSA_MAX_LINHAS);

    int bl = 24;
    for (int k = 0; k < b->n_linhas; ++k) {
      const int lw = (int)d.textWidth(b->linhas[k]) + 24;
      if (lw > bl) { bl = lw; }
    }
    if (b->nome[0]) {
      tab5_texto(d, TAM_NOME);
      const int lw = (int)d.textWidth(b->nome) + 24;
      if (lw > bl) { bl = lw; }
    }
    if (b->meta[0]) {
      tab5_texto(d, TAM_META);
      const int lw = (int)d.textWidth(b->meta) + 24;
      if (lw > bl) { bl = lw; }
    }
    if (bl > teto) { bl = teto; }

    b->nome_a = b->nome[0] ? (int16_t)(alt_nome + respiro) : 0;
    b->meta_a = b->meta[0] ? (int16_t)(alt_meta + respiro) : 0;
    b->bl = (int16_t)bl;
    b->ba = (int16_t)(24 + b->n_linhas * LINHA_STEP + b->nome_a + b->meta_a);

    // Divisor antes da primeira mensagem de cada dia. Sem data (relogio nao
    // acertado quando ela chegou) nao abre dia - senao virava 31/12/1969.
    b->inicio_dia = false;
    if (tab5_hora_valida(b->quando)) {
      const int dia = dia_local(b->quando);
      b->inicio_dia = (dia != dia_ant);
      dia_ant = dia;
    }
    b->altura = (int16_t)((b->inicio_dia ? DIVISOR_A : 0) + b->ba + hora_a(b) + VAO_BALAO);
    if (b->altura != alt_antes) { estrutura = true; }
  }
  d.setTextStyle(estilo);
  return estrutura;
}

static int balao_altura(int i, void* ctx)
{
  const conversa_t* c = (const conversa_t*)ctx;
  return (c->bal && i < c->n) ? c->bal[i].altura : 0;
}

static void balao_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int larg, void* ctx)
{
  const conversa_t* c = (const conversa_t*)ctx;
  if (!c->bal || i >= c->n) { return; }
  const balao_t* b = &c->bal[i];
  const auto estilo = d.getTextStyle();

  if (b->inicio_dia) { desenhar_divisor(d, x0, larg, y, b->quando); y += DIVISOR_A; }

  const int bx = b->minha ? (x0 + larg - b->bl - 16) : (x0 + 16);
  const uint16_t cor = cor_balao(d, b->minha);
  d.fillRoundRect(bx, y, b->bl, b->ba, 10, cor);

  // Recorte no proprio balao (dentro do da faixa): um emoji no fim da linha
  // nao pinta o vao ate o proximo.
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
    if (b->nome[0]) {
      tab5_texto(d, TAM_NOME);
      d.setTextColor(b->cor_nome ? b->cor_nome : (uint16_t)TFT_SKYBLUE, cor);
      d.drawString(b->nome, bx + 12, cur);
      cur += b->nome_a;
    }
    tab5_texto(d, TAM_MSG);
    d.setTextColor(TFT_WHITE, cor);
    for (int k = 0; k < b->n_linhas; ++k) { d.drawString(b->linhas[k], bx + 12, cur + k * LINHA_STEP); }
    cur += b->n_linhas * LINHA_STEP + LINHA_STEP / 4;
    if (b->meta[0]) {
      tab5_texto(d, TAM_META);
      d.setTextColor(b->cor_meta ? b->cor_meta : (uint16_t)TFT_DARKGREY, cor);
      d.drawString(b->meta, bx + 12, cur);
    }
    d.setClipRect(cx, cy, cl, ca);
  }
  if (b->minha) { marca_envio(d, bx - 8 - MARCA_L / 2, y + b->ba / 2, b->envio); }

  if (hora_a(b)) {
    struct tm t;
    tab5_hora_local(b->quando, &t);
    char hora[8];
    snprintf(hora, sizeof(hora), "%02d:%02d", t.tm_hour, t.tm_min);
    const int hy = y + b->ba;
    d.fillRect(x0, hy, larg, HORA_A, fundo(d));
    tab5_texto(d, TAM_HORA);
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
void conversa_abrir(conversa_t* c, pintura_t* p, conversa_contar_t contar, conversa_ler_t ler,
                    conversa_enviar_t enviar, void* ctx)
{
  balao_t* bal = c->bal;   // o cache sobrevive a reabrir (mesmo app)
  const int cap = c->cap;
  memset(c, 0, sizeof(*c));
  c->bal = bal; c->cap = cap;
  c->contar = contar; c->ler = ler; c->enviar = enviar; c->ctx = ctx; c->p = p;
  c->medir = true;
  tecvirt_frase(&c->kb, true);   // texto corrido: maiuscula no inicio de frase
  c->barra_base = c->barra_modo = c->barra_larg = c->barra_cursor = c->barra_topo = c->barra_env = -1;
  lista_definir(&c->lista, ROLAGEM_FIM, 0, balao_altura, balao_desenhar, c);
}

void conversa_fechar(conversa_t* c)
{
  if (c->bal) { heap_caps_free(c->bal); }
  c->bal = nullptr; c->cap = 0; c->n = 0;
  tecvirt_fechar(&c->kb);
}

void conversa_mudou(conversa_t* c) { c->medir = true; sistema_pedir_redesenho(); }

bool conversa_teclado_aberto(const conversa_t* c) { return tecvirt_aberto(&c->kb); }

void conversa_area_teclado(conversa_t* c, const area_t& ka) { c->tec = ka; }

// O retangulo do teclado: o que o app deu, ou o da propria conversa.
static const area_t& area_tec(const conversa_t* c) { return c->tec.largura > 0 ? c->tec : c->ret; }

// Reposiciona tudo (teclado abriu/fechou): passada completa, pelas duas vias
// que a regra da repintura exige (pintura do app + agenda do sistema).
static void pedir_completo(conversa_t* c)
{
  if (c->p) { pintura_pedir(c->p); }
  sistema_pedir_redesenho_completo();
}

static void fechar_teclado(conversa_t* c)
{
  if (!tecvirt_aberto(&c->kb)) { return; }
  tecvirt_fechar(&c->kb);
  pedir_completo(c);
}

void conversa_fechar_teclado(conversa_t* c) { fechar_teclado(c); }

void conversa_trocar(conversa_t* c)
{
  fechar_teclado(c);
  c->medir = true;
  lista_ao_inicio(&c->lista);
  pedir_completo(c);
}

// ---------------------------------------------------------------------------
// EDICAO DO RASCUNHO - sempre no cursor, sempre em fronteira de caractere UTF-8
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

static void cursor_ajustar(conversa_t* c)
{
  const int n = (int)strlen(c->rascunho);
  if (c->cursor > n) { c->cursor = n; }
  if (c->cursor < 0) { c->cursor = 0; }
}

static void inserir(conversa_t* c, const char* t, size_t n)
{
  cursor_ajustar(c);
  const size_t len = strlen(c->rascunho);
  const size_t pedido = n;   // 't' tem EXATAMENTE n bytes - nao e uma string terminada
  size_t teto = sizeof(c->rascunho) - 1;
  if (c->limite > 0 && (size_t)c->limite < teto) { teto = (size_t)c->limite; }
  if (len >= teto) { return; }
  if (len + n > teto) { n = teto - len; }
  // Nunca corta no meio de um caractere: recua ate a fronteira. So olha
  // dentro dos 'pedido' bytes recebidos. Antes isto era strlen(t) - e a
  // tecla do teclado virtual chega como UM char solto na pilha, sem o zero
  // no fim: strlen lia o lixo depois dele, e quando o byte seguinte calhava
  // de parecer continuacao de UTF-8 (0x80-0xBF) a letra era descartada.
  // Dependia do que tinha sobrado na pilha - por isso "do nada".
  while (n > 0 && n < pedido && ((unsigned char)t[n] & 0xC0) == 0x80) { --n; }
  if (!n) { return; }
  memmove(c->rascunho + c->cursor + n, c->rascunho + c->cursor, len - c->cursor + 1);
  memcpy(c->rascunho + c->cursor, t, n);
  c->cursor += (int)n;
}

static void apagar_antes(conversa_t* c)
{
  cursor_ajustar(c);
  if (!c->cursor) { return; }
  const int a = utf8_anterior(c->rascunho, c->cursor);
  memmove(c->rascunho + a, c->rascunho + c->cursor, strlen(c->rascunho) - c->cursor + 1);
  c->cursor = a;
}

static void enviar_rascunho(conversa_t* c)
{
  if (!c->rascunho[0] || !c->enviar) { return; }
  if (c->enviar(c->rascunho, c->ctx)) {
    c->rascunho[0] = 0;
    c->cursor = 0;
    lista_ao_inicio(&c->lista);   // a mensagem nova aparece embaixo: vai ate ela
  }
  sistema_pedir_redesenho();
}

// ---------------------------------------------------------------------------
// QUEBRA DA BARRA DE ESCRITA, com deslocamentos
// ---------------------------------------------------------------------------
// Diferente de conversa_quebrar_linhas (que devolve textos e engole os
// espacos), esta devolve ONDE cada linha comeca no texto original: o toque no
// campo precisa transformar "x, y" em posicao de cursor, e o cursor precisa de
// um lugar exato mesmo entre espacos. As linhas particionam o texto - o
// espaco que sobra no fim de uma linha pertence a ela.
// ini[k] e o inicio da linha k; ini[n] = fim do texto. Devolve n (>= 1).
static int quebrar_offsets(lgfx::LovyanGFX& d, const char* s, int largura_px, int16_t* ini, int max)
{
  const int len = (int)strlen(s);
  int n = 0;
  int i = 0;
  ini[n++] = 0;
  while (i < len && n < max) {
    // Cresce a linha palavra a palavra; 'bom' = fim da ultima que coube.
    int bom = i;
    int j = i;
    for (;;) {
      int fim = j;
      while (s[fim] && s[fim] != ' ') { ++fim; }
      int prox = fim;
      while (s[prox] == ' ') { ++prox; }
      if (fim == j && !s[fim]) { j = len; break; }
      char teste[CONVERSA_LINHA_BYTES];
      const int tam = fim - i;
      const bool cabe_bytes = tam < CONVERSA_LINHA_BYTES;
      bool cabe = false;
      if (cabe_bytes) {
        memcpy(teste, s + i, tam); teste[tam] = 0;
        cabe = (int)d.textWidth(teste) <= largura_px;
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
        const int tam = nx - i;
        if (nx == e || tam >= CONVERSA_LINHA_BYTES) { break; }
        memcpy(teste, s + i, tam); teste[tam] = 0;
        if ((int)d.textWidth(teste) > largura_px && e > i) { break; }
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
// BOTAO DE ENVIAR: circulo com o aviao de papel (seta para a direita com o
// entalhe atras), como nos mensageiros de celular. Aceso so com rascunho: sem
// texto nao ha o que enviar, e o botao apagado diz isso antes do toque.
// Desenho por primitivas - a fonte do sistema nao tem o glifo.
static void botao_enviar(lgfx::LovyanGFX& d, int x, int y, int l, int a, uint16_t cor,
                         bool aceso, uint16_t fundo)
{
  tab5_tela_limpar(x, y, l, a, fundo);
  const int raio = (l < a ? l : a) / 2;
  const int cx = x + l / 2, cy = y + a / 2;
  d.fillCircle(cx, cy, raio, aceso ? cor : d.color565(60, 64, 72));
  const uint16_t tinta = aceso ? d.color565(44, 45, 60) : d.color565(120, 126, 138);
  // Proporcoes em fracao do raio; a ponta puxa o desenho 1/12 para a direita
  // para ele parecer centrado (o peso visual fica atras, no entalhe).
  const int ox = cx + raio / 12;
  const int px = ox + raio * 11 / 20;               // ponta
  const int tx = ox - raio * 10 / 20;               // cauda (as duas asas)
  const int ty = raio * 10 / 20;                    // meia altura das asas
  const int nx = ox - raio * 4 / 20;                // fundo do entalhe
  d.fillTriangle(px, cy, tx, cy - ty, nx, cy, tinta);
  d.fillTriangle(px, cy, nx, cy, tx, cy + ty, tinta);
}

void conversa_desenhar(conversa_t* c, lgfx::LovyanGFX& d, const area_t& r,
                       bool completo, bool pode_escrever, const char* motivo)
{
  // O teclado fisico pode ser encaixado com este ja aberto. Deixar os dois
  // conserva uma janela pequena sem necessidade e, pior, mantem a faixa de
  // navegacao presa ao teclado virtual. O rascunho e o cursor continuam.
  if (tab5_teclado_presente() && tecvirt_aberto(&c->kb)) {
    tecvirt_fechar(&c->kb);
  }
  c->ret = r;
  c->pode_escrever = pode_escrever;
  if (!pode_escrever) { fechar_teclado(c); }

  const int base = r.y + r.altura - ESCRITA_A - (tecvirt_aberto(&c->kb) ? tecvirt_altura(area_tec(c)) : 0);
  const uint16_t FUNDO = fundo(d);
  if (completo) { tab5_tela_limpar(r.x, r.y, r.largura, base - r.y, FUNDO); }

  if (c->medir || r.largura != c->larg) {
    if (medir(c, d, r.largura)) { lista_itens_mudaram(&c->lista, c->n); }
  }

  const area_t janela = { r.x, r.y, r.largura, base - r.y };
  lista_desenhar(&c->lista, d, janela, FUNDO, completo);
  if (c->n == 0 && completo) {
    tab5_texto(d, 3.0f);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_DARKGREY, FUNDO);
    d.drawString("nenhuma mensagem ainda", r.x + r.largura / 2, r.y + (base - r.y) / 2);
    d.setTextDatum(top_left);
  }

  // ---- barra de escrita: so repinta o que mudou ----------------------------
  const char* fonte = !pode_escrever ? (motivo ? motivo : "")
                    : c->rascunho[0] ? c->rascunho
                    // A dica diz a verdade do caso: com teclado encaixado o toque
                    // nao abre nada (nao rouba meia tela).
                    : (tab5_teclado_presente() ? "digite no teclado; ENTER envia" : "toque para escrever");
  const int campo_l = r.largura - 24 - ENV_L;
  const bool eh_rascunho = pode_escrever && c->rascunho[0];
  c->disp = &d;
  cursor_ajustar(c);

  // A QUEBRA SO E REFEITA QUANDO O TEXTO OU A LARGURA MUDAM. Antes cada
  // passada re-quebrava e re-media o rascunho inteiro - e uma passada acontece
  // ao descer o dedo na tecla, ao soltar e a cada letra, ou seja, tres vezes
  // por letra digitada.
  const pintura_t* p = c->p;
  const bool m_txt  = pintura_mudou_s(p, c->barra_txt, sizeof(c->barra_txt), fonte);
  const bool m_larg = pintura_mudou_i(p, &c->barra_larg, campo_l);
  tab5_texto(d, TAM_ESCRITA);
  if (m_txt || m_larg) {
    c->bar_n = quebrar_offsets(d, fonte, campo_l - 24, c->bar_ini, CONVERSA_MAX_LINHAS);
  }
  c->bar_rascunho = eh_rascunho;

  // Linha do cursor e janela de DUAS linhas que a contem. A janela so anda
  // quando o cursor sai dela - editar no meio do texto nao faz a barra pular.
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

  const bool m1 = pintura_mudou_i(p, &c->barra_base, base);
  const bool m2 = pintura_mudou_i(p, &c->barra_modo, (int)pode_escrever);
  const bool m6 = pintura_mudou_i(p, &c->barra_cursor, eh_rascunho ? c->cursor : -1);
  const bool m7 = pintura_mudou_i(p, &c->barra_topo, topo);
  const bool m8 = pintura_mudou_i(p, &c->barra_env, eh_rascunho);
  if (m1 || m2 || m_txt || m_larg || m6 || m7 || m8 || completo) {
    // O botao de enviar so e refeito quando a barra se mexe ou quando o
    // rascunho passa de vazio a cheio (ou o contrario) - nao a cada letra.
    const bool tudo = m1 || m2 || completo;
    const bool env  = tudo || m8;
    if (tudo) { tab5_tela_limpar(r.x, base, r.largura, ESCRITA_A, FUNDO); }
    else      { tab5_tela_limpar(r.x, base, r.largura - ENV_L, ESCRITA_A, FUNDO); }
    d.fillRoundRect(r.x + 12, base + 6, campo_l, ESCRITA_A - 12, 10, pode_escrever ? TFT_DARKGREY : FUNDO);
    if (!pode_escrever) { d.drawRoundRect(r.x + 12, base + 6, campo_l, ESCRITA_A - 12, 10, TFT_DARKGREY); }
    tab5_texto(d, TAM_ESCRITA);
    d.setTextDatum(top_left);
    d.setTextColor(pode_escrever ? TFT_WHITE : TFT_DARKGREY, pode_escrever ? TFT_DARKGREY : FUNDO);
    // Linhas centradas no campo pela altura REAL da fonte (uma linha so fica
    // no meio; duas dividem o campo) - o passo fixo antigo deixava a segunda
    // encostada na borda de baixo.
    const int fa = d.fontHeight();
    const int passo = fa + 4;
    const int y0 = base + 6 + (ESCRITA_A - 12 - (nl * passo - 4)) / 2;
    c->bar_x0 = r.x + 24;
    c->bar_y0 = y0;
    c->bar_passo = passo;
    char lin[CONVERSA_LINHA_BYTES];
    for (int k = 0; k < nl; ++k) {
      const int ini = c->bar_ini[topo + k], fim = c->bar_ini[topo + k + 1];
      int tam = fim - ini;
      if (tam >= CONVERSA_LINHA_BYTES) { tam = CONVERSA_LINHA_BYTES - 1; }
      memcpy(lin, fonte + ini, tam); lin[tam] = 0;
      d.drawString(lin, r.x + 24, y0 + k * passo);
    }
    if (eh_rascunho) {
      const int k = lc - topo;
      const int ini = c->bar_ini[lc];
      char pre[CONVERSA_LINHA_BYTES];
      int tam = c->cursor - ini;
      if (tam >= CONVERSA_LINHA_BYTES) { tam = CONVERSA_LINHA_BYTES - 1; }
      if (tam < 0) { tam = 0; }
      memcpy(pre, fonte + ini, tam); pre[tam] = 0;
      int cx = r.x + 24 + (int)d.textWidth(pre) + 2;
      if (cx > r.x + 12 + campo_l - 6) { cx = r.x + 12 + campo_l - 6; }
      d.fillRect(cx, y0 + k * passo, 3, fa, TFT_GREENYELLOW);
    }
    // Sem botao de enviar quando nao da para enviar: um botao que nao funciona e pior que
    // a ausencia dele.
    if (pode_escrever && env) {
      botao_enviar(d, r.x + r.largura - ENV_L, base + 6, ENV_L - 14, ESCRITA_A - 12,
                   c->cor_enviar ? c->cor_enviar : (uint16_t)TFT_GREENYELLOW, eh_rascunho, FUNDO);
    }
  }

  // ---- teclado virtual (do sistema: ele sabe o que repintar) ---------------
  tecvirt_posicionar(&c->kb, area_tec(c));
  tecvirt_pintar(&c->kb, d, completo);
}

// ---------------------------------------------------------------------------
// TECLAS
// ---------------------------------------------------------------------------
// O teclado olha o texto antes do cursor para armar a maiuscula de inicio de
// frase: chamado sempre que o texto ou o cursor mudam com ele aberto.
static void contexto(conversa_t* c) { tecvirt_contexto(&c->kb, c->rascunho, c->cursor); }

static void aplicar_tecla(conversa_t* c, int acao)
{
  if (acao > 0) {
    char u[4];   // codepoint ja com o shift (o teclado aplica)
    inserir(c, u, (size_t)tecvirt_utf8(acao, u));
    contexto(c);
    sistema_pedir_redesenho();
    return;
  }
  if (acao == TECVIRT_APAGAR) { apagar_antes(c); contexto(c); sistema_pedir_redesenho(); return; }
  if (acao == TECVIRT_ENVIAR) { enviar_rascunho(c); }
  if (acao == TECVIRT_ENVIAR || acao == TECVIRT_FECHAR) { fechar_teclado(c); }
}

// O toque em coordenadas de TELA, e relativo ao retangulo (o que o teclado usa).
static void para_tela(int x, int y, int* xs, int* ys)
{
  const area_t a = sistema_area();
  *xs = a.x + x;
  *ys = a.y + y;
}

// O teclado (dedos, retorno da tecla, soltura) e do sistema: aqui so se
// aplicam as teclas que ele confirmou nesta amostra.
void conversa_tocar_nivel(conversa_t* c, int x, int y, bool pressionado)
{
  int acoes[8];
  const int n = tecvirt_nivel(&c->kb, acoes, 8);
  for (int i = 0; i < n && tecvirt_aberto(&c->kb); ++i) { aplicar_tecla(c, acoes[i]); }

  // Rolagem dos baloes (a janela da lista ja termina acima da barra e do teclado).
  if (lista_arrastar(&c->lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

// TOQUE NO TEXTO: leva o cursor para o caractere mais proximo do dedo, sem
// apagar nada. Usa as posicoes da ULTIMA pintura da barra (bar_x0/bar_y0/
// bar_passo e as linhas visiveis) e mede com a mesma fonte dela.
static void cursor_no_toque(conversa_t* c, int xs, int ys)
{
  if (!c->bar_rascunho || !c->disp || c->bar_nl <= 0 || c->bar_passo <= 0) { return; }
  int k = (ys - c->bar_y0) / c->bar_passo;
  if (ys < c->bar_y0) { k = 0; }
  if (k >= c->bar_nl) { k = c->bar_nl - 1; }
  const int lin = c->bar_topo + k;
  const int ini = c->bar_ini[lin];
  int fim = c->bar_ini[lin + 1];
  // Nas linhas que nao sao a ultima, o espaco de quebra e da linha mas o
  // cursor depois dele seria desenhado na linha de baixo: para no ultimo
  // caractere visivel.
  if (lin + 1 < c->bar_n) { while (fim > ini && c->rascunho[fim - 1] == ' ') { --fim; } }

  lgfx::LovyanGFX& d = *c->disp;
  tab5_texto(d, TAM_ESCRITA);
  const int alvo = xs - c->bar_x0;
  char pre[CONVERSA_LINHA_BYTES];
  int melhor = ini, melhor_dist = 1 << 30;
  for (int i = ini;; i = utf8_seguinte(c->rascunho, i)) {
    int tam = i - ini;
    if (tam >= CONVERSA_LINHA_BYTES) { break; }
    memcpy(pre, c->rascunho + ini, tam); pre[tam] = 0;
    const int dist = abs((int)d.textWidth(pre) - alvo);
    if (dist < melhor_dist) { melhor_dist = dist; melhor = i; }
    if (i >= fim || !c->rascunho[i]) { break; }
  }
  c->cursor = melhor;
  contexto(c);
  sistema_pedir_redesenho();
}

void conversa_tocar(conversa_t* c, int x, int y)
{
  int xs, ys;
  para_tela(x, y, &xs, &ys);
  const area_t& r = c->ret;

  // Teclado aberto: a faixa dele e so dele. O clique aqui e so a rede para um
  // toque rapido demais para o nivel ter visto (senao daria letra dupla).
  if (tecvirt_contem(&c->kb, xs, ys)) {
    const int ac = tecvirt_clique(&c->kb, xs, ys);
    if (ac) { aplicar_tecla(c, ac); }
    return;
  }

  // O fim de uma rolagem nao e clique em nada.
  if (lista_item_tocado(&c->lista, x, y, nullptr) == LISTA_ARRASTE) { return; }

  // Barra de escrita: o botao da seta envia; o campo abre o teclado virtual (so sem o
  // fisico encaixado - com ele, nao roubamos meia tela).
  const int base = r.y + r.altura - ESCRITA_A - (tecvirt_aberto(&c->kb) ? tecvirt_altura(area_tec(c)) : 0);
  if (ys >= base && ys < base + ESCRITA_A && c->pode_escrever) {
    if (xs >= r.x + r.largura - ENV_L) {
      enviar_rascunho(c);
      fechar_teclado(c);
    } else if (tecvirt_aberto(&c->kb)) {
      cursor_no_toque(c, xs, ys);
    } else if (!tab5_teclado_presente()) {
      tecvirt_abrir(&c->kb);            // trava o INICIO; o dedo que abriu nao digita
      cursor_no_toque(c, xs, ys);       // o mesmo toque que abre ja posiciona o cursor
      contexto(c);                      // rascunho vazio: ja abre com a maiuscula armada
      pedir_completo(c);
    }
  }
}

// ---------------------------------------------------------------------------
// CURSOR PELO TECLADO FISICO
// ---------------------------------------------------------------------------
// Del: apaga o caractere DEPOIS do cursor (o Backspace apaga o de antes).
static void apagar_depois(conversa_t* c)
{
  cursor_ajustar(c);
  if (!c->rascunho[c->cursor]) { return; }
  const int b = utf8_seguinte(c->rascunho, c->cursor);
  memmove(c->rascunho + c->cursor, c->rascunho + b, strlen(c->rascunho) - b + 1);
}

// Ctrl + seta: pula uma palavra (os espacos e depois a palavra, como em
// qualquer editor).
static void palavra(conversa_t* c, bool frente)
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

// Quantos CARACTERES (nao bytes) ha de 'ini' ate 'fim'.
static int n_caracteres(const char* s, int ini, int fim)
{
  int n = 0;
  for (int i = ini; i < fim && s[i]; i = utf8_seguinte(s, i)) { ++n; }
  return n;
}

// Seta para cima/baixo: a linha de cima/baixo da barra, na mesma coluna (em
// caracteres). Usa a quebra da ULTIMA pintura (bar_ini) - a barra e repintada
// a cada tecla, entao ela e a do texto de agora. Ja na primeira linha, cima
// vai ao inicio; na ultima, baixo vai ao fim.
static void linha_vizinha(conversa_t* c, bool baixo)
{
  cursor_ajustar(c);
  const int len = (int)strlen(c->rascunho);
  const int n = c->bar_n;
  if (!c->bar_rascunho || n <= 0 || c->bar_ini[n] != len) {   // quebra velha ou ausente
    c->cursor = baixo ? len : 0;
    return;
  }
  int k = 0;
  while (k + 1 < n && c->cursor >= c->bar_ini[k + 1]) { ++k; }
  const int alvo = baixo ? k + 1 : k - 1;
  if (alvo < 0)  { c->cursor = 0;   return; }
  if (alvo >= n) { c->cursor = len; return; }
  const int col = n_caracteres(c->rascunho, c->bar_ini[k], c->cursor);
  int i = c->bar_ini[alvo];
  int fim = c->bar_ini[alvo + 1];
  // O espaco da quebra pertence a linha mas o cursor depois dele cairia na
  // linha de baixo: para no ultimo caractere visivel (como no toque).
  if (alvo + 1 < n) { while (fim > i && c->rascunho[fim - 1] == ' ') { --fim; } }
  for (int k2 = 0; k2 < col && i < fim; ++k2) { i = utf8_seguinte(c->rascunho, i); }
  c->cursor = i;
}

void conversa_teclar(conversa_t* c, const char* texto, uint8_t codigo, uint8_t mod)
{
  if (!c->pode_escrever) { return; }
  const bool ctrl = (mod & TAB5_TECLA_CTRL) != 0;
  switch (codigo) {
    case TAB5_TECLA_ENTER:  enviar_rascunho(c); fechar_teclado(c); return;
    case TAB5_TECLA_APAGAR: apagar_antes(c);  sistema_pedir_redesenho(); return;
    case TAB5_TECLA_DEL:    apagar_depois(c); sistema_pedir_redesenho(); return;
    // Esc limpa o rascunho inteiro (o cursor volta ao comeco).
    case TAB5_TECLA_ESC:    c->rascunho[0] = 0; c->cursor = 0; sistema_pedir_redesenho(); return;
    case TAB5_TECLA_ESQUERDA:
      cursor_ajustar(c);
      if (ctrl) { palavra(c, false); } else { c->cursor = utf8_anterior(c->rascunho, c->cursor); }
      sistema_pedir_redesenho();
      return;
    case TAB5_TECLA_DIREITA:
      cursor_ajustar(c);
      if (ctrl) { palavra(c, true); } else { c->cursor = utf8_seguinte(c->rascunho, c->cursor); }
      sistema_pedir_redesenho();
      return;
    // Ctrl + cima/baixo: comeco/fim do texto de uma vez.
    case TAB5_TECLA_CIMA:
      if (ctrl) { c->cursor = 0; } else { linha_vizinha(c, false); }
      sistema_pedir_redesenho();
      return;
    case TAB5_TECLA_BAIXO:
      if (ctrl) { c->cursor = (int)strlen(c->rascunho); } else { linha_vizinha(c, true); }
      sistema_pedir_redesenho();
      return;
    case TAB5_TECLA_TAB:    return;   // nada a fazer numa mensagem
    default: break;
  }
  // Com Ctrl ou Alt e atalho, nao texto: um "Ctrl+c" nao pode virar um "c".
  if (mod & (TAB5_TECLA_CTRL | TAB5_TECLA_ALT)) { return; }
  // Controle nao vira caractere. Uma sequencia acentuada vem com mais de um
  // byte e entra inteira (ou nao entra) - inserir() nunca corta no meio dela.
  char limpo[16];
  size_t n = 0;
  for (const char* ch = texto; ch && *ch && n < sizeof(limpo) - 1; ++ch) {
    if ((unsigned char)*ch < 0x20) { continue; }
    limpo[n++] = *ch;
  }
  limpo[n] = 0;
  inserir(c, limpo, n);
  sistema_pedir_redesenho();
}
