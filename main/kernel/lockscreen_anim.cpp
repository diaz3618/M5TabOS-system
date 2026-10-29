// Animacoes of screen of bloqueio. Contrato e regras no lockscreen_anim.hpp.
//
// Each animation has three functions (and a fourth, optional):
//   start()  - sorteia o estado inicial;
// step(dt) - moves everything and registers with rec() the box that each object
//                ocupava e a que vai ocupar (essas caixas serao apagadas);
// draw() - paints all objects, after erasing;
//   fundo()    - (opcional) pinta o fundo fixo dela; sem isso, o fundo e one
//                drawing abstrato sorteado.
// O frame (bq_anim_quadro) liga as tres e guarda a lista of caixas para o
// shell saber onde o text precisa ser repintado.

#include "lockscreen_anim.hpp"

#include <math.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "tab5_ui.hpp"
#include "virtual_screen.hpp"

// ---------------------------------------------------------------------------
// Area, aleatorio, cores
// ---------------------------------------------------------------------------
static int s_ax, s_ay, s_W, s_H;   // area util (onde a animacao pode touch)

static uint32_t s_rng = 2463534242u;
static inline uint32_t rnd(void)
{
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
  return s_rng;
}
static inline float frand(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFF) / 65535.0f; }
static inline int   irand(int n)            { return (int)(rnd() % (uint32_t)n); }

