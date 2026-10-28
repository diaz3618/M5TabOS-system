// APP: ESP-NOW - a window of servico services/tab5_espnow.
//
// Uma barra fina of informacoes em top (estado, canal, MAC, quem ja foi
// ouvido) e a conversa embaixo - baloes, touch para write, keyboard. A
// conversa e o componente of sistema (kernel/conversation): este arquivo so diz
// QUAIS mensagens existem e COMO send. O radio, o historico no cartao e o
// aviso com o app closed sao of servico.
//
// Envio em DIFUSAO: todo aparelho no mesmo canal ouve (o Cardputer com o
// firmware oficial of ESP-NOW, por exemplo). Em difusao nao existe ACK - o
// visto verde diz "exited pela antena", nao "alguem recebeu".
#include "app.h"
#include "conversation.hpp"
#include "paint.hpp"
#include "tab5_espnow.hpp"
#include "tab5_wifi.hpp"
#include "tab5_ui.hpp"
#include "tab5_screen.hpp"
#include <cstdio>
#include <cstring>
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR

// Na PSRAM: ~1 KB (rascunho, copia dele para o "changed", keyboard) que so a
// tarefa of interface toca, e nao precisa morar in RAM interna com o app closed.
EXT_RAM_BSS_ATTR static conversation_t s_c;
static paint_t  s_p;
static char       s_barra_txt[128];
static int        s_barra_estado = -1;

// 88 px, "ESP-NOW" em 3.6 e a pilula em 3.2 (era 56/2.2, depois 72/3.0 -
// pequeno demais para read a distancia of uso).
static const int BARRA_A = 88;

// O SELETOR DE CANAL, in barra: cada touch passa para o next. 0 = o of
// roteador; 1, 6 e 11 sao os canais que nao se sobrepoem em 2,4 GHz - e os
// padroes of maioria dos aparelhos (one ESP32 sozinho stays no 1).
static const uint8_t CANAIS[] = { 0, 1, 6, 11 };
static int s_pilula_x0 = -1, s_pilula_x1 = -1;   // onde ela foi desenhada (x of area)

// Cor estavel por aparelho: o mesmo MAC tem sempre a mesma color no rotulo, e
// numa mesa com tres Cardputers of para saber quem e quem of relance.
static uint16_t color_of_mac(const uint8_t mac[6])
{
  static const uint16_t CORES[] = { TFT_SKYBLUE, TFT_ORANGE, TFT_GREENYELLOW, TFT_PINK,
                                    TFT_GOLD, TFT_CYAN, TFT_VIOLET, TFT_SILVER };
  const unsigned h = (unsigned)mac[3] * 31u + (unsigned)mac[4] * 7u + (unsigned)mac[5];
  return CORES[h % (sizeof(CORES) / sizeof(CORES[0]))];
}

// ---- o que o componente of conversa pergunta -------------------------------
static int count(void*) { return tab5_espnow_n(); }

static bool read(int i, conversation_msg_t* out, void*)
{
  static espnow_msg_t m, ant;   // fora of stack of interface
  if (!tab5_espnow_msg(i, &m)) { return false; }
  snprintf(out->text, sizeof(out->text), "%s", m.text);
  out->minha  = m.minha;
  out->when = m.when;
  out->send_state  = m.send_state;   // espnow_envio_t e conversa_envio_t tem a mesma order
  out->name[0] = 0;
  // Rotulo so when o remetente muda - uma sequencia of mesmo aparelho nao
  // repete o name em cada bubble.
  if (!m.minha) {
    const bool mesmo = (i > 0) && tab5_espnow_msg(i - 1, &ant) && !ant.minha
                    && memcmp(ant.mac, m.mac, 6) == 0;
    if (!mesmo) {
      snprintf(out->name, sizeof(out->name), "%02X%02X", m.mac[4], m.mac[5]);
      out->color_name = color_of_mac(m.mac);
    }
  }
  // O DIALETO of cada recebida, pequeno embaixo: com Cardputer, StickC e Basic
  // in mesa, diz quem fala o formato of componente esp-now e quem manda text puro.
  out->meta[0] = 0;
  if (!m.minha) {
    snprintf(out->meta, sizeof(out->meta), "%s", m.espressif ? "esp-now Espressif" : "texto puro");
    out->color_meta = m.espressif ? TFT_SKYBLUE : TFT_DARKGREY;
  }
  return true;
}

static bool send(const char* text, void*) { return tab5_espnow_send(text); }

// ---- barra of informacoes ---------------------------------------------------
// 0 = sem o firmware nosso no C6, 1 = C6 sem enlace (WiFi nunca enabled neste
// boot), 2 = ligando, 3 = no ar, 4 = radio parado (WiFi disabled em Ajustes,
// ou a desistencia fora of alcance of roteador) - o enlace segue of pe, o radio nao.
static int estado_now(void)
{
  if (!tab5_espnow_c6_compativel()) {
    return tab5_wifi_c6_link() ? 0 : 1;   // sem enlace ainda nao of para saber o firmware
  }
  if (!tab5_wifi_c6_link()) { return 1; }
  if (!tab5_wifi_radio_active()) { return 4; }
  return tab5_espnow_pronto() ? 3 : 2;
}

