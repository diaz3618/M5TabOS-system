#include "list.hpp"
#include "internal.hpp"   // diag_toque_ligado
#include "esp_log.h"
#include "app.h"   // sistema_area - o toque chega em coordenadas da area

static const int BARRA_MARGEM = 8;   // rolagem_barra desenha 4px a 8px da borda
static const int BARRA_LARG   = 4;

void lista_definir(lista_t* l, uint8_t ancoragem, int n,
                   lista_altura_t altura, lista_desenhar_t desenhar, void* ctx)
{
  rolagem_definir(&l->rol, ROLAGEM_VERTICAL, ancoragem);
  l->n = n;
  l->altura = altura;
  l->desenhar = desenhar;
  l->ctx = ctx;
  l->topo = 0;
  l->total = 0;
  l->medida_ok = false;
  l->tudo = true;
  l->janela_ant = { 0, 0, 0, 0 };
  l->no_ar = false;
  l->sujo_de = 1; l->sujo_ate = 0;
}

static int somar(const lista_t* l)
{
  int t = l->topo;
  for (int i = 0; i < l->n; ++i) { t += l->altura(i, l->ctx); }
  return t;
}

void lista_itens_mudaram(lista_t* l, int n)
{
  const int antes = l->medida_ok ? l->total : -1;
  l->n = n;
  l->total = somar(l);
  l->medida_ok = true;
  l->tudo = true;

  // LENDO O HISTORICO QUANDO CHEGA ALGO NOVO EMBAIXO. Numa lista ancorada no
  // fim, 'atual' conta a partir de baixo: se o conteudo cresce e 'atual' fica
  // igual, tudo o que voce esta lendo sobe junto. Somando o crescimento, o
  // que esta na tela fica parado. Colado no fim (atual == 0) continua colado:
  // a mensagem nova aparece.
  if (l->rol.ancoragem == ROLAGEM_FIM && antes >= 0 && l->rol.atual > 0 && l->total > antes) {
    l->rol.atual += l->total - antes;
    l->rol.no_visor = l->rol.atual;
  }
}

void lista_item_mudou(lista_t* l, int i)
{
  if (i < 0) { return; }
  if (l->sujo_de > l->sujo_ate) { l->sujo_de = l->sujo_ate = i; return; }
  if (i < l->sujo_de) { l->sujo_de = i; }
  if (i > l->sujo_ate) { l->sujo_ate = i; }
}

void lista_ao_inicio(lista_t* l)
{
  rolagem_zerar(&l->rol);
  l->tudo = true;
}

void lista_esconder(lista_t* l)
{
  l->no_ar = false;
  l->rol.ancora = -1;
  l->tudo = true;   // voltando a aparecer, a janela foi pintada por outra tela
}

// Onde comeca o item 0, em coordenadas de tela.
static int y_do_primeiro(const lista_t* l, const area_t& j)
{
  int y0;
  if (l->rol.ancoragem == ROLAGEM_FIM) {
    if (l->total <= j.altura) { y0 = j.y + j.altura - l->total; }   // pouca coisa: colada embaixo
    else                      { y0 = j.y - (l->total - j.altura - l->rol.atual); }
  } else {
    y0 = j.y - l->rol.atual;
  }
  return y0 + l->topo;
}

static int largura_item(const area_t& j) { return j.largura - BARRA_MARGEM - BARRA_LARG; }

// Desenha os itens que cruzam 'rec' (ja recortado pelo chamador).
static void desenhar_faixa(lista_t* l, lgfx::LovyanGFX& d, const area_t& j, const area_t& rec, uint16_t fundo)
{
  if (rec.largura <= 0 || rec.altura <= 0) { return; }
  d.fillRect(rec.x, rec.y, rec.largura, rec.altura, fundo);

  int32_t cx, cy, cl, ca;
  d.getClipRect(&cx, &cy, &cl, &ca);
  d.setClipRect(rec.x, rec.y, rec.largura, rec.altura);

  const int fim = rec.y + rec.altura;
  int y = y_do_primeiro(l, j);
  for (int i = 0; i < l->n && y < fim; ++i) {
    const int h = l->altura(i, l->ctx);
    if (y + h > rec.y) {
      l->desenhar(d, i, j.x, y, largura_item(j), l->ctx);
      // O RECORTE E DA LISTA, nao do item. Primitivo do M5GFX que mexe nele
      // (drawWideLine troca pela caixa da linha e depois o apaga) soltava
      // os itens seguintes por cima do cabecalho e do rodape - rastro nas
      // duas pontas da janela a cada passo de rolagem.
      d.setClipRect(rec.x, rec.y, rec.largura, rec.altura);
    }
    y += h;
  }

  d.setClipRect(cx, cy, cl, ca);
}