static inline uint16_t c565(int r, int g, int b)
{
  if (r > 255) { r = 255; } if (g > 255) { g = 255; } if (b > 255) { b = 255; }
  if (r < 0)   { r = 0; }   if (g < 0)   { g = 0; }   if (b < 0)   { b = 0; }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Matiz 0..255 (vermelho, amarelo, verde, ciano, azul, magenta) com brilho v.
static uint16_t matiz(uint8_t h, int v)
{
  const int reg = h / 43;
  const int sub = (h - reg * 43) * 6;   // 0..252
  int r, g, b;
  switch (reg) {
    case 0:  r = 255;       g = sub;       b = 0;         break;
    case 1:  r = 255 - sub; g = 255;       b = 0;         break;
    case 2:  r = 0;         g = 255;       b = sub;       break;
    case 3:  r = 0;         g = 255 - sub; b = 255;       break;
    case 4:  r = sub;       g = 0;         b = 255;       break;
    default: r = 255;       g = 0;         b = 255 - sub; break;
  }
  return c565(r * v / 255, g * v / 255, b * v / 255);
}

// ---------------------------------------------------------------------------
// Lista of caixas mexidas neste frame
// ---------------------------------------------------------------------------
// Caixas que se SOBREPOEM viram uma so (erase o mesmo pixel duas vezes e
// desperdicio, e a chuva, por exemplo, gera duas por coluna quase iguais). O
// resto vai como esta: o compositor aguenta muitas areas pequenas por frame
// (compositor.cpp, MAX_AREAS). MAX_RET e so o tamanho of lista; encher e
// raro, e ai a nova se junta on par que desperdica menos.
static const int MAX_RET = 160;
static bq_ret_t s_ret[MAX_RET];
static int      s_n_ret = 0;
static bool     s_everything  = false;   // draw everything (not just what changed)

static inline int area_of(const bq_ret_t& r) { return r.w * r.h; }

static bq_ret_t envolver(const bq_ret_t& a, const bq_ret_t& b)
{
  const int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
  const int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
  const int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
  return { (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

// Pixel DOES NOT request that the junction of a and b forces erase (negative = they are
// sobrepoem: juntar exit of graca).
static inline int desperdicio(const bq_ret_t& a, const bq_ret_t& b)
{
  return area_of(envolver(a, b)) - area_of(a) - area_of(b);
}

static void rec(int x, int y, int w, int h)
{
  int x1 = x + w, y1 = y + h;
  if (x < s_ax)           { x = s_ax; }
  if (y < s_ay)           { y = s_ay; }
  if (x1 > s_ax + s_W)    { x1 = s_ax + s_W; }
  if (y1 > s_ay + s_H)    { y1 = s_ay + s_H; }
  if (x1 <= x || y1 <= y) { return; }
  bq_ret_t r = { (int16_t)x, (int16_t)y, (int16_t)(x1 - x), (int16_t)(y1 - y) };

  // Junta com quem ja se sobrepoe (of graca) - uma juncao pode open outra.
  for (bool juntou = true; juntou; ) {
    juntou = false;
    for (int i = 0; i < s_n_ret; ++i) {
      if (desperdicio(s_ret[i], r) <= 0) {
        r = envolver(s_ret[i], r);
        s_ret[i] = s_ret[--s_n_ret];
        juntou = true;
        break;
      }
    }
  }
  if (s_n_ret < MAX_RET) { s_ret[s_n_ret++] = r; return; }

  // Cheia: junta o par (a nova conta como candidata) que desperdica menos.
  int bi = 0, bj = -1, melhor = 0x7fffffff;
  for (int i = 0; i < s_n_ret; ++i) {
    const int dr = desperdicio(s_ret[i], r);
    if (dr < melhor) { melhor = dr; bi = i; bj = -1; }
    for (int j = i + 1; j < s_n_ret; ++j) {
      const int d = desperdicio(s_ret[i], s_ret[j]);
      if (d < melhor) { melhor = d; bi = i; bj = j; }
    }
  }
  if (bj < 0) {
    s_ret[bi] = envolver(s_ret[bi], r);
  } else {
    s_ret[bi] = envolver(s_ret[bi], s_ret[bj]);
    s_ret[bj] = r;
  }
  // A box together erases an object that has not moved: whoever draws only what
  // changed (a cobrinha) precisa draw everything.
  s_everything = true;
}

// Caixa que cobre dois pontos, com folga 'r' em iteration.
static void rec2(float x0, float y0, float x1, float y1, int r)
{
  const int xa = (int)floorf(fminf(x0, x1)) - r;
  const int ya = (int)floorf(fminf(y0, y1)) - r;
  rec(xa, ya, (int)ceilf(fabsf(x1 - x0)) + 2 * r + 1, (int)ceilf(fabsf(y1 - y0)) + 2 * r + 1);
}

// ---------------------------------------------------------------------------
// FUNDO
// ---------------------------------------------------------------------------
// Um drawing fixo atras of animacao: lines finas e escuras sobre preto. Ele
// e pintado UMA vez por bloqueio numa imagem of tamanho of area (PSRAM,
// ~1.6 MB, only while locked) and the "erase" of each box becomes
// copiar aquele pedaco of imagem em vez of paint preto - o mesmo tanto of
// written before, so the background costs nothing per frame. No memory,
// stays o preto of sempre.
static lgfx::LGFX_Sprite* s_fundo = nullptr;
static bool s_fundo_falhou = false;   // in this block: does not try again every frame
static bool s_fundo_paint = false;   // a imagem e nova: vai inteira para a screen

static void background_release(void)
{
  delete s_fundo;
  s_fundo = nullptr;
}

// Ponto in borda of imagem, 't' of 0 a 1 dando a iteration.
static void ponto_borda(int W, int H, float t, float* x, float* y)
{
  t -= floorf(t);
  const float p = 2.0f * (W + H), d = t * p;
  if (d < W)              { *x = d;                  *y = 0; }
  else if (d < W + H)     { *x = (float)W;           *y = d - W; }
  else if (d < 2 * W + H) { *x = (float)W - (d - W - H); *y = (float)H; }
  else                    { *x = 0;                  *y = (float)H - (d - 2 * W - H); }
}

// FIOS: "string art" - straight lines between two rails draw curves that do not
// existem.
static void background_fios(lgfx::LGFX_Sprite& s, int W, int H)
{
  const int env = 3 + irand(3);
  const uint8_t h0 = (uint8_t)irand(256);
  for (int e = 0; e < env; ++e) {
    float a0x, a0y, a1x, a1y, b0x, b0y, b1x, b1y;
    const float ta = frand(0, 1), tb = frand(0, 1);
    ponto_borda(W, H, ta, &a0x, &a0y);
    ponto_borda(W, H, ta + frand(0.15f, 0.3f), &a1x, &a1y);
    ponto_borda(W, H, tb, &b0x, &b0y);
    ponto_borda(W, H, tb + frand(0.15f, 0.3f), &b1x, &b1y);
    const int n = 44;
    const uint8_t h = (uint8_t)(h0 + e * 30);
    for (int i = 0; i <= n; ++i) {
      const float f = (float)i / n;
      const int v = 34 + (int)(40 * sinf(f * 3.1416f));
      s.drawLine((int)(a0x + (a1x - a0x) * f), (int)(a0y + (a1y - a0y) * f),
                 (int)(b1x + (b0x - b1x) * f), (int)(b1y + (b0y - b1y) * f), matiz((uint8_t)(h + i), v));
    }
  }
}

// ONDAS: curvas of level que escorrem of one lado on outro.
static void background_ondas(lgfx::LGFX_Sprite& s, int W, int H)
{
  const int n = 30;
  const float k1 = frand(0.004f, 0.009f), k2 = frand(0.011f, 0.02f);
  const float p1 = frand(0, 6.28f), p2 = frand(0, 6.28f);
  const float a1 = frand(24, 50), a2 = frand(8, 20);
  const uint8_t h0 = (uint8_t)(120 + irand(80));
  for (int i = 0; i < n; ++i) {
    const float y0 = (i + 0.5f) * H / n;
    const uint16_t color = matiz((uint8_t)(h0 + i * 2), 40 + (i % 3) * 12);
    int px = 0, py = 0;
    for (int x = 0; x <= W + 12; x += 12) {
      const int y = (int)(y0 + a1 * sinf(x * k1 + i * 0.28f + p1) + a2 * sinf(x * k2 - i * 0.17f + p2));
      if (x > 0) { s.drawLine(px, py, x, y, color); }
      px = x; py = y;
    }
  }
}

// TEIA: pontos ligados aos vizinhos mais proximos.
static void background_teia(lgfx::LGFX_Sprite& s, int W, int H)
{
  const int n = 38;
  int16_t px[n], py[n];
  for (int i = 0; i < n; ++i) { px[i] = (int16_t)irand(W); py[i] = (int16_t)irand(H); }
  const uint8_t h = (uint8_t)irand(256);
  for (int i = 0; i < n; ++i) {
    int viz[3] = { -1, -1, -1 };
    int dist[3] = { 0x7fffffff, 0x7fffffff, 0x7fffffff };
    for (int j = 0; j < n; ++j) {
      if (j == i) { continue; }
      const int dx = px[j] - px[i], dy = py[j] - py[i], d = dx * dx + dy * dy;
      for (int k = 0; k < 3; ++k) {
        if (d < dist[k]) {
          for (int m = 2; m > k; --m) { dist[m] = dist[m - 1]; viz[m] = viz[m - 1]; }
          dist[k] = d; viz[k] = j;
          break;
        }
      }
    }
    for (int k = 0; k < 3; ++k) {
      if (viz[k] >= 0) { s.drawLine(px[i], py[i], px[viz[k]], py[viz[k]], matiz((uint8_t)(h + k * 12), 44)); }
    }
  }
  for (int i = 0; i < n; ++i) { s.fillCircle(px[i], py[i], 2, matiz(h, 110)); }
}

// RAYS: fan coming out of one point off screen, with rings in iteration (moire).
static void background_raios(lgfx::LGFX_Sprite& s, int W, int H)
{
  float fx, fy;
  ponto_borda(W, H, frand(0, 1), &fx, &fy);
  fx += (fx < W / 2 ? -60.0f : 60.0f);
  fy += (fy < H / 2 ? -60.0f : 60.0f);
  const float R = sqrtf((float)W * W + (float)H * H) + 80;
  const uint8_t h = (uint8_t)irand(256);
  const int n = 72;
  for (int i = 0; i < n; ++i) {
    const float ang = 6.2832f * i / n;
    s.drawLine((int)fx, (int)fy, (int)(fx + cosf(ang) * R), (int)(fy + sinf(ang) * R),
               matiz((uint8_t)(h + (i & 1) * 20), (i & 1) ? 30 : 48));
  }
  for (int r = 50; r < R; r += 46) { s.drawCircle((int)fx, (int)fy, r, matiz((uint8_t)(h + 128), 34)); }
}

// CIRCULOS: aneis of dois ou tres centros se cruzando (interferencia).
static void background_circulos(lgfx::LGFX_Sprite& s, int W, int H)
{
  const int n = 2 + irand(2);
  const uint8_t h = (uint8_t)irand(256);
  const int R = (int)sqrtf((float)W * W + (float)H * H);
  for (int c = 0; c < n; ++c) {
    const int cx = irand(W), cy = irand(H);
    const uint16_t color = matiz((uint8_t)(h + c * 70), 40);
    for (int r = 14; r < R; r += 22) { s.drawCircle(cx, cy, r, color); }
  }
}

// Estrelas fixas, bem fracas (fundo of sistema solar).
static void background_ceu(lgfx::LGFX_Sprite& s, int W, int H)
{
  for (int i = 0; i < 160; ++i) {
    const int v = 40 + irand(90);
    s.drawPixel(irand(W), irand(H), c565(v, v, v + 20));
  }
}

// Sobe a imagem inteira para a screen, em faixas (one pushSprite por faixa: a
// same rule of not dumping 1.6 MB at once - see tab5_screen_clear).
static void background_paint_everything(lgfx::LovyanGFX& d)
{
  const int FAIXA = 128;
  for (int y = 0; y < s_H; y += FAIXA) {
    const int h = (y + FAIXA <= s_H) ? FAIXA : s_H - y;
    virtual_screen_cover(s_ax, s_ay + y, s_W, h);
    d.setClipRect(s_ax, s_ay + y, s_W, h);
    s_fundo->pushSprite(&d, s_ax, s_ay);
  }
  d.clearClipRect();
}

// Apaga uma caixa: devolve o fundo dela (ou preto, sem fundo).
static void erase(lgfx::LovyanGFX& d, const bq_ret_t& r)
{
  if (!s_fundo) {
    d.fillRect(r.x, r.y, r.w, r.h, TFT_BLACK);
    return;
  }
  // Cover first: the entire box will be written, so the composer will not
  // precisa trazer o frame anterior dela of outra pagina.
  virtual_screen_cover(r.x, r.y, r.w, r.h);
  d.setClipRect(r.x, r.y, r.w, r.h);
  s_fundo->pushSprite(&d, s_ax, s_ay);
}

// ---------------------------------------------------------------------------
// 1. BOLAS - quicam nas paredes e umas nas outras (choque elastico)
// ---------------------------------------------------------------------------
namespace {
struct Bola { float x, y, vx, vy, r, ox, oy; uint16_t color; };
struct Bolas { Bola b[7]; };
}

// ---------------------------------------------------------------------------
// 2. FOGOS - foguete sobe, explode em particulas que caem e apagam
// ---------------------------------------------------------------------------
namespace {
static const int N_FOGUETES = 3;
static const int N_PART     = 80;
struct Foguete { float x, y, vy, alvo, oy; bool on; };
// crep: on erase, released uma chuvinha of faiscas pequenas (estalo).
struct Part    { float x, y, vx, vy, vida, vida0, ox, oy; uint8_t h; bool on, crep; };
struct Fogos   { Foguete f[N_FOGUETES]; Part p[N_PART]; float espera; };
}

// ---------------------------------------------------------------------------
// 3. CHUVA DIGITAL - riscos verdes caindo
// ---------------------------------------------------------------------------
namespace {
struct Coluna { float y, v; int16_t x, len; };
struct Chuva  { Coluna c[18]; };
}

// ---------------------------------------------------------------------------
// 4. ESTRELAS - viagem pela velocidade of luz
// ---------------------------------------------------------------------------
namespace {
struct Estrela { float ca, sa, r, k; };
struct Estrelas { Estrela e[40]; };
}

// ---------------------------------------------------------------------------
// 5. BOLHAS - sobem balancando
// ---------------------------------------------------------------------------
namespace {
struct Bolha  { float x0, y, r, vy, fase, fv, amp, ox, oy; uint8_t h; };
struct Bolhas { Bolha b[14]; };
}

// ---------------------------------------------------------------------------
// 6. COBRINHA - joga sozinha (homenagem on CreazySnake)
// ---------------------------------------------------------------------------
namespace {
static const int CEL = 40;   // lado of celula, px
struct Cobra {
  uint8_t sx[64], sy[64];    // anel: [cab] e a cabeca, [cab-1] o pescoco...
  uint8_t occ[800];          // gx*gy: 1 = body
  int     gx, gy, ox, oy;    // grade e origem in screen
  int     cab, len;
  int     fx, fy;            // comida
  int     dx, dy;            // direcao
  float   acum;
  bool    passou;            // deu one passo neste frame
};
}

// ---------------------------------------------------------------------------
// 7. VAGA-LUMES - brilham e apagam, vagando
// ---------------------------------------------------------------------------
namespace {
struct Lume  { float x, y, ang, vel, fase, fv, ox, oy; uint8_t h; };
struct Lumes { Lume l[16]; };
}


// ---------------------------------------------------------------------------
// 8. PENDULOS - the "wave of pendulums": each ball in a slightly different rhythm,
//    e os desenhos (serpente, duas fileiras, caos, fileira of novo) se repetem
// ---------------------------------------------------------------------------
namespace {
static const int N_PEND = 18;
struct Pendulos { float t; float ox[N_PEND]; };
}

// ---------------------------------------------------------------------------
// 9. NEVE - flocos of tres tamanhos (os pequenos, longe, caem devagar) e one
//    vento que vai e iteration
// ---------------------------------------------------------------------------
namespace {
static const int N_FLOCO = 46;
struct Floco { float x, y, vy, fase, ox, oy; uint8_t size; };
struct Neve  { Floco f[N_FLOCO]; float tv; };
}

// ---------------------------------------------------------------------------
// 10. COMETS - three comets with tails, each one in a Lissajous curve
// ---------------------------------------------------------------------------
namespace {
static const int N_COMETA = 3;
static const int N_CAUDA  = 12;
struct Cometa  { float a, b, fase, vel, t, rx, ry; float ox[N_CAUDA], oy[N_CAUDA]; uint8_t h; };
struct Cometas { Cometa c[N_COMETA]; };
}

// ---------------------------------------------------------------------------
// 11. SISTEMA SOLAR - sol pulsando, seis planetas (one com lua, one com anel)
// ---------------------------------------------------------------------------
namespace {
static const int N_PLANETA = 6;
struct Planeta { float orb, ang, w, ox, oy; int16_t r; uint16_t color; };
struct Solar   { Planeta p[N_PLANETA]; float t, lua, lox, loy; int cx, cy; };
}

// ---------------------------------------------------------------------------
// 12. LOGO - the "M5tabOS" bouncing on the edges and changing color with each beat
//     (homenagem on descanso of screen of DVD; acertar o canto e raro)
// ---------------------------------------------------------------------------
namespace {
struct Logo { float x, y, vx, vy, ox, oy; int w, h; uint8_t color; bool repintar; uint8_t cantos; };
}
static lgfx::LGFX_Sprite* s_logo = nullptr;   // o text, pintado so when a color muda

// O estado of TODAS as animacoes divide estes bytes: so a sorteada vive.
static union {
  Bolas    bolas;
  Fogos    fogos;
  Chuva    chuva;
  Estrelas estrelas;
  Bolhas   bolhas;
  Cobra    cobra;
  Lumes    lumes;
  Pendulos pendulos;
  Neve     neve;
  Cometas  cometas;
  Solar    solar;
  Logo     logo;
} U;


// ===========================================================================
// BOLAS
// ===========================================================================
static void bolas_start(void)
{
  Bolas& s = U.bolas;
  for (int i = 0; i < 7; ++i) {
    Bola& b = s.b[i];
    b.r  = (float)(16 + irand(22));
    b.x  = frand(s_ax + b.r, s_ax + s_W - b.r);
    b.y  = frand(s_ay + b.r, s_ay + s_H - b.r);
    const float v = frand(240, 420), ang = frand(0, 6.2832f);
    b.vx = cosf(ang) * v;
    b.vy = sinf(ang) * v;
    b.color = matiz((uint8_t)(i * 37 + irand(16)), 225);
    b.ox = b.x; b.oy = b.y;
  }
}

static void bolas_passo(float dt)
{
  Bolas& s = U.bolas;
  for (int i = 0; i < 7; ++i) {
    Bola& b = s.b[i];
    b.ox = b.x; b.oy = b.y;
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    if (b.x < s_ax + b.r)            { b.x = s_ax + b.r;            b.vx =  fabsf(b.vx); }
    else if (b.x > s_ax + s_W - b.r) { b.x = s_ax + s_W - b.r;      b.vx = -fabsf(b.vx); }
    if (b.y < s_ay + b.r)            { b.y = s_ay + b.r;            b.vy =  fabsf(b.vy); }
    else if (b.y > s_ay + s_H - b.r) { b.y = s_ay + s_H - b.r;      b.vy = -fabsf(b.vy); }
  }
  // Choque elastico; a massa vai com a area (r^2).
  for (int i = 0; i < 7; ++i) {
    for (int j = i + 1; j < 7; ++j) {
      Bola& a = s.b[i];
      Bola& c = s.b[j];
      const float dx = c.x - a.x, dy = c.y - a.y;
      const float d2 = dx * dx + dy * dy, rr = a.r + c.r;
      if (d2 >= rr * rr || d2 < 0.01f) { continue; }
      const float d = sqrtf(d2), nx = dx / d, ny = dy / d;
      const float ma = a.r * a.r, mc = c.r * c.r, tot = ma + mc;
      const float pen = rr - d;
      a.x -= nx * pen * mc / tot;  a.y -= ny * pen * mc / tot;
      c.x += nx * pen * ma / tot;  c.y += ny * pen * ma / tot;
      const float p = (a.vx - c.vx) * nx + (a.vy - c.vy) * ny;
      if (p > 0) {
        a.vx -= 2 * mc / tot * p * nx;  a.vy -= 2 * mc / tot * p * ny;
        c.vx += 2 * ma / tot * p * nx;  c.vy += 2 * ma / tot * p * ny;
      }
    }
  }
  for (int i = 0; i < 7; ++i) {
    const Bola& b = s.b[i];
    rec2(b.ox, b.oy, b.x, b.y, (int)b.r + 2);
  }
}

static void bolas_draw(lgfx::LovyanGFX& d)
{
  const Bolas& s = U.bolas;
  for (int i = 0; i < 7; ++i) {
    const Bola& b = s.b[i];
    d.fillCircle((int)b.x, (int)b.y, (int)b.r, b.color);
    const int hr = (int)fmaxf(2.0f, b.r * 0.22f);
    d.fillCircle((int)(b.x - b.r * 0.35f), (int)(b.y - b.r * 0.35f), hr, c565(235, 235, 235));
  }
}

// ===========================================================================
// FOGOS
// ===========================================================================
static void fogos_start(void)
{
  Fogos& s = U.fogos;
  memset(&s, 0, sizeof(s));
  // It already starts with a rocket in the middle way: without this the screen remains empty for
  // about 1 s until the first raise and explodes.
  Foguete& f = s.f[0];
  f.x = frand(s_ax + s_W * 0.25f, s_ax + s_W * 0.75f);
  f.y = f.oy = s_ay + s_H * 0.6f;
  f.vy = -520;
  f.alvo = s_ay + s_H * 0.3f;
  f.on = true;
  s.espera = 0.4f;
}

// Solta 'n' particulas a partir of (x, y). Anel: todas in mesma velocidade.
static int fogos_release(Fogos& s, float x, float y, uint8_t h, int n, float vmin, float vmax,
                        float vida_min, float vida_max, bool anel, bool crep)
{
  int k = 0;
  for (int i = 0; i < N_PART && k < n; ++i) {
    Part& p = s.p[i];
    if (p.on) { continue; }
    const float ang = 6.2832f * k / n + frand(-0.1f, 0.1f);
    const float vel = anel ? vmax : frand(vmin, vmax);
    p.x = p.ox = x;  p.y = p.oy = y;
    p.vx = cosf(ang) * vel;
    p.vy = sinf(ang) * vel;
    p.vida = p.vida0 = frand(vida_min, vida_max);
    p.h  = (irand(4) == 0) ? (uint8_t)(h + 40) : h;
    p.crep = crep;
    p.on = true;
    ++k;
  }
  return k;
}

// Explosion in layers: core + outer ring (+ bursts that crackle).
static void fogos_explodir(Fogos& s, float x, float y)
{
  const uint8_t h = (uint8_t)irand(256);
  switch (irand(3)) {
    case 0:   // anel duplo
      fogos_release(s, x, y, h, 22, 0, 330.0f, 1.0f, 1.5f, true, false);
      fogos_release(s, x, y, (uint8_t)(h + 85), 14, 0, 190.0f, 0.9f, 1.3f, true, false);
      break;
    case 1:   // chuva of ouro: stays caindo e estalando
      fogos_release(s, x, y, 40, 30, 100, 360, 1.3f, 2.0f, false, true);
      break;
    default:  // esfera cheia, algumas estalam
      fogos_release(s, x, y, h, 24, 80, 360, 1.0f, 1.6f, false, true);
      fogos_release(s, x, y, (uint8_t)(h + 30), 10, 40, 170, 0.8f, 1.2f, false, false);
      break;
  }
}

static void fogos_passo(float dt)
{
  Fogos& s = U.fogos;
  s.espera -= dt;
  if (s.espera <= 0) {
    for (int i = 0; i < N_FOGUETES; ++i) {
      Foguete& f = s.f[i];
      if (f.on) { continue; }
      f.x = frand(s_ax + s_W * 0.15f, s_ax + s_W * 0.85f);
      f.y = f.oy = (float)(s_ay + s_H);
      f.vy = -frand(700, 950);
      f.alvo = s_ay + s_H * frand(0.15f, 0.55f);
      f.on = true;
      break;
    }
    s.espera = frand(0.3f, 1.0f);
  }
  for (int i = 0; i < N_FOGUETES; ++i) {
    Foguete& f = s.f[i];
    if (!f.on) { continue; }
    f.oy = f.y;
    f.y += f.vy * dt;
    f.vy += 250 * dt;   // perde forca in subida
    if (f.y <= f.alvo || f.vy > -120) {
      f.on = false;
      rec(f.x - 3, f.oy - 3, 7, 30);
      rec(f.x - 3, f.y - 3, 7, 30);
      fogos_explodir(s, f.x, f.y);
    } else {
      rec((int)f.x - 3, (int)f.y - 3, 7, (int)(f.oy - f.y) + 30);
    }
  }
  for (int i = 0; i < N_PART; ++i) {
    Part& p = s.p[i];
    if (!p.on) { continue; }
    p.ox = p.x; p.oy = p.y;
    p.vida -= dt;
    if (p.vida <= 0) {
      p.on = false;
      rec2(p.ox, p.oy, p.ox, p.oy, 3);
      if (p.crep) {   // estalo: faiscas pequenas e curtas
        fogos_release(s, p.x, p.y, p.h, 4, 30, 120, 0.3f, 0.6f, false, false);
      }
      continue;
    }
    p.vy += 320 * dt;
    const float k = 1.0f - 1.4f * dt;
    p.vx *= k; p.vy *= k;
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    rec2(p.ox, p.oy, p.x, p.y, 3);
  }
}

static void fogos_draw(lgfx::LovyanGFX& d)
{
  const Fogos& s = U.fogos;
  for (int i = 0; i < N_FOGUETES; ++i) {
    const Foguete& f = s.f[i];
    if (!f.on) { continue; }
    d.fillRect((int)f.x - 1, (int)f.y + 4, 3, 22, c565(255, 170, 70));
    d.fillRect((int)f.x - 2, (int)f.y - 2, 4, 6, c565(255, 245, 220));
  }
  for (int i = 0; i < N_PART; ++i) {
    const Part& p = s.p[i];
    if (!p.on) { continue; }
    const float f = p.vida / p.vida0;
    const int sz = f > 0.6f ? 4 : (f > 0.25f ? 3 : 2);
    const uint16_t color = f > 0.85f ? c565(255, 250, 235) : matiz(p.h, (int)(255 * f * f));
    d.fillRect((int)p.x - sz / 2, (int)p.y - sz / 2, sz, sz, color);
  }
}

// ===========================================================================
// CHUVA DIGITAL
// ===========================================================================
static void chuva_nova(Coluna& c, bool no_topo)
{
  c.v   = frand(260, 620);
  c.len = (int16_t)frand(120, 320);
  c.y   = no_topo ? (s_ay - frand(0, 300)) : frand(s_ay, s_ay + s_H + c.len);
}

static void chuva_start(void)
{
  Chuva& s = U.chuva;
  for (int i = 0; i < 18; ++i) {
    s.c[i].x = (int16_t)(s_ax + (i * s_W) / 18 + 14);
    chuva_nova(s.c[i], false);
  }
}

static void chuva_passo(float dt)
{
  Chuva& s = U.chuva;
  for (int i = 0; i < 18; ++i) {
    Coluna& c = s.c[i];
    rec(c.x - 1, (int)(c.y - c.len) - 2, 12, c.len + 14);
    c.y += c.v * dt;
    if (c.y - c.len > s_ay + s_H) { chuva_nova(c, true); }
    rec(c.x - 1, (int)(c.y - c.len) - 2, 12, c.len + 14);
  }
}

static void chuva_draw(lgfx::LovyanGFX& d)
{
  const Chuva& s = U.chuva;
  for (int i = 0; i < 18; ++i) {
    const Coluna& c = s.c[i];
    const int n = c.len / 10;
    for (int k = 0; k < n; ++k) {
      if (k > 0 && ((i * 7 + k * 13) & 3) == 0) { continue; }   // buracos: parece text
      const int yy = (int)c.y - k * 10 - 8;
      if (yy + 6 < s_ay || yy > s_ay + s_H) { continue; }
      const float f = 1.0f - (float)k / n;
      const int v = 40 + (int)(190 * f * f);
      d.fillRect(c.x, yy, 8, 6, k == 0 ? c565(210, 255, 220) : c565(v >> 4, v, v >> 3));
    }
  }
}

// ===========================================================================
// ESTRELAS
// ===========================================================================
static void estrela_nova(Estrela& e, bool espalhar)
{
  const float ang = frand(0, 6.2832f);
  e.ca = cosf(ang);
  e.sa = sinf(ang);
  e.r  = espalhar ? frand(4, 500) : frand(4, 40);
  e.k  = frand(0.6f, 1.4f);
}

static void estrelas_start(void)
{
  for (int i = 0; i < 40; ++i) { estrela_nova(U.estrelas.e[i], true); }
}

static void estrelas_passo(float dt)
{
  const float cx = s_ax + s_W * 0.5f, cy = s_ay + s_H * 0.5f;
  for (int i = 0; i < 40; ++i) {
    Estrela& e = U.estrelas.e[i];
    const float r0 = e.r;
    e.r += (50.0f + r0 * 2.0f) * e.k * dt;
    const float hx = cx + e.ca * e.r, hy = cy + e.sa * e.r;
    const bool fora = hx < s_ax - 8 || hx > s_ax + s_W + 8 || hy < s_ay - 8 || hy > s_ay + s_H + 8;
    rec2(cx + e.ca * r0 * 0.88f, cy + e.sa * r0 * 0.88f, hx, hy, 3);
    if (fora) {
      estrela_nova(e, false);
      rec2(cx + e.ca * e.r * 0.88f, cy + e.sa * e.r * 0.88f, cx + e.ca * e.r, cy + e.sa * e.r, 3);
    }
  }
}

static void estrelas_draw(lgfx::LovyanGFX& d)
{
  const float cx = s_ax + s_W * 0.5f, cy = s_ay + s_H * 0.5f;
  for (int i = 0; i < 40; ++i) {
    const Estrela& e = U.estrelas.e[i];
    const int x1 = (int)(cx + e.ca * e.r), y1 = (int)(cy + e.sa * e.r);
    const int x0 = (int)(cx + e.ca * e.r * 0.88f), y0 = (int)(cy + e.sa * e.r * 0.88f);
    const int v = (int)fminf(255.0f, 70.0f + e.r * 0.8f);
    const uint16_t color = c565(v * 3 / 4, v * 7 / 8, v);
    d.drawLine(x0, y0, x1, y1, color);
    if (e.r > 180) { d.drawLine(x0 + 1, y0, x1 + 1, y1, color); }
    if (e.r > 320) { d.drawLine(x0, y0 + 1, x1, y1 + 1, color); }
  }
}

// ===========================================================================
// BOLHAS
// ===========================================================================
static void bolha_nova(Bolha& b, bool espalhar)
{
  b.r    = frand(10, 40);
  b.amp  = frand(14, 30);
  b.x0   = frand(s_ax + b.amp + b.r, s_ax + s_W - b.amp - b.r);
  b.y    = espalhar ? frand(s_ay, s_ay + s_H) : (s_ay + s_H + b.r + frand(0, 200));
  b.vy   = frand(40, 110);
  b.fase = frand(0, 6.2832f);
  b.fv   = frand(1.0f, 2.2f);
  b.h    = (uint8_t)(110 + irand(60));   // of verde on azul, com one touch of roxo
  b.ox   = b.x0; b.oy = b.y;
}

static void bolhas_start(void)
{
  for (int i = 0; i < 14; ++i) { bolha_nova(U.bolhas.b[i], true); }
}

static void bolhas_passo(float dt)
{
  for (int i = 0; i < 14; ++i) {
    Bolha& b = U.bolhas.b[i];
    const float ox = b.x0 + b.amp * sinf(b.fase), oy = b.y;
    b.fase += b.fv * dt;
    b.y    -= b.vy * dt;
    const float nx = b.x0 + b.amp * sinf(b.fase);
    rec2(ox, oy, nx, b.y, (int)b.r + 2);
    if (b.y + b.r < s_ay) { bolha_nova(b, false); }
  }
}

static void bolhas_draw(lgfx::LovyanGFX& d)
{
  for (int i = 0; i < 14; ++i) {
    const Bolha& b = U.bolhas.b[i];
    const int x = (int)(b.x0 + b.amp * sinf(b.fase)), y = (int)b.y, r = (int)b.r;
    const uint16_t color = matiz(b.h, 210);
    d.drawCircle(x, y, r, color);
    d.drawCircle(x, y, r - 1, color);
    d.fillCircle(x - (int)(b.r * 0.45f), y - (int)(b.r * 0.45f), (int)fmaxf(2.0f, b.r * 0.12f),
                 c565(235, 245, 255));
  }
}

// ===========================================================================
// COBRINHA
// ===========================================================================
static inline int cobra_i(const Cobra& s, int k) { return (s.cab - k) & 63; }   // k-esimo segmento a partir of cabeca

static void cobra_comida(Cobra& s)
{
  for (int tent = 0; tent < 200; ++tent) {
    const int x = irand(s.gx), y = irand(s.gy);
    if (!s.occ[y * s.gx + x]) { s.fx = x; s.fy = y; rec(s.ox + x * CEL, s.oy + y * CEL, CEL, CEL); return; }
  }
  s.fx = s.fy = 0;
}

static void cobra_reiniciar(Cobra& s)
{
  // Apaga o que ainda esta in screen, celula por celula (nada of limpar a area).
  if (s.len > 0) {
    for (int k = 0; k < s.len; ++k) {
      const int i = cobra_i(s, k);
      rec(s.ox + s.sx[i] * CEL, s.oy + s.sy[i] * CEL, CEL, CEL);
    }
    rec(s.ox + s.fx * CEL, s.oy + s.fy * CEL, CEL, CEL);
  }
  memset(s.occ, 0, sizeof(s.occ));
  s.len = 4;
  s.dx = 1; s.dy = 0;
  const int hx = s.gx / 2, hy = s.gy / 2;
  s.cab = 3;
  for (int k = 0; k < 4; ++k) {
    const int i = cobra_i(s, k);
    s.sx[i] = (uint8_t)(hx - k);
    s.sy[i] = (uint8_t)hy;
    s.occ[hy * s.gx + hx - k] = 1;
  }
  cobra_comida(s);
  s.acum = 0;
  s_everything = true;
}

static void cobra_start(void)
{
  Cobra& s = U.cobra;
  memset(&s, 0, sizeof(s));
  s.gx = s_W / CEL;
  s.gy = s_H / CEL;
  if (s.gx * s.gy > (int)sizeof(s.occ)) { s.gy = (int)sizeof(s.occ) / s.gx; }
  s.ox = s_ax + (s_W - s.gx * CEL) / 2;
  s.oy = s_ay + (s_H - s.gy * CEL) / 2;
  cobra_reiniciar(s);
}

static void cobra_passo(float dt)
{
  Cobra& s = U.cobra;
  s.passou = false;
  s.acum += dt;
  if (s.acum < 0.10f) { return; }
  s.acum -= 0.10f;
  if (s.acum > 0.10f) { s.acum = 0; }

  static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
  const int hx = s.sx[s.cab], hy = s.sy[s.cab];
  const int it = cobra_i(s, s.len - 1);
  const int tx = s.sx[it], ty = s.sy[it];
  int melhor = -1;
  float nota = 1e9f;
  for (int k = 0; k < 4; ++k) {
    if (DX[k] == -s.dx && DY[k] == -s.dy) { continue; }
    const int nx = hx + DX[k], ny = hy + DY[k];
    if (nx < 0 || ny < 0 || nx >= s.gx || ny >= s.gy) { continue; }
    const bool come = nx == s.fx && ny == s.fy;
    if (s.occ[ny * s.gx + nx] && (come || nx != tx || ny != ty)) { continue; }
    int livres = 0;
    for (int q = 0; q < 4; ++q) {
      const int ax = nx + DX[q], ay = ny + DY[q];
      if (ax >= 0 && ay >= 0 && ax < s.gx && ay < s.gy && !s.occ[ay * s.gx + ax]) { ++livres; }
    }
    float n = (float)(abs(nx - s.fx) + abs(ny - s.fy)) + frand(0, 0.9f);
    if (livres < 2)                          { n += 8; }
    if (DX[k] == s.dx && DY[k] == s.dy)      { n -= 0.4f; }
    if (n < nota) { nota = n; melhor = k; }
  }
  if (melhor < 0) { cobra_reiniciar(s); return; }   // encurralou: recomeca

  const int nx = hx + DX[melhor], ny = hy + DY[melhor];
  s.dx = DX[melhor]; s.dy = DY[melhor];
  const bool come = nx == s.fx && ny == s.fy;
  rec(s.ox + hx * CEL, s.oy + hy * CEL, CEL, CEL);   // o pescoco troca of color
  if (!come) {
    s.occ[ty * s.gx + tx] = 0;
    rec(s.ox + tx * CEL, s.oy + ty * CEL, CEL, CEL);
  } else {
    ++s.len;
  }
  s.cab = (s.cab + 1) & 63;
  s.sx[s.cab] = (uint8_t)nx;
  s.sy[s.cab] = (uint8_t)ny;
  s.occ[ny * s.gx + nx] = 1;
  rec(s.ox + nx * CEL, s.oy + ny * CEL, CEL, CEL);
  s.passou = true;
  if (come) {
    if (s.len >= 50) { cobra_reiniciar(s); } else { cobra_comida(s); }
  }
}

static void cobra_celula(lgfx::LovyanGFX& d, const Cobra& s, int gx, int gy, uint16_t color)
{
  d.fillRoundRect(s.ox + gx * CEL + 2, s.oy + gy * CEL + 2, CEL - 4, CEL - 4, 9, color);
}

static void cobra_draw(lgfx::LovyanGFX& d)
{
  Cobra& s = U.cobra;
  if (!s.passou && !s_everything) { return; }
  d.fillCircle(s.ox + s.fx * CEL + CEL / 2, s.oy + s.fy * CEL + CEL / 2 + 1, 12, c565(255, 70, 70));
  d.fillRect(s.ox + s.fx * CEL + CEL / 2, s.oy + s.fy * CEL + 5, 3, 7, c565(120, 200, 80));
  // Body: of tail on neck, in stripes; just the last two if not everything.
  const int ate = s_everything ? s.len : 2;
  for (int k = ate - 1; k >= 1; --k) {
    const int i = cobra_i(s, k);
    cobra_celula(d, s, s.sx[i], s.sy[i], (k & 1) ? c565(30, 170, 60) : c565(20, 125, 45));
  }
  const int hx = s.sx[s.cab], hy = s.sy[s.cab];
  cobra_celula(d, s, hx, hy, c565(110, 255, 120));
  const int cx = s.ox + hx * CEL + CEL / 2, cy = s.oy + hy * CEL + CEL / 2;
  for (int lado = -1; lado <= 1; lado += 2) {
    const int ex = cx + s.dx * 8 - s.dy * 8 * lado, ey = cy + s.dy * 8 + s.dx * 8 * lado;
    d.fillCircle(ex, ey, 4, TFT_WHITE);
    d.fillCircle(ex + s.dx, ey + s.dy, 2, TFT_BLACK);
  }
}

// ===========================================================================
// VAGA-LUMES
// ===========================================================================
static void lumes_start(void)
{
  for (int i = 0; i < 16; ++i) {
    Lume& l = U.lumes.l[i];
    l.x = frand(s_ax + 20, s_ax + s_W - 20);
    l.y = frand(s_ay + 20, s_ay + s_H - 20);
    l.ang = frand(0, 6.2832f);
    l.vel = frand(30, 75);
    l.fase = frand(0, 6.2832f);
    l.fv = frand(1.2f, 2.8f);
    l.h = (uint8_t)(38 + irand(40));   // amarelo a verde-limao
    l.ox = l.x; l.oy = l.y;
  }
}

static void lumes_passo(float dt)
{
  for (int i = 0; i < 16; ++i) {
    Lume& l = U.lumes.l[i];
    l.ox = l.x; l.oy = l.y;
    l.ang += frand(-4, 4) * dt;
    l.x += cosf(l.ang) * l.vel * dt;
    l.y += sinf(l.ang) * l.vel * dt;
    if (l.x < s_ax + 12)           { l.x = s_ax + 12;           l.ang = 3.1416f - l.ang; }
    else if (l.x > s_ax + s_W - 12) { l.x = s_ax + s_W - 12;     l.ang = 3.1416f - l.ang; }
    if (l.y < s_ay + 12)           { l.y = s_ay + 12;           l.ang = -l.ang; }
    else if (l.y > s_ay + s_H - 12) { l.y = s_ay + s_H - 12;     l.ang = -l.ang; }
    l.fase += l.fv * dt;
    rec2(l.ox, l.oy, l.x, l.y, 10);
  }
}

static void lumes_draw(lgfx::LovyanGFX& d)
{
  for (int i = 0; i < 16; ++i) {
    const Lume& l = U.lumes.l[i];
    float b = 0.5f + 0.5f * sinf(l.fase);
    b *= b;
    const int v = (int)(255 * b);
    if (v < 10) { continue; }
    d.fillCircle((int)l.x, (int)l.y, 8, matiz(l.h, v * 18 / 100));
    d.fillCircle((int)l.x, (int)l.y, 5, matiz(l.h, v * 45 / 100));
    d.fillCircle((int)l.x, (int)l.y, 2, c565(v, v, v * 6 / 10));
  }
}

// ===========================================================================
// PENDULOS
// ===========================================================================
// Ball i makes (K + i) back and forth every CYCLE seconds: at the end of cycle
// todas voltam juntas, e no middle passam pelas figuras.
static const float PEND_CICLO = 60.0f;
static const int   PEND_K     = 12;
static const int   PEND_R     = 12;

static float pend_x(int i, float t)
{
  const float amp = s_W * 0.5f - PEND_R - 18;
  return s_ax + s_W * 0.5f + amp * sinf(6.2832f * (PEND_K + i) * t / PEND_CICLO);
}
static float pend_y(int i)
{
  return s_ay + 50 + (float)i * (s_H - 100) / (N_PEND - 1);
}

static void pendulos_start(void)
{
  Pendulos& s = U.pendulos;
  s.t = 0;   // todas juntas, comecando a se separar
  for (int i = 0; i < N_PEND; ++i) { s.ox[i] = pend_x(i, 0); }
}

static void pendulos_passo(float dt)
{
  Pendulos& s = U.pendulos;
  s.t += dt;
  if (s.t >= PEND_CICLO) { s.t -= PEND_CICLO; }
  for (int i = 0; i < N_PEND; ++i) {
    const float x = pend_x(i, s.t), y = pend_y(i);
    rec2(s.ox[i], y, x, y, PEND_R + 2);
    s.ox[i] = x;
  }
}

static void pendulos_draw(lgfx::LovyanGFX& d)
{
  const Pendulos& s = U.pendulos;
  for (int i = 0; i < N_PEND; ++i) {
    const int x = (int)s.ox[i], y = (int)pend_y(i);
    d.fillCircle(x, y, PEND_R, matiz((uint8_t)(i * 13), 230));
    d.fillCircle(x - 4, y - 4, 3, c565(240, 240, 240));
  }
}

// ===========================================================================
// NEVE
// ===========================================================================
static void floco_novo(Floco& f, bool espalhar)
{
  const int r = irand(10);
  f.size  = (uint8_t)(r < 5 ? 1 : (r < 8 ? 2 : 3));   // muitos pequenos, poucos grandes
  f.vy   = frand(30, 50) * f.size;
  f.x    = frand(s_ax, s_ax + s_W);
  f.y    = espalhar ? frand(s_ay, s_ay + s_H) : s_ay - frand(4, 60);
  f.fase = frand(0, 6.2832f);
  f.ox = f.x; f.oy = f.y;
}

static void neve_start(void)
{
  Neve& s = U.neve;
  for (int i = 0; i < N_FLOCO; ++i) { floco_novo(s.f[i], true); }
  s.tv = frand(0, 20);
}

static void neve_passo(float dt)
{
  Neve& s = U.neve;
  s.tv += dt;
  const float vento = 45.0f * sinf(s.tv * 0.23f) + 15.0f * sinf(s.tv * 0.71f);
  for (int i = 0; i < N_FLOCO; ++i) {
    Floco& f = s.f[i];
    const float ox = f.x, oy = f.y;
    f.fase += 1.7f * dt;
    f.x += (vento * f.size / 3.0f + 18.0f * sinf(f.fase)) * dt;
    f.y += f.vy * dt;
    bool pulou = false;
    if (f.x < s_ax - 4)            { f.x += s_W + 8; pulou = true; }
    else if (f.x > s_ax + s_W + 4) { f.x -= s_W + 8; pulou = true; }
    if (f.y > s_ay + s_H + 4) {
      rec2(ox, oy, ox, oy, 4);
      floco_novo(f, false);
      continue;
    }
    if (pulou) { rec2(ox, oy, ox, oy, 4); rec2(f.x, f.y, f.x, f.y, 4); }
    else       { rec2(ox, oy, f.x, f.y, 4); }
  }
}

static void neve_draw(lgfx::LovyanGFX& d)
{
  const Neve& s = U.neve;
  for (int i = 0; i < N_FLOCO; ++i) {
    const Floco& f = s.f[i];
    const int x = (int)f.x, y = (int)f.y;
    switch (f.size) {
      case 1:
        d.fillRect(x, y, 2, 2, c565(150, 160, 185));
        break;
      case 2: {
        const uint16_t c = c565(200, 215, 240);
        d.fillRect(x - 2, y, 5, 1, c);
        d.fillRect(x, y - 2, 1, 5, c);
      } break;
      default: {
        const uint16_t c = c565(235, 245, 255);
        d.fillRect(x - 3, y, 7, 1, c);
        d.fillRect(x, y - 3, 1, 7, c);
        d.drawLine(x - 2, y - 2, x + 2, y + 2, c);
        d.drawLine(x - 2, y + 2, x + 2, y - 2, c);
      } break;
    }
  }
}

// ===========================================================================
// COMETAS
// ===========================================================================
static const float CAUDA_PASSO = 0.035f;   // "time" entre dois pontos of cauda

static void cometa_pos(const Cometa& c, float t, float* x, float* y)
{
  *x = s_ax + s_W * 0.5f + c.rx * sinf(c.a * t + c.fase);
  *y = s_ay + s_H * 0.5f + c.ry * sinf(c.b * t);
}

static int cauda_raio(int k) { return k < 2 ? 6 - k : (k < 6 ? 3 : 2); }

static void cometas_start(void)
{
  static const uint8_t PARES[][2] = { { 3, 2 }, { 5, 4 }, { 4, 3 }, { 2, 3 }, { 5, 3 }, { 3, 4 } };
  for (int i = 0; i < N_COMETA; ++i) {
    Cometa& c = U.cometas.c[i];
    const int p = irand(6);
    c.a    = PARES[p][0];
    c.b    = PARES[p][1];
    c.fase = frand(0, 6.2832f);
    c.vel  = frand(0.22f, 0.34f);
    c.t    = frand(0, 60);
    c.rx   = s_W * 0.5f - 24;
    c.ry   = s_H * 0.5f - 24;
    c.h    = (uint8_t)(i * 85 + irand(30));
    for (int k = 0; k < N_CAUDA; ++k) { cometa_pos(c, c.t - k * CAUDA_PASSO, &c.ox[k], &c.oy[k]); }
  }
}

static void cometas_passo(float dt)
{
  for (int i = 0; i < N_COMETA; ++i) {
    Cometa& c = U.cometas.c[i];
    c.t    += c.vel * dt;
    c.fase += 0.02f * dt;   // a curva vai mudando devagar
    for (int k = 0; k < N_CAUDA; ++k) {
      float x, y;
      cometa_pos(c, c.t - k * CAUDA_PASSO, &x, &y);
      rec2(c.ox[k], c.oy[k], x, y, cauda_raio(k) + 2);
      c.ox[k] = x; c.oy[k] = y;
    }
  }
}

static void cometas_draw(lgfx::LovyanGFX& d)
{
  for (int i = 0; i < N_COMETA; ++i) {
    const Cometa& c = U.cometas.c[i];
    for (int k = N_CAUDA - 1; k >= 0; --k) {   // the head last, by top
      const int x = (int)c.ox[k], y = (int)c.oy[k], r = cauda_raio(k);
      const uint16_t color = k == 0 ? c565(255, 250, 235)
                                  : matiz((uint8_t)(c.h + k * 3), 250 - k * 17);
      if (r >= 3) { d.fillCircle(x, y, r, color); }
      else        { d.fillRect(x - r, y - r, 2 * r, 2 * r, color); }
    }
  }
}

// ===========================================================================
// SISTEMA SOLAR
// ===========================================================================
static const int SOL_R = 30;

static void solar_start(void)
{
  Solar& s = U.solar;
  s.cx = s_ax + s_W / 2;
  s.cy = s_ay + (int)(s_H * 0.6f);   // abaixo of clock
  float rmax = fminf(s_W * 0.5f, s_H * 0.4f) - 22;
  if (rmax < 120) { rmax = 120; }
  static const float   ORB[N_PLANETA] = { 0.21f, 0.34f, 0.49f, 0.64f, 0.81f, 1.0f };
  static const int16_t RAD[N_PLANETA] = { 5, 8, 9, 6, 13, 10 };
  static const uint8_t COR[N_PLANETA][3] = {
    { 170, 160, 150 }, { 235, 180, 90 }, { 70, 140, 255 }, { 230, 90, 60 }, { 220, 190, 130 }, { 120, 220, 230 } };
  for (int i = 0; i < N_PLANETA; ++i) {
    Planeta& p = s.p[i];
    p.orb = rmax * ORB[i];
    if (p.orb < SOL_R + 16 + RAD[i]) { p.orb = SOL_R + 16 + RAD[i]; }
    p.ang = frand(0, 6.2832f);
    p.w   = 1.4f * powf(70.0f / p.orb, 1.5f);   // Kepler: longe e mais devagar
    p.r   = RAD[i];
    p.color = c565(COR[i][0], COR[i][1], COR[i][2]);
    p.ox  = s.cx + cosf(p.ang) * p.orb;
    p.oy  = s.cy + sinf(p.ang) * p.orb;
  }
  s.t = 0;
  s.lua = 0;
  s.lox = s.p[2].ox; s.loy = s.p[2].oy;
}

static void solar_fundo(lgfx::LGFX_Sprite& sp, int W, int H)
{
  background_ceu(sp, W, H);
  const Solar& s = U.solar;
  for (int i = 0; i < N_PLANETA; ++i) {
    sp.drawCircle(s.cx - s_ax, s.cy - s_ay, (int)s.p[i].orb, c565(38, 40, 58));
  }
}

static void solar_passo(float dt)
{
  Solar& s = U.solar;
  s.t += dt;
  // O sol pulsa: a caixa dele (com a coroa) se refaz todo frame.
  rec(s.cx - SOL_R - 7, s.cy - SOL_R - 7, 2 * SOL_R + 15, 2 * SOL_R + 15);
  for (int i = 0; i < N_PLANETA; ++i) {
    Planeta& p = s.p[i];
    p.ang += p.w * dt;
    if (p.ang > 6.2832f) { p.ang -= 6.2832f; }
    const float x = s.cx + cosf(p.ang) * p.orb, y = s.cy + sinf(p.ang) * p.orb;
    const int folga = p.r + (i == 4 ? 12 : 2);   // o anel passa of planeta
    rec2(p.ox, p.oy, x, y, folga);
    p.ox = x; p.oy = y;
  }
  // A lua of terra (planeta 2).
  s.lua += 3.2f * dt;
  const float lx = s.p[2].ox + cosf(s.lua) * 18, ly = s.p[2].oy + sinf(s.lua) * 18;
  rec2(s.lox, s.loy, lx, ly, 5);
  s.lox = lx; s.loy = ly;
}

static void solar_draw(lgfx::LovyanGFX& d)
{
  const Solar& s = U.solar;
  const int pulso = (int)(2.5f * sinf(s.t * 2.2f));
  d.fillCircle(s.cx, s.cy, SOL_R + 5 + pulso, c565(110, 40, 0));
  d.fillCircle(s.cx, s.cy, SOL_R + pulso, c565(255, 150, 30));
  d.fillCircle(s.cx, s.cy, SOL_R * 6 / 10, c565(255, 225, 120));
  for (int i = 0; i < N_PLANETA; ++i) {
    const Planeta& p = s.p[i];
    const int x = (int)p.ox, y = (int)p.oy;
    if (i == 4) { d.drawEllipse(x, y, p.r + 9, 4, c565(170, 150, 110)); }
    d.fillCircle(x, y, p.r, p.color);
    if (i == 4) { d.drawFastHLine(x - p.r + 2, y, 2 * p.r - 4, c565(170, 150, 110)); }
  }
  d.fillCircle((int)s.lox, (int)s.loy, 3, c565(200, 200, 205));
}

// ===========================================================================
// LOGO
// ===========================================================================
static void logo_paint(Logo& s)
{
  if (!s_logo) { return; }
  const uint16_t color = matiz(s.color, 255);
  s_logo->fillScreen(TFT_BLACK);
  s_logo->drawRoundRect(0, 0, s.w, s.h, 12, color);
  s_logo->drawRoundRect(1, 1, s.w - 2, s.h - 2, 11, color);
  tab5_fonte_body(*s_logo);
  tab5_text(*s_logo, 5.0f);
  s_logo->setTextDatum(middle_center);
  s_logo->setTextColor(color, TFT_BLACK);
  s_logo->drawString("M5tabOS", s.w / 2, s.h / 2);
}

static void logo_start(void)
{
  Logo& s = U.logo;
  s.w = 230; s.h = 76;
  if (!s_logo) {
    s_logo = new lgfx::LGFX_Sprite();
    s_logo->setColorDepth(16);
    s_logo->setPsram(true);
    if (!s_logo->createSprite(s.w, s.h)) { delete s_logo; s_logo = nullptr; }
  }
  s.x = frand(s_ax, s_ax + s_W - s.w);
  s.y = frand(s_ay, s_ay + s_H - s.h);
  const float v = 170;
  s.vx = (irand(2) ? v : -v) * frand(0.8f, 1.1f);
  s.vy = (irand(2) ? v : -v) * frand(0.8f, 1.1f);
  s.ox = s.x; s.oy = s.y;
  s.color = (uint8_t)irand(256);
  s.cantos = 0;
  logo_paint(s);
}

static void logo_passo(float dt)
{
  Logo& s = U.logo;
  s.ox = s.x; s.oy = s.y;
  s.x += s.vx * dt;
  s.y += s.vy * dt;
  int bateu = 0;
  if (s.x < s_ax)                { s.x = (float)s_ax;              s.vx =  fabsf(s.vx); ++bateu; }
  else if (s.x > s_ax + s_W - s.w) { s.x = (float)(s_ax + s_W - s.w); s.vx = -fabsf(s.vx); ++bateu; }
  if (s.y < s_ay)                { s.y = (float)s_ay;              s.vy =  fabsf(s.vy); ++bateu; }
  else if (s.y > s_ay + s_H - s.h) { s.y = (float)(s_ay + s_H - s.h); s.vy = -fabsf(s.vy); ++bateu; }
  if (bateu) {
    s.color = (uint8_t)(s.color + 45 + irand(40));
    s.repintar = true;
    if (bateu == 2) { ++s.cantos; }   // o canto! (stays so in memoria, of trofeu)
  }
  const int x0 = (int)fminf(s.ox, s.x) - 1, y0 = (int)fminf(s.oy, s.y) - 1;
  rec(x0, y0, s.w + (int)ceilf(fabsf(s.x - s.ox)) + 3, s.h + (int)ceilf(fabsf(s.y - s.oy)) + 3);
}

static void logo_draw(lgfx::LovyanGFX& d)
{
  Logo& s = U.logo;
  if (s.repintar) { logo_paint(s); s.repintar = false; }
  if (s_logo) {
    s_logo->pushSprite(&d, (int)s.x, (int)s.y, (uint16_t)0);   // preto = transparente
  } else {
    d.drawRoundRect((int)s.x, (int)s.y, s.w, s.h, 12, matiz(s.color, 255));
  }
}

// ===========================================================================
// O QUADRO
// ===========================================================================
typedef struct {
  void (*start)(void);
  void (*passo)(float dt);
  void (*draw)(lgfx::LovyanGFX& d);
  void (*fundo)(lgfx::LGFX_Sprite& s, int W, int H);   // nullptr = abstrato sorteado
} anim_t;

static const anim_t ANIMS[] = {
  { bolas_start,    bolas_passo,    bolas_draw,    nullptr     },
  { fogos_start,    fogos_passo,    fogos_draw,    nullptr     },
  { chuva_start,    chuva_passo,    chuva_draw,    nullptr     },
  { estrelas_start, estrelas_passo, estrelas_draw, nullptr     },
  { bolhas_start,   bolhas_passo,   bolhas_draw,   nullptr     },
  { cobra_start,    cobra_passo,    cobra_draw,    nullptr     },
  { lumes_start,    lumes_passo,    lumes_draw,    nullptr     },
  { pendulos_start, pendulos_passo, pendulos_draw, nullptr     },
  { neve_start,     neve_passo,     neve_draw,     nullptr     },
  { cometas_start,  cometas_passo,  cometas_draw,  nullptr     },
  { solar_start,    solar_passo,    solar_draw,    solar_fundo },
  { logo_start,     logo_passo,     logo_draw,     nullptr     },
};
static const int N_ANIMS = (int)(sizeof(ANIMS) / sizeof(ANIMS[0]));

static void (* const FUNDOS[])(lgfx::LGFX_Sprite&, int, int) = {
  background_fios, background_ondas, background_teia, background_raios, background_circulos,
};

static const int N_FUNDOS = (int)(sizeof(FUNDOS) / sizeof(FUNDOS[0]));

static int  s_current = 0;
static int  s_fundo_current = 0;
static bool s_pronta = false;   // a sorteada ja foi iniciada

// O dt vem of ritmo of frame of sistema (kernel/frame_pacing.hpp): o time entre as
// scans where two frames go live. Limited here: after a
// pausa (despertar), a animacao continua of onde estava em vez of saltar.
static float passo_dt(void)
{
  float dt = system_frame_dt();
  if (dt < 0.005f) { dt = 0.005f; }
  if (dt > 0.10f)  { dt = 0.10f; }
  return dt;
}

void bq_anim_sortear(void)
{
  s_rng ^= esp_random();
  if (s_rng == 0) { s_rng = 2463534242u; }
  int nova = irand(N_ANIMS - 1);
  if (nova >= s_current) { ++nova; }   // nunca a mesma duas vezes seguidas
  int fundo = irand(N_FUNDOS - 1);
  if (fundo >= s_fundo_current) { ++fundo; }
  s_current       = nova;
  s_fundo_current = fundo;
  s_pronta      = false;
  s_fundo_falhou = false;
  background_release();   // new drawing with each block
  if (ANIMS[s_current].start != logo_start) { delete s_logo; s_logo = nullptr; }
}

void bq_anim_release(void)
{
  background_release();
  delete s_logo;
  s_logo = nullptr;
  s_pronta = false;   // o logo (se for ele) precisa of imagem of iteration
}

// Cria e pinta a imagem of fundo. false = sem memoria (stays o preto).
static bool background_create(void)
{
  auto* sp = new lgfx::LGFX_Sprite();
  if (!sp) { return false; }
  // O MESMO formato das paginas of panel: a copia vira memcpy puro.
  sp->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
  sp->setPsram(true);
  if (!sp->createSprite(s_W, s_H)) { delete sp; return false; }
  sp->fillScreen(TFT_BLACK);
  if (ANIMS[s_current].fundo) { ANIMS[s_current].fundo(*sp, s_W, s_H); }
  else                      { FUNDOS[s_fundo_current](*sp, s_W, s_H); }
  s_fundo = sp;
  return true;
}

void bq_anim_start(const area_t& a)
{
  const bool changed_area = a.x != s_ax || a.y != s_ay || a.width != s_W || a.height != s_H;
  s_ax = a.x; s_ay = a.y; s_W = a.width; s_H = a.height;
  s_n_ret  = 0;
  if (!s_pronta || changed_area) {
    ANIMS[s_current].start();
    s_pronta = true;
    background_release();
  }
  if (!s_fundo && !s_fundo_falhou) { s_fundo_falhou = !background_create(); }
  s_fundo_paint = s_fundo != nullptr;
  s_everything = true;   // a screen foi limpa: everything precisa ser pintado
}

void bq_anim_frame(lgfx::LovyanGFX& d, const bq_ret_t* extras, int n_extras,
                    const bq_ret_t* caixas, int n_caixas, bq_ret_t* clips)
{
  const float dt = passo_dt();

  s_n_ret = 0;
  for (int i = 0; i < n_extras; ++i) { rec(extras[i].x, extras[i].y, extras[i].w, extras[i].h); }
  if (n_extras > 0) { s_everything = true; }

  ANIMS[s_current].passo(dt);

  // WHERE TEXT NEEDS TO BE REPAINTED: for each box of text, the box that
  // covers everything that the animation moved inside it. This ENTIRE box goes in
  // lista of erase. O text e suavizado (borda translucida): pintado duas
  // times in the same pixel it thickens with each frame. Deleting the entire box
  // before, each pixel receives the text once.
  //
  // Em passes: a juncao das caixas (rec) pode crescer uma delas por top of
  // another text. Each pass recreates the clip of each text with EVERYTHING that will be
  // is deleted inside it, and the clip itself goes into the list - until stop of changing.
  for (int j = 0; j < n_caixas; ++j) { clips[j] = { 0, 0, 0, 0 }; }
  for (int pass = 0; pass < 4; ++pass) {
    bool changed = false;
    for (int j = 0; j < n_caixas; ++j) {
      const bq_ret_t& cx = caixas[j];
      bq_ret_t env = { 0, 0, 0, 0 };
      for (int i = 0; i < s_n_ret; ++i) {
        const bq_ret_t& r = s_ret[i];
        const int ix0 = r.x > cx.x ? r.x : cx.x, iy0 = r.y > cx.y ? r.y : cx.y;
        const int ix1 = r.x + r.w < cx.x + cx.w ? r.x + r.w : cx.x + cx.w;
        const int iy1 = r.y + r.h < cx.y + cx.h ? r.y + r.h : cx.y + cx.h;
        if (ix1 <= ix0 || iy1 <= iy0) { continue; }
        const bq_ret_t c = { (int16_t)ix0, (int16_t)iy0, (int16_t)(ix1 - ix0), (int16_t)(iy1 - iy0) };
        env = env.w > 0 ? envolver(env, c) : c;
      }
      if (env.w <= 0 || memcmp(&env, &clips[j], sizeof(env)) == 0) { continue; }
      clips[j] = env;
      rec(env.x, env.y, env.w, env.h);
      s_everything = true;   // o que estava nessa caixa foi apagado: everything se redesenha
      changed = true;
    }
    if (!changed) { break; }
  }

  if (s_fundo_paint) {
    background_paint_everything(d);
    s_fundo_paint = false;
  }
  for (int i = 0; i < s_n_ret; ++i) { erase(d, s_ret[i]); }
  d.setClipRect(s_ax, s_ay, s_W, s_H);   // nunca encosta nas barras
  ANIMS[s_current].draw(d);
  d.clearClipRect();
  s_everything = false;
}

// ---------------------------------------------------------------------------
// O MESMO DESENHO COMO PAPEL DE PAREDE (home). Declarada no .hpp. A imagem
// so existe durante a chamada: pintada, copiada para a screen e devolvida - a
// home so e desenhada inteira (open, switch of pagina, voltar of one app),
// so there is no reason to hold 1.6 MB for the entire team. The same seed does the
// drawing exit the same every time.
// O PAPEL DE PAREDE GUARDADO. Gerar o drawing procedural num sprite of
// size of area (~1.6 MB) every pass complete of home - todo close of
// app, todo despertar - era o mesmo work, com o mesmo resultado (mesma
// semente), repetido in transicao mais pesada of sistema. Agora o sprite
// pronto stays enquanto estilo/semente/area forem os mesmos; o kernel released
// on open one app and no sleep, when a home is not on screen.
static lgfx::LGFX_Sprite* s_parede = nullptr;
static int      s_parede_estilo = -1;
static uint32_t s_parede_semente = 0;
static int      s_parede_l = 0, s_parede_a = 0;

void bq_parede_release(void)
{
  delete s_parede;
  s_parede = nullptr;
  s_parede_estilo = -1;
}

bool bq_fundo_parede(lgfx::LovyanGFX& d, const area_t& a, int estilo, uint32_t semente)
{
  if (estilo < 0 || estilo >= N_FUNDOS) { return false; }
  if (a.width <= 0 || a.height <= 0) { return false; }
  const bool mesma = s_parede && s_parede_estilo == estilo && s_parede_semente == semente
                  && s_parede_l == a.width && s_parede_a == a.height;
  if (!mesma) {
    bq_parede_release();
    auto* sp = new lgfx::LGFX_Sprite();
    if (!sp) { return false; }
    sp->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    sp->setPsram(true);
    if (!sp->createSprite(a.width, a.height)) { delete sp; return false; }
    sp->fillScreen(TFT_BLACK);
    // The seed is of wallpaper, not of animation: the common generator is
    // emprestado e devolvido como estava.
    const uint32_t rng = s_rng;
    s_rng = semente ? semente : 2463534242u;
    FUNDOS[estilo](*sp, a.width, a.height);
    s_rng = rng;
    s_parede = sp;
    s_parede_estilo = estilo; s_parede_semente = semente;
    s_parede_l = a.width; s_parede_a = a.height;
  }

  // IN BANDS, like all large writing (see tab5_screen_clear): the image
  // entire of once the FIFO of panel dries. And covered: the composer does not
  // precisa trazer of pagina of frente o que vai ser sobrescrito.
  const int FAIXA = 128;
  for (int y = 0; y < a.height; y += FAIXA) {
    const int h = (y + FAIXA <= a.height) ? FAIXA : a.height - y;
    virtual_screen_cover(a.x, a.y + y, a.width, h);
    d.setClipRect(a.x, a.y + y, a.width, h);
    s_parede->pushSprite(&d, a.x, a.y);
  }
  d.clearClipRect();
  return true;
}