static const char* reason(int est)
{
  switch (est) {
    case 0:  return "factory C6 radio: ESP-NOW needs an update (optional)";
    case 1:  return "turn on Wi-Fi in Settings (the C6 only gets power with it)";
    case 2:  return "ligando o ESP-NOW...";
    case 4:  return "radio stopped - turn on Wi-Fi or fix a channel in Settings";
    default: return "";
  }
}

static void draw_barra(lgfx::LovyanGFX& d, const area_t& a, int est)
{
  const uint8_t fixo = tab5_espnow_canal_fixo();
  char canal[32];
  // Fixo: o canal que o C6 DISSE que esta (o servico confere) - se ele
  // recusou a troca, a pilula mostra o request e o real, em vez of mentir.
  const uint8_t real = tab5_espnow_canal();
  // Em pe a barra tem 720 px: a pilula vai curta. "fixo" x "roteador" continua
  // dito pela COR dela (laranja = fixo), so sem a palavra.
  const bool pe = a.width < a.height;
  if (fixo && est == 3 && real && real != fixo) {
    if (pe) { snprintf(canal, sizeof(canal), "ch %u? (%u)", (unsigned)real, (unsigned)fixo); }
    else    { snprintf(canal, sizeof(canal), "canal %u?  fixo %u", (unsigned)real, (unsigned)fixo); }
  } else if (fixo) { snprintf(canal, sizeof(canal), pe ? "ch %u" : "canal %u  fixo", (unsigned)fixo); }
  else if (est == 3) { snprintf(canal, sizeof(canal), pe ? "ch %u" : "canal %u  roteador", (unsigned)tab5_espnow_canal()); }
  else { snprintf(canal, sizeof(canal), pe ? "ch auto" : "canal: roteador"); }

  char txt[80];
  if (est == 3) {
    uint8_t mac[6];
    tab5_espnow_meu_mac(mac);
    const uint32_t ms = tab5_espnow_ms_since_rx();
    char ultimo[24];
    if (ms == UINT32_MAX)  { snprintf(ultimo, sizeof(ultimo), "nada ouvido"); }
    else if (ms < 60000)   { snprintf(ultimo, sizeof(ultimo), "last %lus", (unsigned long)(ms / 1000)); }
    else                   { snprintf(ultimo, sizeof(ultimo), "last %lumin", (unsigned long)(ms / 60000)); }
    // EM PE NAO CABE O MAC. Depois of titulo e of pilula sobram ~150 px, e
    // a line inteira encolhida ate o minimum ainda passava of borda. O MAC e
    // o dado que menos muda (e o of proprio Tab5): exit ele, stays o que diz
    // se ha alguem por perto.
    if (pe) {
      snprintf(txt, sizeof(txt), "%d ouvido(s)  %s", tab5_espnow_vistos(), ultimo);
    } else {
      snprintf(txt, sizeof(txt), "%02X:%02X:%02X:%02X:%02X:%02X   %d ouvido(s)   %s",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], tab5_espnow_vistos(), ultimo);
    }
  } else {
    snprintf(txt, sizeof(txt), "%s", est == 2 ? "turning on..." : est == 4 ? "radio stopped" : "offline");
  }

  char everything[128];
  snprintf(everything, sizeof(everything), "%s|%s", canal, txt);
  const bool m1 = paint_changed_i(&s_p, &s_barra_estado, est);
  const bool m2 = paint_changed_s(&s_p, s_barra_txt, sizeof(s_barra_txt), everything);
  if (!m1 && !m2) { return; }

  const uint16_t FUNDO = d.color565(13, 17, 23);
  tab5_screen_clear(a.x, a.y, a.width, BARRA_A, FUNDO);
  const int cy = a.y + BARRA_A / 2;
  const uint16_t color = (est == 3) ? TFT_GREENYELLOW : (est == 2) ? TFT_ORANGE : TFT_DARKGREY;
  d.fillCircle(a.x + 34, cy, 15, color);

  d.setTextDatum(middle_left);
  tab5_text(d, 3.6f);
  d.setTextColor(TFT_WHITE, FUNDO);
  d.drawString("ESP-NOW", a.x + 64, cy);
  int x = a.x + 64 + (int)d.textWidth("ESP-NOW") + 28;

  // Pilula of canal: e one botao (touch troca), entao tem cara of botao.
  tab5_text(d, 3.2f);
  const int pl = (int)d.textWidth(canal) + 44;
  const int pa = BARRA_A - 22;
  const uint16_t color_p = fixo ? d.color565(120, 70, 10) : d.color565(38, 60, 90);
  d.fillRoundRect(x, cy - pa / 2, pl, pa, pa / 2, color_p);
  d.setTextColor(TFT_WHITE, color_p);
  d.drawString(canal, x + 22, cy);
  s_pilula_x0 = x - a.x;
  s_pilula_x1 = x - a.x + pl;
  x += pl + 24;

  // O resto (MAC, ouvidos, ultimo) no MAIOR tamanho que caiba no que sobrou -
  // measured, nao chutado: em retrato ou com MAC comprido, encolhe sozinho em
  // vez of passar of borda.
  const int sobra = a.x + a.width - 16 - x;
  float size = 3.2f;
  tab5_text(d, size);
  while (size > 1.8f && (int)d.textWidth(txt) > sobra) {
    size -= 0.2f;
    tab5_text(d, size);
  }
  // No menor tamanho e ainda sem caber: corta no end, por caractere (UTF-8),
  // em vez of passar of borda of screen.
  tab5_cortar_on_caber(d, txt, sobra);
  d.setTextColor(TFT_LIGHTGREY, FUNDO);
  d.drawString(txt, x, cy);
  d.drawFastHLine(a.x, a.y + BARRA_A - 1, a.width, d.color565(38, 42, 50));
  d.setTextDatum(top_left);
}