// So os itens marcados por lista_item_mudou, e so a parte visivel deles. A
// coluna da barra fica de fora: o item nunca desenha nela.
static bool desenhar_sujos(lista_t* l, lgfx::LovyanGFX& d, const area_t& j, uint16_t fundo)
{
  if (l->sujo_de > l->sujo_ate) { return false; }
  bool pintou = false;
  int y = y_do_primeiro(l, j);
  for (int i = 0; i < l->n && i <= l->sujo_ate && y < j.y + j.altura; ++i) {
    const int h = l->altura(i, l->ctx);
    if (i >= l->sujo_de) {
      int y0 = y, y1 = y + h;
      if (y0 < j.y) { y0 = j.y; }
      if (y1 > j.y + j.altura) { y1 = j.y + j.altura; }
      if (y1 > y0) {
        const area_t rec = { j.x, y0, largura_item(j), y1 - y0 };
        desenhar_faixa(l, d, j, rec, fundo);
        pintou = true;
      }
    }
    y += h;
  }
  l->sujo_de = 1; l->sujo_ate = 0;
  return pintou;
}

bool lista_desenhar(lista_t* l, lgfx::LovyanGFX& d, const area_t& j, uint16_t fundo, bool tudo)
{
  if (!l->medida_ok) { lista_itens_mudaram(l, l->n); }
  l->no_ar = true;

  // A janela mudou (o teclado abriu ou fechou, a tela girou): tudo de novo.
  if (j.x != l->janela_ant.x || j.y != l->janela_ant.y
   || j.largura != l->janela_ant.largura || j.altura != l->janela_ant.altura) {
    l->janela_ant = j;
    tudo = true;
  }

  // O limite depende do total e da janela - e pode puxar 'atual' de volta.
  if (rolagem_limite(&l->rol, l->total, j.altura)) { tudo = true; }

  tudo = tudo || l->tudo;
  if (tudo) {
    desenhar_faixa(l, d, j, j, fundo);
    rolagem_sincronizar(&l->rol);
    l->sujo_de = 1; l->sujo_ate = 0;   // a janela inteira ja cobriu os marcados
  } else if (rolagem_pendente(&l->rol)) {
    const rolagem_passada_t p = rolagem_deslocar(&l->rol, j, fundo);
    if (p.deslocou) { desenhar_faixa(l, d, j, p.exposta, fundo); }
    else            { desenhar_faixa(l, d, j, j, fundo); tudo = true; }
    desenhar_sujos(l, d, j, fundo);    // depois do deslocamento: posicoes novas
  } else {
    return desenhar_sujos(l, d, j, fundo);   // so valores; a barra nao mudou
  }
  l->tudo = false;

  // A BARRA. O deslocamento arrastou os pixels dela junto com o conteudo:
  // a coluna e sempre reescrita inteira - trilha + bolinha, ou so fundo quando
  // ela nao deve aparecer (numa lista ancorada no fim, encostada no fim).
  const area_t col = { j.x + j.largura - BARRA_MARGEM, j.y, BARRA_LARG, j.altura };
  d.fillRect(col.x, col.y, col.largura, col.altura, fundo);
  rolagem_barra(d, &l->rol, j, fundo, BARRA_MARGEM);
  return true;
}

// ---------------------------------------------------------------------------
// TOQUE
// ---------------------------------------------------------------------------
// A janela guardada e de TELA; o toque chega em coordenadas da AREA do app.
static bool na_janela(const lista_t* l, int x, int y, int* ty)
{
  if (!l->no_ar) { return false; }
  const area_t a = sistema_area();
  const int tx = x + a.x;
  *ty = y + a.y;
  const area_t& j = l->janela_ant;
  return tx >= j.x && tx < j.x + j.largura && *ty >= j.y && *ty < j.y + j.altura;
}

bool lista_arrastar(lista_t* l, int x, int y, bool pressionado)
{
  int ty = 0;
  const bool dentro = pressionado && na_janela(l, x, y, &ty) && l->rol.max > 0;
  return rolagem_arrastar(&l->rol, ty, dentro);
}

int lista_item_tocado(lista_t* l, int x, int y, int* dy)
{
  const int afastou = l->rol.afastamento;
  if (rolagem_foi_arraste(&l->rol)) {   // fim de arraste nao e clique
    if (diag_toque_ligado()) { ESP_LOGI("lista", "toque recusado como arrasto da lista (%d px)", afastou); }
    return LISTA_ARRASTE;
  }
  int ty = 0;
  if (!na_janela(l, x, y, &ty)) { return -1; }
  int yi = y_do_primeiro(l, l->janela_ant);
  for (int i = 0; i < l->n; ++i) {
    const int h = l->altura(i, l->ctx);
    if (ty >= yi && ty < yi + h) {
      if (dy) { *dy = ty - yi; }
      return i;
    }
    yi += h;
  }
  return -1;
}