// ---- icon of screen inicial ------------------------------------------------
// Na line of marca of Espressif (dona of ESP-NOW, que nao tem logotipo
// proprio - a pagina oficial usa a marca of empresa): o vermelho dela, e em
// branco o ponto of origem com tres ondas abrindo para top e para a right -
// o sinal saindo of one aparelho, sem roteador no middle. Tudo proporcional on
// lado: o mesmo drawing serve em qualquer tamanho of grade.
static void marca_espnow(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t vermelho = d.color565(231, 53, 44);   // o vermelho of Espressif
  d.fillRoundRect(x, y, lado, lado, lado / 6, vermelho);

  const int cx = x + (lado * 28) / 100;
  const int cy = y + (lado * 72) / 100;
  int grosso = (lado * 7) / 100;
  if (grosso < 3) { grosso = 3; }

  d.fillCircle(cx, cy, (lado * 8) / 100, TFT_WHITE);
  // 270..360 graus = o quadrante of cima-direita (y cresce para bottom).
  static const int RAIO[3] = { 24, 40, 56 };   // % of lado, ate a borda of fora of onda
  for (int k = 0; k < 3; ++k) {
    const int r1 = (lado * RAIO[k]) / 100;
    d.fillArc(cx, cy, r1 - grosso, r1, 270, 360, TFT_WHITE);
  }
}

// ---- ciclo of vida ---------------------------------------------------------
static bool on_open(void)
{
  s_barra_estado = -1;
  s_barra_txt[0] = 0;
  conversation_open(&s_c, &s_p, count, read, send, nullptr);
  tab5_espnow_marcar_lidas();
  tab5_espnow_window(true);    // conversa in frente: o servico pede o receptor enabled
  return true;
}

static void on_close(void) { tab5_espnow_window(false); conversation_close(&s_c); }

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  paint_start(&s_p);
  const int est = estado_now();
  draw_barra(d, a, est);
  const area_t r = { a.x, a.y + BARRA_A, a.width, a.height - BARRA_A };
  conversation_draw(&s_c, d, r, paint_complete(&s_p), est == 3, reason(est));
}

static void on_aviso(uint32_t)
{
  // Com a window open, o que chega ja nasce lido.
  tab5_espnow_marcar_lidas();
  conversation_changed(&s_c);
}

static void on_touch(int x, int y)
{
  // A pilula of canal passa on next: roteador -> 1 -> 6 -> 11 -> roteador.
  if (y < BARRA_A && s_pilula_x0 >= 0 && x >= s_pilula_x0 && x < s_pilula_x1) {
    const uint8_t current = tab5_espnow_canal_fixo();
    int k = 0;
    while (k < (int)sizeof(CANAIS) && CANAIS[k] != current) { ++k; }
    tab5_espnow_set_canal(CANAIS[(k + 1) % (int)sizeof(CANAIS)]);
    system_request_redraw();
    return;
  }
  if (y < BARRA_A) { return; }
  conversation_touch(&s_c, x, y);
}
static void on_touch_level(int x, int y, bool p)       { conversation_touch_level(&s_c, x, y, p); }
static void on_type(const char* t, uint8_t c, uint8_t m) { conversation_type(&s_c, t, c, m); }

extern const app_t APP_ESPNOW = {
  .name  = "ESP-NOW",
  .color   = TFT_CYAN,
  // O radio e of servico of WiFi (o ajuste of sistema mantem). O IMU e para
  // a screen follow o aparelho.
  .profile = { .imu = true },
  .uses_sd = false,          // o historico e of servico
  // Em pe ou deitado conforme o aparelho; com o keyboard encaixado, deitado
  // (igual on Mesh). A conversa e o componente of sistema e ja se mede pela
  // width que chega - so a barra of top precisa saber of retrato.
  .orientation = ORIENTATION_FOLLOWS_IMU,
  .piso_sleep_mhz = 0,
  .on_open  = on_open,
  .on_close = on_close,
  .draw  = draw,
  .on_touch  = on_touch,
  .on_touch_level = on_touch_level,   // key acende sob o finger; rolagem
  .on_type = on_type,             // keyboard fisico of ExtPort1
  .periodo_ms = 1000,                 // o "ultimo ha Ns" of barra
  .notices = AVISO_ESPNOW,
  .on_aviso = on_aviso,
  .icon = marca_espnow,
};
