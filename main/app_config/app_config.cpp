// APP: Ajustes - as preferences of sistema.
//
// NOVA FORMA, a of qualquer celular:
//
//   - no TOPO, o que se mexe sem pensar: BRILHO e VOLUME, direto in barra;
//   - a line, and below the LIST of pages - icon, name, and a line of
//     estado ("Conectado a ...", "Desligado") para saber sem entrar;
//   - each page has BACK and its options. The list scrolls (kernel/list):
//     The new option is ONE line in the PAGES table, not a patch of position.
//
// The previous version stacked all sections in a column of 1800 px which
// system rolled; each new section pushed the others, and the heights were
// numbers added by hand (and they got it wrong more than once).
//
// What lives here is of the USER, not of the application: each app continues
// declarando o hardware que precisa no profile dele; este app controla o que
// traverses all (settings.h).

#include <math.h>
#include "tab5_time.hpp"   // tab5_now_ms
#include "app.h"
#include <cmath>
#include "settings.h"
#include "tab5_wifi.hpp"
#include "tab5_ble.hpp"
#include "tab5_signal.hpp"
#include "tab5_espnow.hpp"
#include "tab5_mesh.hpp"
#include "tab5_mesh_ble.hpp"
#include "tab5_lora.hpp"
#include "tab5_keyboard.hpp"
#include "tab5_grove.hpp"
#include "tab5_screen.hpp"
#include "tab5_ui.hpp"
#include "paint.hpp"
#include "list.hpp"
#include "conversation.hpp"          // conversa_quebrar_linhas (text corrido)
#include "virtual_keyboard.hpp"   // o PIN of Meshtastic
#include "work.hpp"

#include "tab5_flashdrive.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// MEDIDAS
// ---------------------------------------------------------------------------
static const int MARGEM   = 24;
static const int ROTULO_PX = 30;      // "Brilho", "Volume"
static const int CTRL_A   = 64;       // height of barra e dos botoes - e /+
static const int PM_L     = 88;       // width dos botoes - e +
static const int RAPIDO_Y0 = 16;
static const int RAPIDO_A  = 44 + CTRL_A + 24;   // rotulo + controle + respiro
static const int DIVISOR_Y = RAPIDO_Y0 + 2 * RAPIDO_A + 4;
static const int LISTA_Y0  = DIVISOR_Y + 12;
static const int ITEM_A    = 104;
static const int ICONE_L   = 64;

// Paginas
static const int VOLTAR_L = 200, VOLTAR_A = 64;
static const int CONTEUDO_Y = 112;     // abaixo of VOLTAR/titulo
static const int BTN_A = 72;

static const int PASSO_BRILHO = 50;   // em BRILHO_POS_MAX: 20 touches of minimum on maximum
static const int PASSO_VOLUME = 15;
static const int PASSO_TELA_S = 5;
static const int PASSO_RELOGIO = 5;

static uint16_t color_line(lgfx::LovyanGFX& d) { return d.color565(48, 52, 62); }
static uint16_t color_sub(lgfx::LovyanGFX& d)   { return d.color565(150, 156, 168); }

// ---------------------------------------------------------------------------
// AS PAGINAS - a lista of bottom exit daqui
// ---------------------------------------------------------------------------
typedef enum {
  PAG_INICIO = -1,
  PAG_WIFI = 0, PAG_BLUETOOTH, PAG_ESPNOW, PAG_MESH, PAG_LORA, PAG_TELA, PAG_APPS, PAG_TECLADO, PAG_GROVE, PAG_USB, PAG_SOBRE,
  PAG_N
} pagina_t;

typedef void (*icon_fn)(lgfx::LovyanGFX& d, int x, int y, int l);
typedef void (*estado_fn)(char* out, size_t size);

static void ic_wifi(lgfx::LovyanGFX&, int, int, int);
static void ic_bt(lgfx::LovyanGFX&, int, int, int);
static void ic_espnow(lgfx::LovyanGFX&, int, int, int);
static void ic_mesh(lgfx::LovyanGFX&, int, int, int);
static void ic_lora(lgfx::LovyanGFX&, int, int, int);
static void ic_screen(lgfx::LovyanGFX&, int, int, int);
static void ic_apps(lgfx::LovyanGFX&, int, int, int);
static void ic_keyboard(lgfx::LovyanGFX&, int, int, int);
static void ic_grove(lgfx::LovyanGFX&, int, int, int);
static void ic_usb(lgfx::LovyanGFX&, int, int, int);
static void ic_sobre(lgfx::LovyanGFX&, int, int, int);
static void est_wifi(char*, size_t);
static void est_bt(char*, size_t);
static void est_espnow(char*, size_t);
static void est_mesh(char*, size_t);
static void est_lora(char*, size_t);
static void est_screen(char*, size_t);
static void est_apps(char*, size_t);
static void est_keyboard(char*, size_t);
static void est_grove(char*, size_t);
static void est_usb(char*, size_t);
static void est_sobre(char*, size_t);

typedef struct { const char* name; icon_fn icon; estado_fn state; } pagina_def_t;
static const pagina_def_t PAGINAS[PAG_N] = {
  { "Wi-Fi",        ic_wifi,    est_wifi    },
  { "Bluetooth",    ic_bt,      est_bt      },
  { "ESP-NOW",      ic_espnow,  est_espnow  },
  { "Meshtastic",   ic_mesh,    est_mesh    },
  { "LoRa",         ic_lora,    est_lora    },
  { "Screen",       ic_screen,    est_screen    },
  { "Apps",         ic_apps,    est_apps    },
  { "Keyboard",     ic_keyboard, est_keyboard },
  { "Grove 5V",     ic_grove,   est_grove   },
  { "USB (pendrive)", ic_usb,   est_usb     },
  { "About",        ic_sobre,   est_sobre   },
};

// ---------------------------------------------------------------------------
// ESTADO DA TELA
// ---------------------------------------------------------------------------
static paint_t s_p;
static int       s_pag = PAG_INICIO;
static list_t   s_lista;
static char      s_sub[PAG_N][48];     // a line of status of each item, as drawn
static int       s_ub = -1, s_uv = -1; // brilho/volume desenhados
static char      s_assin[512];         // "assinatura" of pagina open: changed, redesenha
static char      s_aviso[96];          // recado curto numa pagina ("escolhido para o Mesh")

// Aplicativos: a lista complete e os nomes escondidos of screen inicial.
static list_t   s_l_apps;
static char      s_apps_ocultos[1024];

// Bluetooth: a lista of aparelhos vistos
static list_t   s_l_bt;
static uint32_t  s_bt_ver = 0;
static int       s_bt_n = 0;
static tab5_ble_seen_t s_bt[TAB5_BLE_SEEN_MAX];

// Meshtastic: o editor of PIN
static bool  s_pin_editando = false;
static char  s_pin_txt[8];
// O keyboard virtual of sistema - one so, para a senha of Wi-Fi e o PIN (nunca
// abertos on mesmo time: switch of pagina fecha).
static virtual_keyboard_t s_kb;

static void open_pagina(int p);

// ---------------------------------------------------------------------------
// TEXTO E BOTOES
// ---------------------------------------------------------------------------
static void text(lgfx::LovyanGFX& d, int x, int y, int px, bool negrito, uint16_t color,
                  const char* s, uint8_t datum = top_left)
{
  d.setFont(tab5_fonte_suave(negrito, px));
  d.setTextSize(1);
  d.setTextDatum(datum);
  d.setTextColor(color, TFT_BLACK);
  d.drawString(s, x, y);
  d.setTextDatum(top_left);
  tab5_fonte_body(d);
}

// Paragrafo quebrado in width - o mesmo quebrador dos baloes of conversa.
static int paragrafo(lgfx::LovyanGFX& d, int x, int y, int width, const char* s)
{
  d.setFont(tab5_fonte_suave(false, 24));
  d.setTextSize(1);
  char lines[6][CONVERSA_LINHA_BYTES];
  const int n = conversation_break_lines(d, s, width, lines, 6);
  d.setTextDatum(top_left);
  d.setTextColor(color_sub(d), TFT_BLACK);
  for (int k = 0; k < n; ++k) { d.drawString(lines[k], x, y + k * 32); }
  tab5_fonte_body(d);
  return y + n * 32;
}


// Chevron ">" drawn (the font would have a different weight for each size).
static void chevron(lgfx::LovyanGFX& d, int cx, int cy, uint16_t color)
{
  for (int k = 0; k < 3; ++k) {
    d.drawLine(cx - 6 + k, cy - 12, cx + 6 + k, cy, color);
    d.drawLine(cx + 6 + k, cy, cx - 6 + k, cy + 12, color);
  }
}

// Cabecalho of pagina: VOLTAR e o titulo.
static void cabecalho(lgfx::LovyanGFX& d, const area_t& a, const char* titulo)
{
  tab5_botao(d, a.x + 16, a.y + 16, VOLTAR_L, VOLTAR_A, "< VOLTAR", TFT_SKYBLUE);
  tab5_titulo(d, 40);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(titulo, a.x + 16 + VOLTAR_L + 28, a.y + 16 + VOLTAR_A / 2);
  d.setTextDatum(top_left);
  tab5_fonte_body(d);
  d.drawFastHLine(a.x, a.y + CONTEUDO_Y - 16, a.width, color_line(d));
}

// Linha of informacao: rotulo pequeno em top, valor embaixo. 76 px.
static int info(lgfx::LovyanGFX& d, const area_t& a, int y, const char* rotulo,
                const char* valor, uint16_t color = TFT_WHITE)
{
  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), rotulo);
  text(d, a.x + MARGEM, a.y + y + 28, 32, false, color, valor);
  return y + 84;
}

// Botao of width of pagina.
static int botao_largo(lgfx::LovyanGFX& d, const area_t& a, int y, const char* txt, uint16_t color)
{
  tab5_botao(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM, BTN_A, txt, color);
  return y + BTN_A + 20;
}

// Par - / valor / + (time of screen, clock, canal).
static const int PM_VALOR_L = 260;
static void menos_mais(lgfx::LovyanGFX& d, const area_t& a, int y, const char* valor)
{
  const int x = a.x + MARGEM;
  tab5_botao(d, x, a.y + y, PM_L + 40, BTN_A, "-", TFT_ORANGE);
  text(d, x + PM_L + 40 + PM_VALOR_L / 2 + 20, a.y + y + BTN_A / 2, 34, true, TFT_WHITE, valor, middle_center);
  tab5_botao(d, x + PM_L + 40 + PM_VALOR_L + 40, a.y + y, PM_L + 40, BTN_A, "+", TFT_GREENYELLOW);
}
static int menos_mais_touch(int tx, int ty, int y)
{
  if (tab5_dentro(tx, ty, MARGEM, y, PM_L + 40, BTN_A)) { return -1; }
  if (tab5_dentro(tx, ty, MARGEM + PM_L + 40 + PM_VALOR_L + 40, y, PM_L + 40, BTN_A)) { return +1; }
  return 0;
}

// ---------------------------------------------------------------------------
// ICONES DA LISTA - quadrado colorido com one drawing simples, como qualquer
// screen of ajustes of celular. Tudo em fracao of lado (l).
// ---------------------------------------------------------------------------
static void background_icon(lgfx::LovyanGFX& d, int x, int y, int l, uint16_t color)
{
  d.fillRoundRect(x, y, l, l, l / 5, color);
}

static void ic_wifi(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(30, 120, 230));
  const int cx = x + l / 2, cy = y + l * 3 / 4;
  for (int k = 0; k < 3; ++k) {
    const int r = l * (18 + 12 * k) / 100;
    d.fillArc(cx, cy, r, r - l / 14, 225, 315, TFT_WHITE);
  }
  d.fillCircle(cx, cy - 1, l / 14, TFT_WHITE);
}

static void ic_bt(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(20, 80, 200));
  const int cx = x + l / 2, t = y + l / 6, b = y + l * 5 / 6, m = y + l / 2;
  const int dx = l / 5, q = (b - t) / 4;
  for (int k = -1; k <= 1; ++k) {
    d.drawLine(cx + k, t, cx + k, b, TFT_WHITE);
    d.drawLine(cx + k, t, cx + dx + k, t + q, TFT_WHITE);
    d.drawLine(cx + dx + k, t + q, cx - dx + k, m + q, TFT_WHITE);
    d.drawLine(cx + k, b, cx + dx + k, b - q, TFT_WHITE);
    d.drawLine(cx + dx + k, b - q, cx - dx + k, m - q, TFT_WHITE);
  }
}

static void ic_espnow(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(0, 150, 110));
  const int cy = y + l / 2;
  d.fillCircle(x + l / 4, cy, l / 9, TFT_WHITE);
  d.fillCircle(x + l * 3 / 4, cy, l / 9, TFT_WHITE);
  for (int k = 0; k < 3; ++k) {
    d.fillCircle(x + l * (38 + 12 * k) / 100, cy, l / 28, TFT_WHITE);
  }
}

static void ic_mesh(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(40, 170, 60));
  const int ax = x + l / 4, ay = y + l * 3 / 4, bx = x + l / 2, by = y + l / 4;
  const int cx = x + l * 3 / 4, cy = y + l * 3 / 4;
  for (int k = -1; k <= 1; ++k) {
    d.drawLine(ax, ay + k, bx, by + k, TFT_WHITE);
    d.drawLine(bx, by + k, cx, cy + k, TFT_WHITE);
    d.drawLine(ax, ay + k, cx, cy + k, TFT_WHITE);
  }
  d.fillCircle(ax, ay, l / 10, TFT_WHITE);
  d.fillCircle(bx, by, l / 10, TFT_WHITE);
  d.fillCircle(cx, cy, l / 10, TFT_WHITE);
}

static void ic_lora(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(190, 80, 35));
  const int cx = x + l / 2, cy = y + l * 3 / 4;
  d.fillRect(cx - 2, y + l / 4, 5, l / 2, TFT_WHITE);
  d.fillCircle(cx, cy, l / 14, TFT_WHITE);
  for (int k = 0; k < 2; ++k) {
    const int r = l * (22 + 14 * k) / 100;
    d.drawArc(cx, cy, r, r - 3, 205, 335, TFT_WHITE);
  }
}

static void ic_screen(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(240, 150, 20));
  const int cx = x + l / 2, cy = y + l / 2;
  d.fillCircle(cx, cy, l / 6, TFT_WHITE);
  for (int k = 0; k < 8; ++k) {
    const float ang = k * (float)M_PI / 4.0f;
    const int r0 = l * 26 / 100, r1 = l * 36 / 100;
    for (int e = -1; e <= 1; ++e) {
      d.drawLine(cx + (int)(r0 * cosf(ang)) + e, cy + (int)(r0 * sinf(ang)),
                 cx + (int)(r1 * cosf(ang)) + e, cy + (int)(r1 * sinf(ang)), TFT_WHITE);
    }
  }
}

static void ic_apps(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(70, 130, 210));
  const int q = l / 4, vao = l / 10;
  const int x0 = x + (l - 2 * q - vao) / 2, y0 = y + (l - 2 * q - vao) / 2;
  for (int f = 0; f < 2; ++f) {
    for (int c = 0; c < 2; ++c) {
      d.fillRoundRect(x0 + c * (q + vao), y0 + f * (q + vao), q, q, q / 5, TFT_WHITE);
    }
  }
}

static void ic_keyboard(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(110, 116, 128));
  const int kx = x + l / 8, ky = y + l * 3 / 10, kl = l * 3 / 4, ka = l * 2 / 5;
  d.fillRoundRect(kx, ky, kl, ka, 4, TFT_WHITE);
  const uint16_t tec = d.color565(110, 116, 128);
  const int t = kl / 7;
  for (int f = 0; f < 2; ++f) {
    for (int c = 0; c < 5; ++c) { d.fillRect(kx + t / 2 + c * (t + t / 3), ky + 5 + f * (t + 2), t, t - 2, tec); }
  }
  d.fillRect(kx + kl / 4, ky + ka - t / 2 - 5, kl / 2, t / 2, tec);
}

static void ic_grove(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(150, 70, 200));
  const int cx = x + l / 2;
  d.fillRoundRect(cx - l / 5, y + l * 35 / 100, l * 2 / 5, l * 3 / 10, 5, TFT_WHITE);
  d.fillRect(cx - l / 8, y + l / 6, l / 14, l / 5, TFT_WHITE);
  d.fillRect(cx + l / 8 - l / 14, y + l / 6, l / 14, l / 5, TFT_WHITE);
  d.fillRect(cx - l / 20, y + l * 65 / 100, l / 10, l / 6, TFT_WHITE);
}

// The symbol of USB: rod, triangle at the tip, and the two branches (ball and
// quadrado) - o mesmo drawing gravado no plugue.
static void ic_usb(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(40, 120, 170));
  const int cx = x + l / 2, topo = y + l / 6, base = y + l * 80 / 100, e = l / 24 + 1;
  d.fillRect(cx - e / 2, topo + l / 10, e, base - topo - l / 10, TFT_WHITE);
  d.fillTriangle(cx, topo, cx - l / 10, topo + l / 8, cx + l / 10, topo + l / 8, TFT_WHITE);
  d.fillCircle(cx, base, l / 14, TFT_WHITE);
  const int ry = y + l * 45 / 100;
  d.drawLine(cx, ry + l / 8, cx - l / 5, ry, TFT_WHITE);
  d.drawLine(cx + 1, ry + l / 8, cx - l / 5 + 1, ry, TFT_WHITE);
  d.fillCircle(cx - l / 5, ry - l / 22, l / 18, TFT_WHITE);
  const int ry2 = y + l * 55 / 100;
  d.drawLine(cx, ry2 + l / 8, cx + l / 5, ry2, TFT_WHITE);
  d.drawLine(cx - 1, ry2 + l / 8, cx + l / 5 - 1, ry2, TFT_WHITE);
  d.fillRect(cx + l / 5 - l / 20, ry2 - l / 10, l / 10, l / 10, TFT_WHITE);
}

static void ic_sobre(lgfx::LovyanGFX& d, int x, int y, int l)
{
  background_icon(d, x, y, l, d.color565(90, 96, 110));
  const int cx = x + l / 2;
  d.fillCircle(cx, y + l * 28 / 100, l / 12, TFT_WHITE);
  d.fillRoundRect(cx - l / 14, y + l * 42 / 100, l / 7, l * 38 / 100, 3, TFT_WHITE);
}

// ---------------------------------------------------------------------------
// STATUS LINE OF EACH PAGE (subtitle in list)
// ---------------------------------------------------------------------------
static void est_wifi(char* o, size_t t)
{
  const tab5_wifi_state_t e = tab5_wifi_state();
  if (e == TAB5_WIFI_CONNECTED)           { snprintf(o, t, "Connected to %s", tab5_wifi_ssid()); }
  else if (e == TAB5_WIFI_NO_CREDENTIALS) { snprintf(o, t, "No saved network"); }
  else if (e == TAB5_WIFI_C6_UNRESPONSIVE)        { snprintf(o, t, "The C6 is not responding"); }
  else if (e == TAB5_WIFI_FAILED)         { snprintf(o, t, "Could not connect"); }
  else if (e == TAB5_WIFI_OFF)      { snprintf(o, t, settings_wifi() ? "Not connected to a router" : "Off"); }
  else                                    { snprintf(o, t, "Connecting..."); }
}

static void est_bt(char* o, size_t t)
{
  const tab5_ble_state_t e = tab5_ble_state();
  if (e == TAB5_BLE_READY)       { snprintf(o, t, "On"); }
  else if (e == TAB5_BLE_STARTING) { snprintf(o, t, "Turning on..."); }
  // C6 with the factory firmware (Wi-Fi works, Bluetooth does not): it is not
  // defect, and an optional update - the text has to say that.
  else if (e == TAB5_BLE_FAILED && tab5_wifi_c6_link() && !tab5_wifi_c6_is_ours())
                                  { snprintf(o, t, "Factory C6 radio: update optional"); }
  else if (e == TAB5_BLE_FAILED)  { snprintf(o, t, "Failed - the C6 did not bring up Bluetooth"); }
  else if (e == TAB5_BLE_STOPPING) { snprintf(o, t, "Turning off..."); }
  else                            { snprintf(o, t, "Off"); }
}

static void est_espnow(char* o, size_t t)
{
  if (!tab5_espnow_c6_compativel()) {
    // Without a link, I still can't find out which firmware the C6 has.
    snprintf(o, t, tab5_wifi_c6_link() ? "Factory C6 radio: update optional"
                                         : "Turn on Wi-Fi");
    return;
  }
  const uint8_t f = tab5_espnow_canal_fixo();
  if (f) { snprintf(o, t, "Fixed channel %u (no router)", (unsigned)f); }
  else   { snprintf(o, t, "Router channel (%u)", (unsigned)tab5_espnow_canal()); }
}

static const char* name_via_mesh(uint8_t via)
{
  return via == SETTINGS_MESH_TRANSPORT_BLUETOOTH ? "Bluetooth" : via == SETTINGS_MESH_TRANSPORT_INTERNET ? "Internet" : "Grove cable";
}

static void est_mesh(char* o, size_t t)
{
  snprintf(o, t, "%s - %s", name_via_mesh(settings_mesh_transport()), tab5_mesh_enlace_descricao());
}

static void est_lora(char* o, size_t t)
{
  const tab5_lora_status_t s = tab5_lora_status();
  if (!s.probed)             { snprintf(o, t, "Not probed yet"); }
  else if (!s.ioe_address)   { snprintf(o, t, "M5IOE1 not found"); }
  else if (s.radio_present)  { snprintf(o, t, "SX1262 responde · M5IOE1 0x%02X", s.ioe_address); }
  else                        { snprintf(o, t, "M5IOE1 0x%02X · SX1262 did not respond", s.ioe_address); }
}

static void est_screen(char* o, size_t t)
{
  snprintf(o, t, "Blanks after %u s  ·  panel at %d MHz", (unsigned)settings_screen_timeout_s(),
           tab5_power_dpi_clock());
}

static bool name_em_lista(const char* lista, const char* name)
{
  const size_t name_n = strlen(name);
  const char* p = lista ? lista : "";
  while (*p) {
    const char* end = strchr(p, ',');
    const size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n == name_n && strncasecmp(p, name, n) == 0) { return true; }
    if (!end) { break; }
    p = end + 1;
  }
  return false;
}

static void est_apps(char* o, size_t t)
{
  int visiveis = 0;
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (strcasecmp(SISTEMA_APPS[i]->name, "Settings") == 0
        || !name_em_lista(s_apps_ocultos, SISTEMA_APPS[i]->name)) { ++visiveis; }
  }
  snprintf(o, t, "%d of %d visible", visiveis, SISTEMA_NUM_APPS);
}

static void est_keyboard(char* o, size_t t)
{
  if (!settings_keyboard())            { snprintf(o, t, "Disabled"); }
  else if (tab5_keyboard_present())  { snprintf(o, t, "Docked"); }
  else                               { snprintf(o, t, "Looking on ExtPort1..."); }
}

static void est_usb(char* o, size_t t)
{
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  if (e.fase == PENDRIVE_ATIVO)           { snprintf(o, t, "Card on the computer"); }
  else if (e.fase != PENDRIVE_PARADO)     { snprintf(o, t, "Flash drive: please wait..."); }
  else if (e.oferta)                      { snprintf(o, t, "Computer connected"); }
  else if (settings_flash_drive())            { snprintf(o, t, "Turns on by itself on the computer"); }
  else                                    { snprintf(o, t, "Flash drive off"); }
}

static void est_grove(char* o, size_t t)
{
  const int dono = (int)tab5_grove_owner();
  if (system_grove_probing())                 { snprintf(o, t, "Probing the cable..."); }
  else if (dono != (int)GROVE_OWNER_NONE)     { snprintf(o, t, "In use: %s", grove_owner_name((grove_owner_t)dono)); }
  else if (settings_grove())                     { snprintf(o, t, "On - %s", grove_name(tab5_grove_type())); }
  else                                          { snprintf(o, t, "Off"); }
}

// The whole build's stamp (main/build_stamp.cmake), not this file's
// __DATE__ - which only changed when THIS file recompiled.
extern "C" const char* const m5tabos_compiled;

static void est_sobre(char* o, size_t t)
{
  snprintf(o, t, "M5tabOS %s", esp_app_get_description()->version);
}

// ---------------------------------------------------------------------------
// INICIO: BRILHO, VOLUME e a lista
// ---------------------------------------------------------------------------
static int barra_x(const area_t& a) { return a.x + MARGEM + PM_L + 16; }
static int barra_l(const area_t& a) { return a.width - 2 * MARGEM - 2 * (PM_L + 16); }

// BRIGHTNESS ON THE EYE SCALE. The value of PWM is linear in light, but the eye does not:
// 4% of duty ja parece "one quarto of brilho". Com a barra linear quase todo o
// progress ficava no claro e o first degrau (10 -> 25) pulava of 4% para 10%.
// A barra anda numa position 0..1000 e o PWM exit dela pela curva gama 2,2 - a
// metade of bottom of barra cobre of 0,9% a ~22%, em degraus finos.
#define BRILHO_POS_MAX 1000
static int brilho_para_pos(int b)
{
  const float f = (float)(b - SETTINGS_BRIGHTNESS_MIN) / (float)(SETTINGS_BRIGHTNESS_MAX - SETTINGS_BRIGHTNESS_MIN);
  return (int)(powf(f < 0 ? 0 : f, 1.0f / 2.2f) * BRILHO_POS_MAX + 0.5f);
}
static int pos_para_brilho(int pos)
{
  const float f = (float)pos / (float)BRILHO_POS_MAX;
  return SETTINGS_BRIGHTNESS_MIN + (int)(powf(f, 2.2f) * (SETTINGS_BRIGHTNESS_MAX - SETTINGS_BRIGHTNESS_MIN) + 0.5f);
}

static void draw_rapido(lgfx::LovyanGFX& d, const area_t& a, int y0, const char* name,
                            int valor, int minimum, int maximum, bool everything, int anterior)
{
  const int x = a.x + MARGEM;
  const int pct = ((valor - minimum) * 100) / (maximum - minimum);
  char buf[16];
  if (everything) {
    text(d, x, a.y + y0, ROTULO_PX, true, TFT_WHITE, name);
    tab5_botao(d, x, a.y + y0 + 44, PM_L, CTRL_A, "-", TFT_ORANGE);
    tab5_botao(d, a.x + a.width - MARGEM - PM_L, a.y + y0 + 44, PM_L, CTRL_A, "+", TFT_GREENYELLOW);
  }
  // Valor a right of rotulo (apaga o anterior: width variavel)
  d.fillRect(a.x + a.width - MARGEM - 160, a.y + y0, 160, 40, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%d%%", pct);
  text(d, a.x + a.width - MARGEM, a.y + y0, ROTULO_PX, false, TFT_LIGHTGREY, buf, top_right);

  // Barra: moldura so in pass complete; o preenchimento so no que changed.
  const int bx = barra_x(a), bl = barra_l(a), by = a.y + y0 + 44;
  const int dentro_l = bl - 8;
  const int cheio = ((valor - minimum) * dentro_l) / (maximum - minimum);
  const int antes = (everything || anterior < 0) ? -1 : ((anterior - minimum) * dentro_l) / (maximum - minimum);
  if (everything) {
    d.fillRoundRect(bx, by, bl, CTRL_A, CTRL_A / 2, d.color565(34, 38, 46));
  }
  if (antes < 0) {
    d.fillRoundRect(bx + 4, by + 4, dentro_l, CTRL_A - 8, (CTRL_A - 8) / 2, d.color565(34, 38, 46));
    if (cheio > 0) { d.fillRoundRect(bx + 4, by + 4, cheio < CTRL_A - 8 ? CTRL_A - 8 : cheio, CTRL_A - 8, (CTRL_A - 8) / 2, TFT_SKYBLUE); }
  } else if (cheio != antes) {
    // Redesigns the entire bar from the inside - the round corners won't let you
    // paint so a diferenca sem sobrar serrilhado.
    d.fillRoundRect(bx + 4, by + 4, dentro_l, CTRL_A - 8, (CTRL_A - 8) / 2, d.color565(34, 38, 46));
    if (cheio > 0) { d.fillRoundRect(bx + 4, by + 4, cheio < CTRL_A - 8 ? CTRL_A - 8 : cheio, CTRL_A - 8, (CTRL_A - 8) / 2, TFT_SKYBLUE); }
  }
}

static int item_height(int, void*) { return ITEM_A; }

static void item_draw(lgfx::LovyanGFX& d, int i, int x0, int y, int width, void*)
{
  if (i < 0 || i >= PAG_N) { return; }
  d.fillRect(x0, y, width, ITEM_A, TFT_BLACK);
  const int ix = x0 + MARGEM, iy = y + (ITEM_A - ICONE_L) / 2;
  PAGINAS[i].icon(d, ix, iy, ICONE_L);
  const int tx = ix + ICONE_L + 24;
  text(d, tx, y + 18, 32, true, TFT_WHITE, PAGINAS[i].name);
  // State cut MEASUREMENT, so as not to go through the bottom of chevron.
  char sub[48];
  snprintf(sub, sizeof(sub), "%s", s_sub[i]);
  d.setFont(tab5_fonte_suave(false, 24));
  const int max_l = x0 + width - MARGEM - 40 - tx;
  tab5_cortar_on_caber(d, sub, max_l);
  tab5_fonte_body(d);
  text(d, tx, y + 60, 24, false, color_sub(d), sub);
  chevron(d, x0 + width - MARGEM - 12, y + ITEM_A / 2, color_sub(d));
  if (i + 1 < PAG_N) { d.drawFastHLine(tx, y + ITEM_A - 1, x0 + width - tx, color_line(d)); }
}

static void draw_start(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  const int b = settings_brightness(), v = settings_volume();
  const int b_ant = s_ub, v_ant = s_uv;
  if (paint_changed_i(&s_p, &s_ub, b) || everything) {
    draw_rapido(d, a, RAPIDO_Y0, "Brightness", brilho_para_pos(b), 0, BRILHO_POS_MAX, everything,
                    b_ant < 0 ? -1 : brilho_para_pos(b_ant));
  }
  if (paint_changed_i(&s_p, &s_uv, v) || everything) {
    draw_rapido(d, a, RAPIDO_Y0 + RAPIDO_A, "Volume", v, SETTINGS_VOLUME_MIN, SETTINGS_VOLUME_MAX, everything, v_ant);
  }
  if (everything) { d.drawFastHLine(a.x, a.y + DIVISOR_Y, a.width, color_line(d)); }

  // State of each page: only the item whose text changed and redesigned.
  for (int i = 0; i < PAG_N; ++i) {
    char now[48];
    PAGINAS[i].state(now, sizeof(now));
    if (strcmp(now, s_sub[i]) != 0) {
      snprintf(s_sub[i], sizeof(s_sub[i]), "%s", now);
      list_item_changed(&s_lista, i);
    }
  }
  const area_t window = { a.x, a.y + LISTA_Y0, a.width, a.height - LISTA_Y0 };
  list_draw(&s_lista, d, window, TFT_BLACK, everything);
}

static void mexer_rapido(int y0, int tx, int ty, bool so_barra)
{
  const area_t a = system_area();
  const bool brilho = (y0 == RAPIDO_Y0);
  // O brilho anda in position perceptual (ver brilho_para_pos).
  const int minimum = brilho ? 0 : SETTINGS_VOLUME_MIN;
  const int maximum = brilho ? BRILHO_POS_MAX : SETTINGS_VOLUME_MAX;
  const int current  = brilho ? brilho_para_pos(settings_brightness()) : settings_volume();
  const int passo  = brilho ? PASSO_BRILHO : PASSO_VOLUME;
  const int cy = y0 + 44;
  int novo = current;
  const int bx = barra_x(a) - a.x, bl = barra_l(a);
  if (tab5_dentro(tx, ty, bx - 8, cy, bl + 16, CTRL_A)) {
    int rel = tx - bx; if (rel < 0) { rel = 0; } if (rel > bl) { rel = bl; }
    novo = minimum + (rel * (maximum - minimum)) / bl;
  } else if (so_barra) {
    return;
  } else if (tab5_dentro(tx, ty, MARGEM, cy, PM_L, CTRL_A)) {
    novo = current - passo;
  } else if (tab5_dentro(tx, ty, a.width - MARGEM - PM_L, cy, PM_L, CTRL_A)) {
    novo = current + passo;
  } else {
    return;
  }
  if (novo < minimum) { novo = minimum; }
  if (novo > maximum) { novo = maximum; }
  if (novo == current) { return; }
  if (brilho) {
    int b = pos_para_brilho(novo);
    // In the case of curve two touches of "-"/"+" can fall into the same value of PWM:
    // o botao sempre anda pelo menos one.
    const int b_current = settings_brightness();
    if (b == b_current) { b += (novo > current) ? 1 : -1; }
    if (b < SETTINGS_BRIGHTNESS_MIN) { b = SETTINGS_BRIGHTNESS_MIN; }
    if (b > SETTINGS_BRIGHTNESS_MAX) { b = SETTINGS_BRIGHTNESS_MAX; }
    if (b == b_current) { return; }
    settings_set_brightness((uint8_t)b);
  }
  else {
    settings_set_volume((uint8_t)novo);
    if (!so_barra) { system_beep(880.0f, 80); }   // ouvir o volume novo
  }
  system_request_redraw();
}

// ---------------------------------------------------------------------------
// PAGINAS
// ---------------------------------------------------------------------------
// Each page: draw(everything) and touch(x,y). The page redesigns the content
// inteiro when a ASSINATURA (os valores que ela mostra) muda - e so entao.
static void content_limpo(lgfx::LovyanGFX& d, const area_t& a)
{
  tab5_screen_clear(a.x, a.y + CONTEUDO_Y, a.width, a.height - CONTEUDO_Y, TFT_BLACK);
}

// ---- Wi-Fi -----------------------------------------------------------------
// Estado, enable/disable, procurar, e a LISTA of redes em iteration. Tocar numa
// rede conhecida conecta; numa nova, pede a senha (keyboard of screen ou of
// ExtPort1), save it encrypted on the card and connect. Double tap on network
// uso a esquece.
static const int WF_ITEM_A = 92;
static list_t   s_l_wifi;
static uint32_t  s_wf_ver = 0;
static int       s_wf_n = 0;
static tab5_wifi_seen_t s_wf[TAB5_WIFI_MAX_SEEN];
static bool      s_wf_senha = false;          // editando a senha of s_wf_alvo
static char      s_wf_alvo[33];
static char      s_wf_txt[65];
static char      s_wf_field[70];          // o que esta pintado no field of senha
static char      s_wf_esquecer[33];           // 1o touch in rede em uso
static uint32_t  s_wf_esquecer_ms = 0;

static int wf_y_lista(void) { return CONTEUDO_Y + 84 + (BTN_A + 20) * 2 + 44; }

static bool wf_em_uso(const char* ssid)
{
  return tab5_wifi_state() == TAB5_WIFI_CONNECTED && strcmp(tab5_wifi_ssid(), ssid) == 0;
}

static int wf_item_height(int, void*) { return WF_ITEM_A; }
static void wf_item_draw(lgfx::LovyanGFX& d, int i, int x0, int y, int width, void*)
{
  if (i < 0 || i >= s_wf_n) { return; }
  const tab5_wifi_seen_t& v = s_wf[i];
  d.fillRect(x0, y, width, WF_ITEM_A, TFT_BLACK);
  const bool uso = wf_em_uso(v.ssid);
  // Sinal em 4 barras, a left.
  const int nb = v.rssi > -55 ? 4 : v.rssi > -65 ? 3 : v.rssi > -75 ? 2 : 1;
  for (int b = 0; b < 4; ++b) {
    const int h = 8 + b * 8;
    d.fillRect(x0 + MARGEM + b * 10, y + 56 - h, 7, h, b < nb ? (uso ? TFT_GREENYELLOW : TFT_WHITE) : color_line(d));
  }
  const int tx = x0 + MARGEM + 56;
  text(d, tx, y + 12, 30, true, uso ? TFT_GREENYELLOW : TFT_WHITE, v.ssid);
  char line[80], dist[12];
  tab5_signal_distance_text(v.rssi, TAB5_SIGNAL_P1M_WIFI, dist, sizeof(dist));
  snprintf(line, sizeof(line), "%s%s%d dBm %s  ·  channel %u  ·  %s", uso ? "connected  ·  " : "",
           (!uso && v.known) ? "known  ·  " : "", (int)v.rssi, dist, (unsigned)v.channel,
           v.open ? "open" : "password-protected");
  text(d, tx, y + 52, 22, false, color_sub(d), line);
  d.drawFastHLine(tx, y + WF_ITEM_A - 1, x0 + width - tx, color_line(d));
}

static void wf_reload(void)
{
  const int n = tab5_wifi_seen_count();
  int k = 0;
  for (int i = 0; i < n && k < TAB5_WIFI_MAX_SEEN; ++i) { if (tab5_wifi_get_seen(i, &s_wf[k])) { ++k; } }
  s_wf_n = k;
  list_items_changed(&s_l_wifi, s_wf_n);
}

static void pag_wifi(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  char sub[48], assin[160];
  est_wifi(sub, sizeof(sub));
  const bool con = tab5_wifi_state() == TAB5_WIFI_CONNECTED;
  const uint32_t ver = tab5_wifi_networks_version();
  if (ver != s_wf_ver) { s_wf_ver = ver; wf_reload(); }
  char det[48];
  const int r = tab5_wifi_rssi();
  if (con && r != TAB5_WIFI_RSSI_UNKNOWN) { snprintf(det, sizeof(det), "%s  ·  %d dBm", tab5_wifi_ip(), r); }
  else if (con)                                { snprintf(det, sizeof(det), "%s", tab5_wifi_ip()); }
  else                                         { snprintf(det, sizeof(det), "%d known network(s)", tab5_wifi_known_count()); }
  // The typed text does NOT enter the signature: every letter used to
  // repaint the whole page plus the keyboard. Now a letter repaints only
  // the field.
  snprintf(assin, sizeof(assin), "%s|%s|%d|%d|%d|%s|%d", sub, det, (int)settings_wifi(), (int)tab5_wifi_searching(),
           s_wf_n, s_aviso, (int)s_wf_senha);
  const bool changed = paint_changed_s(&s_p, s_assin, sizeof(s_assin), assin) || everything;
  if (s_wf_senha) {
    const int y_field = CONTEUDO_Y + 36;
    virtual_keyboard_position(&s_kb, a);
    if (changed) {
      list_hide(&s_l_wifi);
      content_limpo(d, a);
      char rot[64];
      snprintf(rot, sizeof(rot), "Password for \"%s\"", s_wf_alvo);
      text(d, a.x + MARGEM, a.y + CONTEUDO_Y, 24, false, color_sub(d), rot);
      paragrafo(d, a.x + MARGEM, a.y + y_field + 92, a.width - 2 * MARGEM,
                "Tap SEND to save and connect. The password stays encrypted on the card. X cancels.");
    }
    char field[70];
    snprintf(field, sizeof(field), "%s_", s_wf_txt);
    if (paint_changed_s(&s_p, s_wf_field, sizeof(s_wf_field), field) || changed) {
      d.fillRoundRect(a.x + MARGEM, a.y + y_field, a.width - 2 * MARGEM, 72, 12, d.color565(40, 44, 54));
      d.setFont(tab5_fonte_suave(false, 34));
      d.setTextSize(1);
      d.setTextDatum(middle_left);
      d.setTextColor(TFT_WHITE, d.color565(40, 44, 54));
      d.drawString(field, a.x + MARGEM + 16, a.y + y_field + 36);
      d.setTextDatum(top_left);
      tab5_fonte_body(d);
    }
    virtual_keyboard_paint(&s_kb, d, changed);
    return;
  }
  if (changed) {
    content_limpo(d, a);
    int y = CONTEUDO_Y;
    text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "Status");
    text(d, a.x + MARGEM, a.y + y + 28, 32, false, con ? TFT_GREENYELLOW : TFT_WHITE, sub);
    text(d, a.x + a.width - MARGEM, a.y + y + 34, 22, false, color_sub(d), det, top_right);
    y += 84;
    y = botao_largo(d, a, y, settings_wifi() ? "TURN OFF WI-FI" : "TURN ON WI-FI",
                    settings_wifi() ? TFT_ORANGE : TFT_GREENYELLOW);
    y = botao_largo(d, a, y, tab5_wifi_searching() ? "PROCURANDO..." : "PROCURAR REDES",
                    tab5_wifi_searching() ? TFT_DARKGREY : TFT_SKYBLUE);
    text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d),
          s_aviso[0] ? s_aviso : (s_wf_n ? "Tap a network to connect" : "Tap SCAN FOR NETWORKS"));
  }
  const area_t window = { a.x, a.y + wf_y_lista(), a.width, a.height - wf_y_lista() };
  list_draw(&s_l_wifi, d, window, TFT_BLACK, changed);
}

// Write to the card and connect: to the worker (the card takes a while), with the password
// numa copia propria que o work apaga on terminar.
typedef struct { char ssid[33]; char senha[65]; bool esquecer; } wf_request_t;

static void wf_work(void* arg)
{
  wf_request_t* p = (wf_request_t*)arg;
  // The recording mounts the card alone (and, with it borrowed from the computer,
  // goes to the diary - the new network is not lost).
  if (p->esquecer) { tab5_wifi_forget(p->ssid); }
  else             { tab5_wifi_save_credentials(p->ssid, p->senha[0] ? p->senha : "-"); }
  if (!p->esquecer) { tab5_wifi_connect(p->ssid); }
  memset(p, 0, sizeof(*p));
  free(p);
}

static void wf_request(const char* ssid, const char* senha, bool esquecer)
{
  wf_request_t* p = (wf_request_t*)calloc(1, sizeof(wf_request_t));
  if (!p) { return; }
  snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
  snprintf(p->senha, sizeof(p->senha), "%s", senha ? senha : "");
  p->esquecer = esquecer;
  if (!system_work_released(wf_work, p)) { memset(p, 0, sizeof(*p)); free(p); }
}

// Conectar requires o Wi-Fi enabled: liga (e grava a vontade) se estiver disabled.
static void wf_enable_se_preciso(void)
{
  if (!settings_wifi()) { settings_set_wifi(true); tab5_wifi_turn_on(); }
}

static void wf_close_senha(void)
{
  s_wf_senha = false;
  memset(s_wf_txt, 0, sizeof(s_wf_txt));
  virtual_keyboard_close(&s_kb);
  system_request_redraw_complete();
}

static void wf_send_senha(void)
{
  if (!s_wf_txt[0]) { return; }
  wf_enable_se_preciso();
  wf_request(s_wf_alvo, s_wf_txt, false);
  snprintf(s_aviso, sizeof(s_aviso), "conectando a %s...", s_wf_alvo);
  wf_close_senha();
}

static void wf_key(int ac)
{
  if (ac == TECVIRT_ENVIAR) { wf_send_senha(); return; }
  if (ac == TECVIRT_FECHAR) { wf_close_senha(); return; }
  // Password and ASCII (as on the physical keyboard): the accent of the page of symbols does not enter.
  virtual_keyboard_edit(s_wf_txt, sizeof(s_wf_txt), ac, true);
  system_request_redraw();
}

static void touch_wifi(int tx, int ty)
{
  const area_t a = system_area();
  if (s_wf_senha) {
    if (virtual_keyboard_contains(&s_kb, a.x + tx, a.y + ty)) {
      const int ac = virtual_keyboard_click(&s_kb, a.x + tx, a.y + ty);
      if (ac) { wf_key(ac); }
    }
    return;
  }
  const int l = a.width - 2 * MARGEM;
  int y = CONTEUDO_Y + 84;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    const bool enable = !settings_wifi();
    settings_set_wifi(enable);
    if (enable) { tab5_wifi_turn_on(); } else { tab5_wifi_turn_off(); }
    s_aviso[0] = 0;
    system_request_redraw();
    return;
  }
  y += BTN_A + 20;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    if (!tab5_wifi_searching()) { tab5_wifi_search(); }
    s_aviso[0] = 0;
    system_request_redraw();
    return;
  }
  int dy = 0;
  const int i = list_item_touched(&s_l_wifi, tx, ty, &dy);
  if (i < 0 || i >= s_wf_n) { return; }
  const tab5_wifi_seen_t& v = s_wf[i];
  const uint32_t now = tab5_now_ms();
  if (wf_em_uso(v.ssid)) {
    // Em uso: o first touch avisa, o segundo (em 4 s) esquece a rede.
    if (strcmp(s_wf_esquecer, v.ssid) == 0 && now - s_wf_esquecer_ms < 4000) {
      wf_request(v.ssid, nullptr, true);
      snprintf(s_aviso, sizeof(s_aviso), "%s esquecida", v.ssid);
      s_wf_esquecer[0] = 0;
    } else {
      snprintf(s_wf_esquecer, sizeof(s_wf_esquecer), "%s", v.ssid);
      s_wf_esquecer_ms = now;
      snprintf(s_aviso, sizeof(s_aviso), "Connected. Tap again to forget");
    }
  } else if (v.known || v.open) {
    wf_enable_se_preciso();
    if (v.known) { tab5_wifi_connect(v.ssid); }
    else             { wf_request(v.ssid, nullptr, false); }   // open: guarda sem senha e conecta
    snprintf(s_aviso, sizeof(s_aviso), "conectando a %s...", v.ssid);
  } else {
    snprintf(s_wf_alvo, sizeof(s_wf_alvo), "%s", v.ssid);
    memset(s_wf_txt, 0, sizeof(s_wf_txt));
    s_wf_senha = true;
    virtual_keyboard_open(&s_kb);
    system_request_redraw_complete();
    return;
  }
  system_request_redraw();
}

static void type_wifi(const char* txt, uint8_t code)
{
  if (!s_wf_senha) { return; }
  if (code == TAB5_KEY_ENTER)  { wf_send_senha(); return; }
  if (code == TAB5_KEY_BACKSPACE) { wf_key(TECVIRT_APAGAR); return; }
  if (code == TAB5_KEY_ESC)    { wf_close_senha(); return; }
  // Password and ASCII: the letter that the dead key composed iteration to be the two keys.
  char cru[8];
  tab5_keyboard_ascii(txt, cru, sizeof(cru));
  for (const char* c = cru; *c; ++c) { if ((unsigned char)*c >= 0x20) { wf_key((unsigned char)*c); } }
}

// ---- Bluetooth -------------------------------------------------------------
static const int BT_ITEM_A = 92;
static int bt_lista_y0(void) { return CONTEUDO_Y + 84 + 84 + BTN_A + 20 + BTN_A + 20 + 44; }

static int bt_item_height(int, void*) { return BT_ITEM_A; }
static void bt_item_draw(lgfx::LovyanGFX& d, int i, int x0, int y, int width, void*)
{
  if (i < 0 || i >= s_bt_n) { return; }
  const tab5_ble_seen_t& v = s_bt[i];
  d.fillRect(x0, y, width, BT_ITEM_A, TFT_BLACK);
  char end[18], line[80], dist[12];
  tab5_ble_address_text(v.addr, end);
  // Tracker in orange: and what do you look for when you want to know "does it have a
  // etiqueta perto of mim?".
  const uint16_t color = v.meshtastic ? TFT_GREENYELLOW
                     : v.class_id == TAB5_BLE_TRACKER ? TFT_ORANGE : TFT_WHITE;
  char titulo[48];
  if (v.name[0])       { snprintf(titulo, sizeof(titulo), "%s", v.name); }
  else if (v.brand[0]) { snprintf(titulo, sizeof(titulo), "%s %s", v.class_id == TAB5_BLE_TRACKER ? "Tracker" : "Beacon", v.brand); }
  else                 { snprintf(titulo, sizeof(titulo), "(no name)"); }
  text(d, x0 + MARGEM, y + 12, 30, true, color, titulo);
  tab5_signal_distance_text(v.rssi, v.p1m ? v.p1m : TAB5_SIGNAL_P1M_BLE, dist, sizeof(dist));
  const char* tipo = v.meshtastic ? "  ·  Meshtastic" : "";
  char marca[24] = "";
  if (!v.meshtastic && v.brand[0] && v.name[0]) { snprintf(marca, sizeof(marca), "  ·  %s", v.brand); }
  snprintf(line, sizeof(line), "%s  ·  %d dBm %s%s%s", end, (int)v.rssi, dist, tipo, marca);
  text(d, x0 + MARGEM, y + 52, 22, false, color_sub(d), line);
  if (v.meshtastic) { chevron(d, x0 + width - MARGEM - 12, y + BT_ITEM_A / 2, color_sub(d)); }
  d.drawFastHLine(x0 + MARGEM, y + BT_ITEM_A - 1, width - MARGEM, color_line(d));
}

static void bt_reload(void)
{
  // Meshtastic first, depois por sinal: o que interessa stays no alto.
  const int n = tab5_ble_seen_count();
  int k = 0;
  for (int i = 0; i < n && k < TAB5_BLE_SEEN_MAX; ++i) { if (tab5_ble_get_seen(i, &s_bt[k])) { ++k; } }
  for (int i = 1; i < k; ++i) {
    tab5_ble_seen_t t = s_bt[i];
    int j = i - 1;
    // Meshtastic, then trackers, then the rest - each group by the way.
    auto grupo = [](const tab5_ble_seen_t& x) {
      return x.meshtastic ? 0 : x.class_id == TAB5_BLE_TRACKER ? 1 : 2;
    };
    auto antes = [grupo](const tab5_ble_seen_t& x, const tab5_ble_seen_t& y) {
      if (grupo(x) != grupo(y)) { return grupo(x) < grupo(y); }
      return x.rssi > y.rssi;
    };
    while (j >= 0 && antes(t, s_bt[j])) { s_bt[j + 1] = s_bt[j]; --j; }
    s_bt[j + 1] = t;
  }
  s_bt_n = k;
  list_items_changed(&s_l_bt, s_bt_n);
}

static void pag_bt(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  char sub[48], assin[160];
  est_bt(sub, sizeof(sub));
  const bool pronto = tab5_ble_ready();
  const bool varrendo = tab5_ble_scanning();
  const uint32_t ver = tab5_ble_version();
  if (ver != s_bt_ver) { s_bt_ver = ver; bt_reload(); }
  const bool via_bt = settings_mesh_transport() == SETTINGS_MESH_TRANSPORT_BLUETOOTH;
  const char* enlace = via_bt ? mesh_ble_descricao()
                     : settings_mesh_transport() == SETTINGS_MESH_TRANSPORT_INTERNET ? "Mesh over the internet" : "Mesh over the Grove cable";
  snprintf(assin, sizeof(assin), "%s|%d|%d|%d|%s|%s", sub, (int)settings_bluetooth(), (int)varrendo, s_bt_n, s_aviso, enlace);
  const bool changed = paint_changed_s(&s_p, s_assin, sizeof(s_assin), assin) || everything;
  if (changed) {
    content_limpo(d, a);
    int y = CONTEUDO_Y;
    y = info(d, a, y, "Status", sub, pronto ? TFT_GREENYELLOW : TFT_WHITE);
    y = info(d, a, y, "Mesh connection", enlace, mesh_ble_conectado() ? TFT_GREENYELLOW : TFT_WHITE);
    const bool enabled = tab5_ble_state() != TAB5_BLE_OFF;
    const bool descendo = tab5_ble_state() == TAB5_BLE_STOPPING;
    y = botao_largo(d, a, y, descendo ? "TURNING OFF..." : enabled ? "TURN OFF BLUETOOTH" : "TURN ON BLUETOOTH",
                    descendo ? TFT_DARKGREY : enabled ? TFT_ORANGE : TFT_GREENYELLOW);
    y = botao_largo(d, a, y, varrendo ? "PROCURANDO..." : "PROCURAR APARELHOS",
                    pronto && !varrendo ? TFT_SKYBLUE : TFT_DARKGREY);
    text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d),
          s_aviso[0] ? s_aviso : (s_bt_n ? "Tap a Meshtastic device (green) to connect Mesh" : "No device seen yet"));
  }
  const area_t window = { a.x, a.y + bt_lista_y0(), a.width, a.height - bt_lista_y0() };
  list_draw(&s_l_bt, d, window, TFT_BLACK, changed);
}

static void reiniciar_mesh_work(void*)
{
  tab5_mesh_stop();
  // Bluetooth e internet sobem direto (a internet espera o Wi-Fi por conta
  // propria). O cabo: a sondagem acha o C6L e sobe a serial.
  if (settings_mesh_transport() != SETTINGS_MESH_TRANSPORT_SERIAL) { tab5_mesh_start(); }
  else { system_grove_redetect(); }
}

static void touch_bt(int tx, int ty)
{
  const int l = system_area().width - 2 * MARGEM;
  int y = CONTEUDO_Y + 84 + 84;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    // Descendo: o botao espera a stack terminar (o touch repetido of quem
    // thought you didn't get it, don't stack orders).
    if (tab5_ble_state() == TAB5_BLE_STOPPING) { return; }
    system_bluetooth(tab5_ble_state() == TAB5_BLE_OFF);
    s_aviso[0] = 0;
    system_request_redraw();
    return;
  }
  y += BTN_A + 20;
  if (tab5_dentro(tx, ty, MARGEM, y, l, BTN_A)) {
    if (tab5_ble_scan(6000)) { s_aviso[0] = 0; }
    system_request_redraw();
    return;
  }
  int dy = 0;
  const int i = list_item_touched(&s_l_bt, tx, ty, &dy);
  if (i < 0 || i >= s_bt_n) { return; }
  char end[18];
  tab5_ble_address_text(s_bt[i].addr, end);
  const char* name = s_bt[i].name[0] ? s_bt[i].name : end;
  // Every touch responds: one device that the system does not know how to use says this, in
  // vez of o touch parecer perdido.
  if (!s_bt[i].meshtastic) {
    char dist[12];
    tab5_signal_distance_text(s_bt[i].rssi, s_bt[i].p1m ? s_bt[i].p1m : TAB5_SIGNAL_P1M_BLE, dist, sizeof(dist));
    if (s_bt[i].brand[0]) {
      snprintf(s_aviso, sizeof(s_aviso), "%s (%s) at %s - read-only", name, s_bt[i].brand, dist);
    } else {
      snprintf(s_aviso, sizeof(s_aviso), "%s at %s: not usable yet (Meshtastic nodes only)", name, dist);
    }
    ESP_LOGI("settings", "bluetooth: %s (%s) is not Meshtastic - nothing to do", name, end);
    system_request_redraw();
    return;
  }
  // Meshtastic: vira o no of Mesh, e o Mesh passa para o Bluetooth - touch e
  // the request to connect, and the "Connection" line follows what happens.
  settings_set_mesh_ble_address(end);
  if (settings_mesh_transport() != SETTINGS_MESH_TRANSPORT_BLUETOOTH) { settings_set_mesh_transport(SETTINGS_MESH_TRANSPORT_BLUETOOTH); }
  // Conectar liga o Bluetooth - e stays gravado, como no botao.
  if (!settings_bluetooth()) { settings_set_bluetooth(true); }
  tab5_ble_turn_on();   // I2C of trilho: aqui, in interface
  system_work_released(reiniciar_mesh_work, nullptr);
  snprintf(s_aviso, sizeof(s_aviso), "conectando a %s...", name);
  ESP_LOGI("ajustes", "bluetooth: conectar o Mesh a %s (%s)", name, end);
  system_request_redraw();
}

// ---- ESP-NOW ---------------------------------------------------------------
static void pag_espnow(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  char sub[48], assin[160], val[24];
  est_espnow(sub, sizeof(sub));
  const uint8_t f = tab5_espnow_canal_fixo();
  snprintf(assin, sizeof(assin), "%s|%u|%u", sub, (unsigned)f, (unsigned)tab5_espnow_canal());
  if (!paint_changed_s(&s_p, s_assin, sizeof(s_assin), assin) && !everything) { return; }
  content_limpo(d, a);
  int y = CONTEUDO_Y;
  y = info(d, a, y, "Current channel", sub);
  y = botao_largo(d, a, y, "USE THE ROUTER CHANNEL", f ? TFT_SKYBLUE : TFT_DARKGREY);
  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "Fixed channel");
  y += 34;
  if (f) { snprintf(val, sizeof(val), "%u", (unsigned)f); } else { snprintf(val, sizeof(val), "-"); }
  menos_mais(d, a, y, val);
  y += BTN_A + 24;
  paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM,
            "The radio stays on one channel at a time. On the router's channel, Wi-Fi and "
            "ESP-NOW work together, but other devices need to be on that same channel. "
            "A fixed channel drops the router. The choice is saved.");
}
static void touch_espnow(int tx, int ty)
{
  int y = CONTEUDO_Y + 84;
  if (tab5_dentro(tx, ty, MARGEM, y, system_area().width - 2 * MARGEM, BTN_A)) {
    tab5_espnow_set_canal(0);
    system_request_redraw();
    return;
  }
  y += BTN_A + 20 + 34;
  const int s = menos_mais_touch(tx, ty, y);
  if (s) {
    int c = tab5_espnow_canal_fixo();
    c = (c == 0) ? (s > 0 ? 1 : 13) : c + s;
    if (c < 1) { c = 13; } if (c > 13) { c = 1; }
    tab5_espnow_set_canal((uint8_t)c);
    system_request_redraw();
  }
}

// ---- Meshtastic ------------------------------------------------------------
static int mesh_y_via(void)    { return CONTEUDO_Y + 30; }
static int mesh_y_pin(void)    { return mesh_y_via() + BTN_A + 24 + 84 + 84; }
// A position of GPS para a malha exited daqui: e of grupo Localizacao of app
// Mesh (Configure the node), along with what the NO does with it - two places for the
// mesma coisa deixavam one contradizer o outro.
static int mesh_y_net(void)    { return mesh_y_pin() + 34 + BTN_A + 24; }   // bloco of internet (MQTT)

static bool s_pin_para_no = false;   // o editor abriu porque o no esta pedindo o PIN now
static char s_pin_pintado[16];       // o numero que esta in screen

// The PIN number, alone: ​​just clear its track (up to the button) and write it.
static void pin_valor(lgfx::LovyanGFX& d, const area_t& a, bool forcar)
{
  char pin[16];
  if (s_pin_editando) { snprintf(pin, sizeof(pin), "%s_", s_pin_txt); }
  else                { snprintf(pin, sizeof(pin), "%06lu", (unsigned long)settings_mesh_pin()); }
  if (!paint_changed_s(&s_p, s_pin_pintado, sizeof(s_pin_pintado), pin) && !forcar) { return; }
  const int y = mesh_y_pin() + 34;
  if (!forcar) { d.fillRect(a.x + MARGEM, a.y + y, a.width - 2 * MARGEM - 260 - 8, BTN_A, TFT_BLACK); }
  text(d, a.x + MARGEM, a.y + y + BTN_A / 2, 40, true, s_pin_editando ? TFT_YELLOW : TFT_WHITE, pin, middle_left);
}

static void pag_mesh(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  // No asked for the PIN (random mode): the keyboard opens by itself.
  if (mesh_ble_esperando_pin() && !s_pin_editando) {
    s_pin_editando = true;
    s_pin_para_no = true;
    s_pin_txt[0] = 0;
    virtual_keyboard_open(&s_kb);
    everything = true;
  }
  char assin[320], salvo[18];
  settings_mesh_ble_address(salvo);
  snprintf(assin, sizeof(assin), "%d|%s|%s|%lu|%u|%d|%s|%d|%s", (int)settings_mesh_transport(), tab5_mesh_enlace_descricao(), salvo,
           (unsigned long)settings_mesh_pin(), 0u, (int)s_pin_editando, "",
           (int)settings_mesh_internet(), tab5_mesh_internet_estado());
  // A memoria of assinatura (s_assin) guarda 159 caracteres: o que muda
  // After that (the states, which come at the end) I would not repaint. Go the HASH of
  // entire signature (FNV-1a), which fits and changes with any character.
  uint32_t h = 2166136261u;
  for (const char* c = assin; *c; ++c) { h = (h ^ (uint8_t)*c) * 16777619u; }
  snprintf(assin, sizeof(assin), "mesh|%08lx", (unsigned long)h);
  // The entered PIN stays OUTSIDE of signature: the letter only repaints the number.
  if (s_pin_editando) { virtual_keyboard_position(&s_kb, a); }
  if (!paint_changed_s(&s_p, s_assin, sizeof(s_assin), assin) && !everything) {
    if (s_pin_editando) { pin_valor(d, a, false); virtual_keyboard_paint(&s_kb, d, false); }
    return;
  }
  content_limpo(d, a);
  const int l3 = (a.width - 2 * MARGEM - 32) / 3;

  // THREE WAY. The Grove wins alone when the C6L is found on the cable; after
  // Therefore, the choice here is valid until the next finding (see settings.h).
  text(d, a.x + MARGEM, a.y + CONTEUDO_Y, 22, false, color_sub(d), "Talk to the mesh over (Grove takes priority)");
  int y = mesh_y_via();
  static const char* const ROTULO[3] = { "CABO GROVE", "BLUETOOTH", "INTERNET" };
  for (int k = 0; k < 3; ++k) {
    const bool sel = settings_mesh_transport() == k;
    tab5_botao(d, a.x + MARGEM + k * (l3 + 16), a.y + y, l3, BTN_A, ROTULO[k],
               sel ? TFT_GREENYELLOW : d.color565(50, 55, 65), sel ? TFT_BLACK : TFT_LIGHTGREY);
  }
  y += BTN_A + 24;
  y = info(d, a, y, "Link", tab5_mesh_enlace_descricao(),
           tab5_mesh_no_ar() ? TFT_GREENYELLOW : TFT_WHITE);
  y = info(d, a, y, "Bluetooth node", salvo[0] ? salvo : "the first one that shows up");

  // PIN
  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "PIN do pareamento Bluetooth");
  y += 34;
  pin_valor(d, a, true);
  tab5_botao(d, a.x + a.width - MARGEM - 260, a.y + y, 260, BTN_A,
             s_pin_editando ? "CANCELAR" : "ALTERAR", s_pin_editando ? TFT_ORANGE : TFT_SKYBLUE);
  y += BTN_A + 24;

  if (s_pin_editando) {
    paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM,
              s_pin_para_no ? "The node is asking for the PIN right now: type what it shows and tap SEND (30 s)."
                            : "Enter the node's fixed PIN (123456 on nodes with no screen) and tap SEND.");
    virtual_keyboard_paint(&s_kb, d, true);
    return;
  }

  // Internet (MQTT pelo Wi-Fi): so roda sem o no; o Grove tem priority.
  const bool net = settings_mesh_internet();
  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "Internet too when the node is unreachable (MQTT over Wi-Fi)");
  y += 34;
  text(d, a.x + MARGEM, a.y + y + BTN_A / 2, 32, true, net ? TFT_GREENYELLOW : TFT_WHITE,
        net ? "On" : "Off", middle_left);
  tab5_botao(d, a.x + a.width - MARGEM - 260, a.y + y, 260, BTN_A, net ? "TURN OFF" : "TURN ON",
             net ? TFT_ORANGE : TFT_SKYBLUE);
  y += BTN_A + 12;
  paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM, tab5_mesh_internet_estado());
  y += 100;
  if (salvo[0]) { y = botao_largo(d, a, y, "FORGET THE BLUETOOTH NODE", d.color565(50, 55, 65)); }
  paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM,
            "With the node on Bluetooth, the Grove cable is free for the GPS. What the node does "
            "(location, the Tab5's internet for it, radio): the Mesh app, Node page.");
}

static void aplicar_pin(void)
{
  if (s_pin_txt[0]) {
    const uint32_t pin = (uint32_t)atol(s_pin_txt);
    if (s_pin_para_no) {
      // PIN PEDIDO AGORA PELO NO (o que aparece in screen dele): vai so para
      // this pairing. No is saved - no generates another in next time, and one
      // numero velho salvo so faria o pareamento seguinte falhar.
      if (mesh_ble_esperando_pin()) { mesh_ble_digitar_pin(pin); }
    } else {
      // PIN FIXO, editado por conta propria (ALTERAR): stays salvo e e tentado
      // first nas proximas conexoes.
      settings_set_mesh_pin(pin);
      settings_set_mesh_pin_on_screen(false);
      mesh_ble_tentar_of_novo();
    }
  }
  s_pin_para_no = false;
  s_pin_editando = false;
  virtual_keyboard_close(&s_kb);
  system_request_redraw_complete();
}

static void pin_key(int ac)
{
  const size_t n = strlen(s_pin_txt);
  if (ac >= '0' && ac <= '9' && n < 6) { s_pin_txt[n] = (char)ac; s_pin_txt[n + 1] = 0; }
  else if (ac == TECVIRT_APAGAR && n) { s_pin_txt[n - 1] = 0; }
  else if (ac == TECVIRT_ENVIAR) { aplicar_pin(); return; }
  else if (ac == TECVIRT_FECHAR) { s_pin_editando = false; virtual_keyboard_close(&s_kb); system_request_redraw_complete(); return; }
  system_request_redraw();
}

static void touch_mesh(int tx, int ty)
{
  const area_t a = system_area();
  if (s_pin_editando && virtual_keyboard_contains(&s_kb, a.x + tx, a.y + ty)) {
    const int ac = virtual_keyboard_click(&s_kb, a.x + tx, a.y + ty);
    if (ac) { pin_key(ac); }
    return;
  }
  const int l3 = (a.width - 2 * MARGEM - 32) / 3;
  int y = mesh_y_via();
  int via = -1;
  for (int k = 0; k < 3; ++k) { if (tab5_dentro(tx, ty, MARGEM + k * (l3 + 16), y, l3, BTN_A)) { via = k; } }
  if (via >= 0 && via != settings_mesh_transport()) {
    settings_set_mesh_transport((uint8_t)via);
    if (via == SETTINGS_MESH_TRANSPORT_BLUETOOTH) {   // liga o Bluetooth, e stays gravado como no botao
      if (!settings_bluetooth()) { settings_set_bluetooth(true); }
      tab5_ble_turn_on();   // I2C of trilho: aqui, in interface
    }
    system_work_released(reiniciar_mesh_work, nullptr);
    system_request_redraw();
    return;
  }
  y = mesh_y_pin() + 34;
  if (tab5_dentro(tx, ty, a.width - MARGEM - 260, y, 260, BTN_A)) {
    s_pin_editando = !s_pin_editando;
    s_pin_txt[0] = 0;
    if (s_pin_editando) { virtual_keyboard_open(&s_kb); } else { virtual_keyboard_close(&s_kb); }
    system_request_redraw_complete();
    return;
  }
  if (s_pin_editando) { return; }
  y = mesh_y_net() + 34;
  if (tab5_dentro(tx, ty, a.width - MARGEM - 260, y, 260, BTN_A)) {
    settings_set_mesh_internet(!settings_mesh_internet());
    system_request_redraw();
    return;
  }
  y += BTN_A + 12 + 100;
  char salvo[18];
  settings_mesh_ble_address(salvo);
  if (salvo[0] && tab5_dentro(tx, ty, MARGEM, y, a.width - 2 * MARGEM, BTN_A)) {
    settings_set_mesh_ble_address("");
    if (settings_mesh_transport() == SETTINGS_MESH_TRANSPORT_BLUETOOTH) { system_work_released(reiniciar_mesh_work, nullptr); }
    system_request_redraw();
  }
}

static void type_mesh(const char* txt, uint8_t code)
{
  if (!s_pin_editando) { return; }
  const size_t n = strlen(s_pin_txt);
  if (code == TAB5_KEY_ENTER) { aplicar_pin(); return; }
  if (code == TAB5_KEY_BACKSPACE) { if (n) { s_pin_txt[n - 1] = 0; } system_request_redraw(); return; }
  if (code == TAB5_KEY_ESC) { s_pin_editando = false; virtual_keyboard_close(&s_kb); system_request_redraw_complete(); return; }
  if (txt && txt[0] >= '0' && txt[0] <= '9' && n < 6) { s_pin_txt[n] = txt[0]; s_pin_txt[n + 1] = 0; system_request_redraw(); }
}

// ---- LoRa ------------------------------------------------------------------
// Diagnostics of NU transceiver on M5-Bus. Do not use or change the service
// Meshtastic: sao aparelhos e caminhos eletricos diferentes.
static const int LORA_BOTAO_Y = 840;

// A line "sw2=G4* · sw3=G2 · sw4=G47" from table of HAL - generated,
// not copied by hand, never to clash with it again (that's how "24/G35"
// of NSS, que o esquematico provou nunca ter existido, tinha sobrevivido).
// '*' marca o switch que battery com a rota tentada now (gpio_ativo).
static void lora_line_candidatos(char* out, size_t cap, const tab5_lora_pin_t* pins, size_t n,
                                  tab5_lora_signal_t sinal, int gpio_active)
{
  size_t k = 0;
  out[0] = 0;
  for (size_t i = 0; i < n; ++i) {
    if (pins[i].signal != sinal) { continue; }
    char item[24];
    const int len = snprintf(item, sizeof(item), "%ssw%u=G%d%s", k ? " · " : "",
                             (unsigned)pins[i].switch_number, pins[i].gpio,
                             pins[i].gpio == gpio_active ? "*" : "");
    if (len <= 0 || k + (size_t)len + 1 > cap) { break; }
    memcpy(out + k, item, (size_t)len + 1);
    k += (size_t)len;
  }
}

static void pag_lora(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  const tab5_lora_status_t s = tab5_lora_status();
  tab5_lora_pin_t pins[16];
  const size_t n = tab5_lora_pins(pins, sizeof(pins) / sizeof(pins[0]));
  char assin[384];
  int pos = snprintf(assin, sizeof(assin), "%d|%u|%u|%04X|%02X|%d|%d|%d",
                     (int)s.probed, s.ioe_address, s.ioe_mask, s.ioe_uid, s.ioe_revision,
                     (int)s.radio_present, s.radio_error, s.state_error);
  for (size_t i = 0; i < n && pos > 0 && pos < (int)sizeof(assin); ++i) {
    pos += snprintf(assin + pos, sizeof(assin) - (size_t)pos, "|%d", (int)pins[i].state);
  }
  if (!paint_changed_s(&s_p, s_assin, sizeof(s_assin), assin) && !everything) { return; }

  content_limpo(d, a);
  int y = CONTEUDO_Y;
  char valor[96];
  if (!s.ioe_address) {
    snprintf(valor, sizeof(valor), "M5IOE1 not found · SX1262 not tested");
  } else if (s.radio_present) {
    snprintf(valor, sizeof(valor), "SX1262 respondeu · M5IOE1 0x%02X rev %02X",
             s.ioe_address, s.ioe_revision);
  } else {
    snprintf(valor, sizeof(valor), "M5IOE1 0x%02X · SX1262 did not respond (%d)",
             s.ioe_address, s.radio_error);
  }
  y = info(d, a, y, "Live status", valor,
           s.radio_present ? TFT_GREENYELLOW : (s.ioe_address ? TFT_ORANGE : TFT_WHITE));

  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "SW2 · M5IOE1 address (green = responded)");
  y += 32;
  static const uint8_t END[] = { 0x74, 0x73, 0x72, 0x71 };
  static const char* const AB[] = { "A0 B0", "A1 B0", "A0 B1", "A1 B1" };
  const int caixa_l = (a.width - 2 * MARGEM - 24) / 4;
  for (int i = 0; i < 4; ++i) {
    const int x = a.x + MARGEM + i * (caixa_l + 8);
    const bool vivo = (s.ioe_mask & (1u << (END[i] - 0x71))) != 0;
    d.fillRoundRect(x, a.y + y, caixa_l, 60, 8,
                    vivo ? d.color565(35, 105, 45) : d.color565(38, 42, 50));
    snprintf(valor, sizeof(valor), "%s  0x%02X", AB[i], END[i]);
    text(d, x + caixa_l / 2, a.y + y + 30, 20, vivo, TFT_WHITE, valor, middle_center);
  }
  y += 84;

  // CONFERIDO no esquematico of modulo e cruzado com o of Tab5 (M5_BUS) -
  // see the large warning in hal/tab5_lora.hpp. The switch 1 of SW1 (labeled
  // "ISO_G0" in the module) does not appear here on purpose: it does not go to pin
  // None of M5-Bus, only NSS has 3 true candidates, not 4.
  char line[128];
  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "SW1 · switches 2-4 = NSS, 5-8 = BUSY");
  y += 30;
  lora_line_candidatos(valor, sizeof(valor), pins, n, TAB5_LORA_SIGNAL_NSS, s.gpio_nss);
  snprintf(line, sizeof(line), "NSS   %s", valor);
  text(d, a.x + MARGEM, a.y + y, 24, false, TFT_WHITE, line);
  y += 30;
  lora_line_candidatos(valor, sizeof(valor), pins, n, TAB5_LORA_SIGNAL_BUSY, s.gpio_busy);
  snprintf(line, sizeof(line), "BUSY  %s", valor);
  text(d, a.x + MARGEM, a.y + y, 24, false, TFT_WHITE, line);
  y += 30;
  text(d, a.x + MARGEM, a.y + y, 20, false, color_sub(d), "SW1 switch 1 is internal to the module - no pin here");
  y += 38;

  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "SW3 · switches 9-11 = IRQ");
  y += 30;
  lora_line_candidatos(valor, sizeof(valor), pins, n, TAB5_LORA_SIGNAL_IRQ, s.gpio_irq);
  snprintf(line, sizeof(line), "IRQ   %s", valor);
  text(d, a.x + MARGEM, a.y + y, 24, false, TFT_WHITE, line);
  y += 30;
  text(d, a.x + MARGEM, a.y + y, 20, false, color_sub(d), "* is the route being tried now - check it against your switches");
  y += 46;

  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "Electrical probe · M5-Bus pin / GPIO");
  y += 30;
  const int col_l = (a.width - 2 * MARGEM) / 2;
  for (size_t i = 0; i < n; ++i) {
    const int col = i >= (n + 1) / 2;
    const int lin = col ? (int)i - (int)((n + 1) / 2) : (int)i;
    snprintf(valor, sizeof(valor), "%2u / G%-2d  %s", pins[i].mbus, pins[i].gpio,
             tab5_lora_pin_state_name(pins[i].state));
    text(d, a.x + MARGEM + col * col_l, a.y + y + lin * 30, 21, false,
          pins[i].state == TAB5_LORA_PIN_FLOATING ? TFT_DARKGREY : TFT_LIGHTGREY, valor);
  }

  tab5_botao(d, a.x + MARGEM, a.y + LORA_BOTAO_Y,
             a.width - 2 * MARGEM, BTN_A, "SONDAR E TENTAR O SX1262", TFT_SKYBLUE);
  text(d, a.x + MARGEM, a.y + LORA_BOTAO_Y + BTN_A + 12, 20, false, color_sub(d),
        "SDA/SCL are read without changing the pulls, to preserve the internal I2C.");
}

static void touch_lora(int tx, int ty)
{
  const area_t a = system_area();
  if (!tab5_dentro(tx, ty, MARGEM, LORA_BOTAO_Y, a.width - 2 * MARGEM, BTN_A)) { return; }
  tab5_lora_start();
  system_request_redraw();
}

// ---- Tela ------------------------------------------------------------------
static void pag_screen(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  char assin[64], v1[16], v2[16];
  snprintf(v1, sizeof(v1), "%u s", (unsigned)settings_screen_timeout_s());
  // The clock is IN USE, not recorded: in screen TDDI it remains not of factory
  // (tab5_panel in hal/tab5_power.hpp) and the recording is not valid.
  snprintf(v2, sizeof(v2), "%d MHz", tab5_power_dpi_clock());
  snprintf(assin, sizeof(assin), "%s|%s|%d|%d", v1, v2, (int)settings_lock_screen(), (int)settings_wallpaper());
  if (!paint_changed_s(&s_p, s_assin, sizeof(s_assin), assin) && !everything) { return; }
  content_limpo(d, a);
  int y = CONTEUDO_Y;
  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "Blank the screen after, without touch");
  y += 34; menos_mais(d, a, y, v1); y += BTN_A + 12;
  char ajuda[64];
  snprintf(ajuda, sizeof(ajuda), "From %d to %d seconds.", SETTINGS_SCREEN_TIMEOUT_MIN_S, SETTINGS_SCREEN_TIMEOUT_MAX_S);
  y = paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM, ajuda) - a.y + 24;
  // Bloqueio: alturas fixas, o touch (toque_tela) repete esta conta.
  const bool bq = settings_lock_screen();
  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "Lock screen when turning on and waking up");
  y += 34;
  text(d, a.x + MARGEM, a.y + y + BTN_A / 2, 32, true, bq ? TFT_GREENYELLOW : TFT_WHITE,
        bq ? "On" : "Off", middle_left);
  tab5_botao(d, a.x + a.width - MARGEM - 260, a.y + y, 260, BTN_A, bq ? "TURN OFF" : "TURN ON",
             bq ? TFT_ORANGE : TFT_SKYBLUE);
  y += BTN_A + 24;
  // Home screen wallpaper: CHANGE cycles through the options on a wheel.
  static const char* const PAREDE[] = { "Random", "Wires", "Waves", "Web", "Rays", "Circles", "None" };
  const uint8_t pp = settings_wallpaper();
  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "Home screen wallpaper");
  y += 34;
  text(d, a.x + MARGEM, a.y + y + BTN_A / 2, 32, true,
        pp == SETTINGS_WALLPAPER_NONE ? TFT_WHITE : TFT_GREENYELLOW, PAREDE[pp], middle_left);
  tab5_botao(d, a.x + a.width - MARGEM - 260, a.y + y, 260, BTN_A, "CHANGE", TFT_SKYBLUE);
  y += BTN_A + 24;
  text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), "Panel clock");
  y += 34; menos_mais(d, a, y, v2); y += BTN_A + 12;
  paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM,
            "Lower saves battery (less memory reading by the panel), but reduces "
            "frames per second. 60 MHz is the default.");
}
static void touch_screen(int tx, int ty)
{
  int y = CONTEUDO_Y + 34;
  int s = menos_mais_touch(tx, ty, y);
  if (s) { settings_set_screen_timeout_s((uint8_t)(settings_screen_timeout_s() + s * PASSO_TELA_S)); system_request_redraw(); return; }
  y += BTN_A + 12 + 32 + 24 + 34;   // line of botao of bloqueio
  if (tab5_dentro(tx, ty, system_area().width - MARGEM - 260, y, 260, BTN_A)) {
    settings_set_lock_screen(!settings_lock_screen());
    system_request_redraw();
    return;
  }
  y += BTN_A + 24 + 34;   // line of papel of parede
  if (tab5_dentro(tx, ty, system_area().width - MARGEM - 260, y, 260, BTN_A)) {
    settings_set_wallpaper((uint8_t)((settings_wallpaper() + 1) % (SETTINGS_WALLPAPER_NONE + 1)));
    system_request_redraw();
    return;
  }
  y += BTN_A + 24 + 34;
  s = menos_mais_touch(tx, ty, y);
  if (s) {
    int v = tab5_power_dpi_clock() + s * PASSO_RELOGIO;
    if (v < SETTINGS_PANEL_CLOCK_MIN_MHZ) { v = SETTINGS_PANEL_CLOCK_MIN_MHZ; }
    if (v > SETTINGS_PANEL_CLOCK_MAX_MHZ) { v = SETTINGS_PANEL_CLOCK_MAX_MHZ; }
    // Aplica first, grava so se deu certo: one valor que o panel recusou
    // cannot turn on the of next boot.
    if (tab5_power_dpi_clock(v)) {
      settings_set_panel_clock_mhz((uint8_t)v);
      system_request_redraw_complete();
    }
  }
}

// ---- Aplicativos -----------------------------------------------------------
// The list comes from the complete table of apps, never from home already filtered.
static const int APPS_ITEM_A = 92;
static const int APPS_LISTA_Y = CONTEUDO_Y + 52;

static int apps_item_height(int, void*) { return APPS_ITEM_A; }

static void apps_item_draw(lgfx::LovyanGFX& d, int i, int x0, int y, int width, void*)
{
  if (i < 0 || i >= SISTEMA_NUM_APPS) { return; }
  const char* name = SISTEMA_APPS[i]->name;
  const bool ajustes = strcasecmp(name, "Settings") == 0;
  const bool oculto = !ajustes && name_em_lista(s_apps_ocultos, name);
  d.fillRect(x0, y, width, APPS_ITEM_A, TFT_BLACK);
  text(d, x0 + MARGEM, y + APPS_ITEM_A / 2, 30, true,
        oculto ? color_sub(d) : TFT_WHITE, name, middle_left);
  const int bl = 230, ba = 60;
  tab5_botao(d, x0 + width - MARGEM - bl, y + (APPS_ITEM_A - ba) / 2, bl, ba,
             ajustes ? "ALWAYS VISIBLE" : oculto ? "SHOW" : "HIDE",
             ajustes ? TFT_DARKGREY : oculto ? TFT_SKYBLUE : TFT_ORANGE);
  d.drawFastHLine(x0 + MARGEM, y + APPS_ITEM_A - 1, width - 2 * MARGEM, color_line(d));
}

static void apps_alternar(int alvo)
{
  if (alvo < 0 || alvo >= SISTEMA_NUM_APPS) { return; }
  // Adjustments always remain visible so the person does not lose the path of iteration
  // until this page.
  if (strcasecmp(SISTEMA_APPS[alvo]->name, "Settings") == 0) { return; }

  char novos[sizeof(s_apps_ocultos)];
  size_t k = 0;
  novos[0] = 0;
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const char* name = SISTEMA_APPS[i]->name;
    if (strcasecmp(name, "Settings") == 0) { continue; }
    bool oculto = name_em_lista(s_apps_ocultos, name);
    if (i == alvo) { oculto = !oculto; }
    if (!oculto) { continue; }
    const size_t n = strlen(name);
    if (k + n + 2 > sizeof(novos)) { return; }
    if (k) { novos[k++] = ','; }
    memcpy(novos + k, name, n);
    k += n;
    novos[k] = 0;
  }
  snprintf(s_apps_ocultos, sizeof(s_apps_ocultos), "%s", novos);
  settings_set_apps_hidden(s_apps_ocultos);
  list_item_changed(&s_l_apps, alvo);
  system_home_apps_reload();
}

static void pag_apps(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  if (everything) {
    content_limpo(d, a);
    text(d, a.x + MARGEM, a.y + CONTEUDO_Y + 10, 22, false, color_sub(d),
          "Choose which apps show up on the home screen.");
  }
  const area_t window = { a.x, a.y + APPS_LISTA_Y, a.width, a.height - APPS_LISTA_Y };
  list_draw(&s_l_apps, d, window, TFT_BLACK, everything);
}

static void touch_apps(int tx, int ty)
{
  int dy = 0;
  const int i = list_item_touched(&s_l_apps, tx, ty, &dy);
  if (i >= 0) { apps_alternar(i); }
}

// ---- Teclado ---------------------------------------------------------------
static void pag_keyboard(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  char sub[48], assin[64];
  est_keyboard(sub, sizeof(sub));
  if (settings_keyboard() && tab5_keyboard_present()) {
    snprintf(sub, sizeof(sub), "Docked (firmware %u)", (unsigned)tab5_keyboard_version());
  }
  snprintf(assin, sizeof(assin), "%s|%d", sub, (int)settings_keyboard());
  if (!paint_changed_s(&s_p, s_assin, sizeof(s_assin), assin) && !everything) { return; }
  content_limpo(d, a);
  int y = info(d, a, CONTEUDO_Y, "Status", sub, tab5_keyboard_present() ? TFT_GREENYELLOW : TFT_WHITE);
  y = botao_largo(d, a, y, settings_keyboard() ? "DISABLE" : "ENABLE",
                  settings_keyboard() ? TFT_ORANGE : TFT_GREENYELLOW);
  paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM,
            "The keyboard docks on ExtPort1. Disabled, the system does not look for it "
            "and the on-screen keyboard is used in conversations.");
}
static void touch_keyboard(int tx, int ty)
{
  if (tab5_dentro(tx, ty, MARGEM, CONTEUDO_Y + 84, system_area().width - 2 * MARGEM, BTN_A)) {
    const bool novo = !settings_keyboard();
    settings_set_keyboard(novo);
    tab5_keyboard_enable(novo);
    system_request_redraw();
  }
}

// ---- Grove -----------------------------------------------------------------
static void pag_grove(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  char sub[48], assin[64];
  est_grove(sub, sizeof(sub));
  snprintf(assin, sizeof(assin), "%s|%d", sub, (int)settings_grove());
  if (!paint_changed_s(&s_p, s_assin, sizeof(s_assin), assin) && !everything) { return; }
  content_limpo(d, a);
  int y = info(d, a, CONTEUDO_Y, "Status", sub);
  y = botao_largo(d, a, y, settings_grove() ? "TURN OFF THE 5V" : "TURN ON THE 5V",
                  settings_grove() ? TFT_ORANGE : TFT_GREENYELLOW);
  paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM,
            "On, the cable module (C6L, GPS) stays powered at all times and does not reset "
            "on every use. Off saves battery when nothing is plugged in.");
}
static void touch_grove(int tx, int ty)
{
  if (tab5_dentro(tx, ty, MARGEM, CONTEUDO_Y + 84, system_area().width - 2 * MARGEM, BTN_A)) {
    settings_set_grove(!settings_grove());
    system_reapply_profile();
    system_grove_redetect();
    system_request_redraw();
  }
}

// ---- Sobre -----------------------------------------------------------------
static void pag_sobre(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  char assin[96], memory[48], ps[48], active[32];
  const size_t li = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t lp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  snprintf(memory, sizeof(memory), "%u KB livres", (unsigned)(li / 1024));
  snprintf(ps, sizeof(ps), "%u KB livres", (unsigned)(lp / 1024));
  const uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
  snprintf(active, sizeof(active), "%lu h %02lu min", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
  snprintf(assin, sizeof(assin), "%s|%s|%s", memory, ps, active);
  if (!paint_changed_s(&s_p, s_assin, sizeof(s_assin), assin) && !everything) { return; }
  content_limpo(d, a);
  int y = CONTEUDO_Y;
  char sistema[48];
  snprintf(sistema, sizeof(sistema), "M5tabOS %s", esp_app_get_description()->version);
  y = info(d, a, y, "System", sistema);
  y = info(d, a, y, "Built on", m5tabos_compiled);
  y = info(d, a, y, "ESP-IDF", esp_get_idf_version());
  y = info(d, a, y, "Internal memory", memory);
  y = info(d, a, y, "PSRAM", ps);
  y = info(d, a, y, "Uptime", active);
  paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM,
            "M5Stack Tab5: ESP32-P4 (system) and ESP32-C6 (Wi-Fi, Bluetooth, and ESP-NOW).");
}

// ---- USB (pendrive) --------------------------------------------------------
// A screen of pendrive inteira: estado, transferencia, o diario das gravacoes
// saved, the result of last iteration and the setting of enable alone. THE
// work mora no servico (services/tab5_flashdrive) - esta pagina so pede e
// mostra, e a pessoa pode exit dela a vontade: o pendrive segue em segundo
// plano. A CONFIRMACAO of return sem o computador ter ejetado e daqui.
typedef struct { int x, y, l, a; } usb_ret_t;
static usb_ret_t s_usb_bt[3];          // os botoes como desenhados (coordenadas of area)
static bool      s_usb_confirm = false;

static void kb_txt(char* o, size_t t, uint64_t b)
{
  if (b < 1024ULL * 1024)             { snprintf(o, t, "%u KB", (unsigned)(b / 1024)); }
  else if (b < 1024ULL * 1024 * 1024) { snprintf(o, t, "%.1f MB", (double)b / (1024.0 * 1024.0)); }
  else                                { snprintf(o, t, "%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0)); }
}

static int usb_botao(lgfx::LovyanGFX& d, const area_t& a, int i, int y, const char* txt, uint16_t color)
{
  s_usb_bt[i] = { MARGEM, y, a.width - 2 * MARGEM, BTN_A };
  tab5_botao(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM, BTN_A, txt, color);
  return y + BTN_A + 20;
}

static void pag_usb(lgfx::LovyanGFX& d, const area_t& a, bool everything)
{
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  tab5_sd_summary_t r;
  tab5_sd_summary(&r);
  if (e.fase != PENDRIVE_ATIVO) { s_usb_confirm = false; }
  // Opened with one result waiting: the person saw - the warning of bar exit.
  if (e.fase == PENDRIVE_PARADO) { tab5_pendrive_resultado_visto(); }

  // O QUE MOSTRAR
  char estado[64], l2[96] = "", l3[80] = "", end[96] = "";
  uint16_t color = TFT_WHITE;
  switch (e.fase) {
    case PENDRIVE_PREPARANDO: snprintf(estado, sizeof(estado), "Preparing the card..."); break;
    case PENDRIVE_DEVOLVENDO: snprintf(estado, sizeof(estado), "Returning the card..."); break;
    case PENDRIVE_ATIVO:
      if (!e.usb.montado)       { snprintf(estado, sizeof(estado), "Waiting for the computer"); color = TFT_YELLOW; }
      else if (e.usb.suspenso)  { snprintf(estado, sizeof(estado), "Computer stalled or cable unplugged"); color = TFT_ORANGE; }
      else if (e.usb.active)     { snprintf(estado, sizeof(estado), "Copying..."); color = TFT_SKYBLUE; }
      else                      { snprintf(estado, sizeof(estado), "Card on the computer"); color = TFT_GREENYELLOW; }
      break;
    default:
      snprintf(estado, sizeof(estado), "%s", e.oferta ? "Computador conectado" : "Desligado");
      color = e.oferta ? TFT_YELLOW : TFT_WHITE;
      break;
  }
  if (e.fase == PENDRIVE_ATIVO) {
    char li[24], gr[24];
    kb_txt(li, sizeof(li), e.usb.lidos);
    kb_txt(gr, sizeof(gr), e.usb.gravados);
    snprintf(l2, sizeof(l2), "Read %s  ·  written %s%s", li, gr, e.usb.erros ? "  ·  ERRORS" : "");
  }
  if (e.fase != PENDRIVE_PARADO || r.in_journal) {
    snprintf(l3, sizeof(l3), "%u write(s) held (%u of %u KB)",
             (unsigned)r.in_journal, (unsigned)(r.journal_used / 1024), (unsigned)(r.journal_total / 1024));
  }
  // DIAGNOSIS: how far the computer has gotten, in the order in which it does it. And the
  // which says, on a disk that does not appear, if the problem is with the USB (or
  // reconheceu), no request of disco ou in leitura (o sistema dele desistiu).
  char diag[96] = "";
  if (e.fase == PENDRIVE_ATIVO) {
    const tab5_pendrive_usb_t& u = e.usb;
    if (!u.n_config)            { snprintf(diag, sizeof(diag), "USB: the computer did not recognize the device"); }
    else if (!u.n_inquiry)      { snprintf(diag, sizeof(diag), "USB: recognized it, but never asked for the disk"); }
    else if (!u.n_respostas)    { snprintf(diag, sizeof(diag), "USB: asked for the disk %ux, the response never arrived",
                                           (unsigned)u.n_inquiry); }
    else if (!u.n_capacidade)   { snprintf(diag, sizeof(diag), "USB: asked for the disk, not the size (resp. %u, last 0x%02X)",
                                           (unsigned)u.n_respostas, (unsigned)u.op_respondido); }
    else if (!u.n_leituras)     { snprintf(diag, sizeof(diag), "USB: saw the disk, hasn't read yet"); }
    else                        { snprintf(diag, sizeof(diag), "USB: %u read(s), %u delivered, slowest %u ms",
                                           (unsigned)u.n_cmd_leitura, (unsigned)u.n_leituras_ok,
                                           (unsigned)u.leitura_max_ms); }
    // It stopped more than 3 s ago: it says WHERE - and what separates a crash from USB
    // (or of card) of computer that simply stopped of request.
    if (u.n_config && u.parado_ms >= 3000) {
      switch (u.onde) {
        case PENDRIVE_ESPERA_CARTAO:
          snprintf(diag, sizeof(diag), "USB: STALLED on the card (sector %u)", (unsigned)u.ultimo_setor); break;
        case PENDRIVE_ESPERA_USB:
          snprintf(diag, sizeof(diag), "USB: STALLED on USB, delivering sector %u (%u/%u)",
                   (unsigned)u.ultimo_setor, (unsigned)u.n_leituras_ok, (unsigned)u.n_cmd_leitura); break;
        default:
          if (u.n_cmd_leitura) {
            snprintf(diag, sizeof(diag), "USB: the computer stopped asking (%u/%u reads)",
                     (unsigned)u.n_leituras_ok, (unsigned)u.n_cmd_leitura);
          }
          break;
      }
    }
    if (u.op_recusado) {
      const size_t k = strlen(diag);
      snprintf(diag + k, sizeof(diag) - k, " · recusou 0x%02X", (unsigned)u.op_recusado);
    }
  }
  char perdas[64] = "";
  if (r.lost) {
    snprintf(perdas, sizeof(perdas), "%u write(s) lost: memory full", (unsigned)r.lost);
  }
  if (e.fase == PENDRIVE_PARADO) {
    switch (e.reason) {
      case PENDRIVE_RECUSA_APP:        snprintf(end, sizeof(end), "Close the app that's using the card (Files, Camera)."); break;
      case PENDRIVE_RECUSA_EM_USO:     snprintf(end, sizeof(end), "The card was in use. Try again."); break;
      case PENDRIVE_RECUSA_SEM_CARTAO: snprintf(end, sizeof(end), "No SD card, or it did not respond."); break;
      case PENDRIVE_RECUSA_SEM_DIARIO: snprintf(end, sizeof(end), "Missing the flash-drive journal: reflash the firmware."); break;
      case PENDRIVE_RECUSA_USB:        snprintf(end, sizeof(end), "USB did not start. The card stays on the Tab5."); break;
      default:
        if (e.sem_ejetar) {
          // See tab5_flashdrive.cpp (s_saiu_sem_ejetar): the disk does not appear
          // soon of next time and the computer checking the card.
          snprintf(end, sizeof(end), "Removed without ejecting: the computer checks the card before opening it.");
        } else if (r.complete) {
          snprintf(end, sizeof(end), "Last time: %u write(s) applied%s%s",
                   (unsigned)r.applied, r.conflicts ? ", conflict in " : "",
                   r.conflicts ? ".tab5 file(s)" : "");
        }
        break;
    }
  }

  char assin[sizeof(s_assin)];
  snprintf(assin, sizeof(assin), "%d|%d|%d|%s|%s|%s|%s|%d|%u|%u|%s", (int)e.fase, (int)e.reason, (int)s_usb_confirm,
           estado, l2, l3, end, (int)settings_flash_drive(), (unsigned)r.conflicts, (unsigned)r.lost, diag);
  if (!paint_changed_s(&s_p, s_assin, sizeof(s_assin), assin) && !everything) { return; }
  content_limpo(d, a);
  memset(s_usb_bt, 0, sizeof(s_usb_bt));

  int y = info(d, a, CONTEUDO_Y, "SD card as a flash drive (USB-C)", estado, color);
  if (l2[0])  { text(d, a.x + MARGEM, a.y + y, 26, false, TFT_WHITE, l2); y += 44; }
  if (l3[0])  { text(d, a.x + MARGEM, a.y + y, 24, false, color_sub(d), l3); y += 44; }
  if (diag[0])   { text(d, a.x + MARGEM, a.y + y, 22, false, color_sub(d), diag); y += 40; }
  if (perdas[0]) { text(d, a.x + MARGEM, a.y + y, 24, false, TFT_RED, perdas); y += 44; }
  if (end[0]) { text(d, a.x + MARGEM, a.y + y, 24, false, TFT_ORANGE, end); y += 44; }
  y += 8;

  switch (e.fase) {
    case PENDRIVE_PARADO:
      y = usb_botao(d, a, 0, y, "USE AS A FLASH DRIVE", TFT_SKYBLUE);
      break;
    case PENDRIVE_ATIVO:
      if (s_usb_confirm) {
        y = paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM,
                      "The computer did not report an eject. If it had anything cached, files "
                      "may be incomplete. Eject on the computer and the card returns by itself. Already "
                      "used \"Safely Remove\" on Windows? You can return it.") - a.y + 16;
        y = usb_botao(d, a, 0, y, "GO BACK", TFT_SKYBLUE);
        y = usb_botao(d, a, 1, y, "RETURN ANYWAY", TFT_RED);
      } else {
        y = usb_botao(d, a, 0, y, "RETURN THE CARD TO THE TAB5", TFT_ORANGE);
      }
      break;
    default:
      y = usb_botao(d, a, 0, y, "PLEASE WAIT...", TFT_DARKGREY);
      s_usb_bt[0] = { };   // does not respond
      break;
  }
  y = usb_botao(d, a, 2, y, settings_flash_drive() ? "TURN ON BY ITSELF ON THE COMPUTER: YES" : "TURN ON BY ITSELF ON THE COMPUTER: NO",
                settings_flash_drive() ? TFT_GREENYELLOW : TFT_LIGHTGREY);
  paragrafo(d, a.x + MARGEM, a.y + y, a.width - 2 * MARGEM,
            "The Tab5 stays usable in the meantime. Whatever it writes stays in internal memory and "
            "goes to the card when it returns. Files and Camera wait for the card. To finish, "
            "eject the disk on the computer.");
}

static bool usb_em(int i, int tx, int ty)
{
  const usb_ret_t& b = s_usb_bt[i];
  return b.l > 0 && tab5_dentro(tx, ty, b.x, b.y, b.l, b.a);
}

static void touch_usb(int tx, int ty)
{
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  if (usb_em(2, tx, ty)) {
    settings_set_flash_drive(!settings_flash_drive());
    system_request_redraw();
    return;
  }
  if (e.fase == PENDRIVE_PARADO && usb_em(0, tx, ty)) {
    tab5_pendrive_start();
    system_request_redraw();
    return;
  }
  if (e.fase == PENDRIVE_ATIVO) {
    const bool montado = e.usb.montado && !e.usb.ejetado;
    if (!s_usb_confirm && usb_em(0, tx, ty)) {
      if (montado) { s_usb_confirm = true; } else { tab5_pendrive_return(); }
    } else if (s_usb_confirm && usb_em(0, tx, ty)) {
      s_usb_confirm = false;
    } else if (s_usb_confirm && usb_em(1, tx, ty)) {
      ESP_LOGW("settings", "card returned without the computer ejecting it");
      s_usb_confirm = false;
      tab5_pendrive_return();
    }
    system_request_redraw();
  }
}

// ---------------------------------------------------------------------------
// DESPACHO
// ---------------------------------------------------------------------------
typedef void (*pag_draw_fn)(lgfx::LovyanGFX&, const area_t&, bool);
typedef void (*pag_touch_fn)(int, int);
static const pag_draw_fn PAG_DESENHAR[PAG_N] = {
  pag_wifi, pag_bt, pag_espnow, pag_mesh, pag_lora, pag_screen, pag_apps, pag_keyboard, pag_grove, pag_usb, pag_sobre };
static const pag_touch_fn PAG_TOCAR[PAG_N] = {
  touch_wifi, touch_bt, touch_espnow, touch_mesh, touch_lora, touch_screen, touch_apps, touch_keyboard, touch_grove, touch_usb, nullptr };

static void open_pagina(int p)
{
  if (s_pin_editando) { s_pin_editando = false; virtual_keyboard_close(&s_kb); }
  s_pag = p;
  s_assin[0] = 0;
  s_aviso[0] = 0;
  if (s_wf_senha) { s_wf_senha = false; memset(s_wf_txt, 0, sizeof(s_wf_txt)); virtual_keyboard_close(&s_kb); }
  if (p == PAG_WIFI) {
    list_set(&s_l_wifi, ROLAGEM_INICIO, 0, wf_item_height, wf_item_draw, nullptr);
    s_wf_ver = 0;
    tab5_wifi_search();   // entrar in pagina ja procura
  } else {
    list_hide(&s_l_wifi);
  }
  if (p == PAG_BLUETOOTH) {
    list_set(&s_l_bt, ROLAGEM_INICIO, 0, bt_item_height, bt_item_draw, nullptr);
    s_bt_ver = 0;
    // Entrar in pagina ja procura: e o que se quer ver on open.
    if (tab5_ble_ready()) { tab5_ble_scan(6000); }
  } else {
    list_hide(&s_l_bt);
  }
  if (p == PAG_APPS) {
    settings_apps_hidden(s_apps_ocultos, sizeof(s_apps_ocultos));
    list_set(&s_l_apps, ROLAGEM_INICIO, SISTEMA_NUM_APPS,
                  apps_item_height, apps_item_draw, nullptr);
  } else {
    list_hide(&s_l_apps);
  }
  if (p == PAG_LORA) { tab5_lora_start(); }
  if (p == PAG_INICIO) { s_ub = s_uv = -1; }
  else { list_hide(&s_lista); }
  paint_request(&s_p);
  system_request_redraw_complete();
}

static bool on_open(void)
{
  s_pag = PAG_INICIO;
  s_ub = s_uv = -1;
  settings_apps_hidden(s_apps_ocultos, sizeof(s_apps_ocultos));
  memset(s_sub, 0, sizeof(s_sub));
  for (int i = 0; i < PAG_N; ++i) { PAGINAS[i].state(s_sub[i], sizeof(s_sub[i])); }
  list_set(&s_lista, ROLAGEM_INICIO, PAG_N, item_height, item_draw, nullptr);
  paint_request(&s_p);
  // The pendrive warning (status bar) opens Settings: it goes straight to the page.
  if (tab5_pendrive_pede_atencao()) { open_pagina(PAG_USB); }
  return true;
}

static void on_close(void)
{
  if (s_pin_editando) { s_pin_editando = false; virtual_keyboard_close(&s_kb); }
  if (s_wf_senha) { s_wf_senha = false; memset(s_wf_txt, 0, sizeof(s_wf_txt)); virtual_keyboard_close(&s_kb); }
}

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  // O NO PEDIU O PIN: em qualquer pagina of Ajustes, vai para a of
  // Meshtastic, que abre o keyboard (o pareamento espera ~30 s).
  if (mesh_ble_esperando_pin() && s_pag != PAG_MESH) { open_pagina(PAG_MESH); }
  paint_start(&s_p);
  const bool everything = paint_complete(&s_p);
  if (s_pag == PAG_INICIO) { draw_start(d, a, everything); return; }
  if (everything) { cabecalho(d, a, PAGINAS[s_pag].name); }
  PAG_DESENHAR[s_pag](d, a, everything);
}

static void on_touch(int tx, int ty)
{
  if (s_pag == PAG_INICIO) {
    if (ty < DIVISOR_Y) {
      mexer_rapido(ty < RAPIDO_Y0 + RAPIDO_A ? RAPIDO_Y0 : RAPIDO_Y0 + RAPIDO_A, tx, ty, false);
      return;
    }
    int dy = 0;
    const int i = list_item_touched(&s_lista, tx, ty, &dy);
    if (i >= 0 && i < PAG_N) { open_pagina(i); }
    return;
  }
  if (!s_pin_editando && !s_wf_senha && tab5_dentro(tx, ty, 16, 16, VOLTAR_L, VOLTAR_A)) { open_pagina(PAG_INICIO); return; }
  if (PAG_TOCAR[s_pag]) { PAG_TOCAR[s_pag](tx, ty); }
}

static void on_touch_level(int x, int y, bool pressed)
{
  // Teclado open (senha of Wi-Fi ou PIN): os fingers sao dele.
  if (virtual_keyboard_is_open(&s_kb)) {
    int actions[8];
    const int n = virtual_keyboard_level(&s_kb, actions, 8);
    for (int i = 0; i < n && virtual_keyboard_is_open(&s_kb); ++i) {
      if (s_wf_senha)          { wf_key(actions[i]); }
      else if (s_pin_editando) { pin_key(actions[i]); }
    }
    return;
  }
  if (s_pag == PAG_INICIO) {
    // Arrastar o finger in barra of brilho/volume ajusta on vivo.
    if (pressed && y >= 0 && y < DIVISOR_Y) {
      mexer_rapido(y < RAPIDO_Y0 + RAPIDO_A ? RAPIDO_Y0 : RAPIDO_Y0 + RAPIDO_A, x, y, true);
    }
    if (list_drag(&s_lista, x, y, pressed)) { system_request_redraw(); }
  } else if (s_pag == PAG_BLUETOOTH) {
    if (list_drag(&s_l_bt, x, y, pressed)) { system_request_redraw(); }
  } else if (s_pag == PAG_WIFI && !s_wf_senha) {
    if (list_drag(&s_l_wifi, x, y, pressed)) { system_request_redraw(); }
  } else if (s_pag == PAG_APPS) {
    if (list_drag(&s_l_apps, x, y, pressed)) { system_request_redraw(); }
  }
}

static void on_type(const char* txt, uint8_t code, uint8_t)
{
  if (s_pag == PAG_MESH) { type_mesh(txt, code); }
  if (s_pag == PAG_WIFI) { type_wifi(txt, code); }
}

// ---------------------------------------------------------------------------
// ICONE DA TELA INICIAL
// ---------------------------------------------------------------------------
// Primitivas e fracao of lado, como a marca of Mesh: exit limpo em qualquer
// size, no flash and no serration. The big letter that was here was
// the embedded font magnified 6x - each pixel of it became a square of 6.

// GEARS THAT REALLY MESH (third version).
//
// The previous one had the right composition - large, and two smaller ones on
// lado, como in referencia of icon of ajustes classico - mas as tres se
// SOBREPUNHAM: a distancia entre centros era bem menor que a soma dos raios, e
// viravam one borrao so. Agora e engrenagem of verdade:
//   - MESMO PASSO of dente nas tres (PASSO, % of lado): so assim o dente of
//     one fits in the space of another; the radius of each exit of number of teeth;
//   - distancia entre centros = soma dos raios primitivos (onde os dentes se
//     encontram);
//   - FASE calculada: a grande aponta one dente para a media, a media poe one
//     GO there; for the small one, the phase compensates for where the big tooth fell out.
// Dente TRAPEZIOIDAL (mais largo in raiz que in ponta), bottom e largo - o
// thin and long tooth of first test read like sun, not gear.
// Prototyped in a before image (the same account), also checked at 96 px.
static const float ENG_PASSO = 10.5f;   // passo circular, % of lado
static const float ENG_DENTE = 6.0f;    // height of dente, % of lado

static float eng_raio(int n) { return (float)n * ENG_PASSO / (2.0f * (float)M_PI); }

static void draw_engrenagem(lgfx::LovyanGFX& d, int x0, int y0, int lado,
                                float cx, float cy, int n, float fase_graus,
                                uint16_t escuro, uint16_t fundo)
{
  const float k  = (float)lado / 100.0f;   // % of lado -> pixels
  const float rp = eng_raio(n);
  const float rr = rp - ENG_DENTE / 2.0f;  // raiz
  const float rt = rp + ENG_DENTE / 2.0f;  // ponta
  auto px = [&](float v) { return x0 + (int)lroundf(v * k); };
  auto py = [&](float v) { return y0 + (int)lroundf(v * k); };

  d.fillCircle(px(cx), py(cy), (int)lroundf(rr * k), escuro);
  const float passo = 360.0f / (float)n;
  const float hb = passo * 0.36f * (float)M_PI / 180.0f;   // meia width in raiz
  const float ht = passo * 0.22f * (float)M_PI / 180.0f;   // meia width in ponta
  for (int i = 0; i < n; ++i) {
    const float a = (fase_graus + (float)i * passo) * (float)M_PI / 180.0f;
    const int x1 = px(cx + rr * cosf(a - hb)), y1 = py(cy + rr * sinf(a - hb));
    const int x2 = px(cx + rt * cosf(a - ht)), y2 = py(cy + rt * sinf(a - ht));
    const int x3 = px(cx + rt * cosf(a + ht)), y3 = py(cy + rt * sinf(a + ht));
    const int x4 = px(cx + rr * cosf(a + hb)), y4 = py(cy + rr * sinf(a + hb));
    d.fillTriangle(x1, y1, x2, y2, x3, y3, escuro);
    d.fillTriangle(x1, y1, x3, y3, x4, y4, escuro);
  }
  // Furo of middle, in color of fundo.
  d.fillCircle(px(cx), py(cy), (int)lroundf(rr * 0.40f * k), fundo);
}

static void icon_config(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo  = d.color565(250, 205, 60);
  const uint16_t escuro = d.color565(50, 42, 10);
  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  // Grande embaixo a left; a media engrena nela para cima-direita (-40
  // graus), a pequena para baixo-direita (+20). Folgas conferidas: none
  // slopes on a rounded edge, and medium and small do not touch.
  const int   nG = 14, nM = 9, nP = 6;
  const float gx = 40.0f, gy = 58.0f;
  const float t1 = -40.0f, t2 = 20.0f;
  const float rG = eng_raio(nG), rM = eng_raio(nM), rP = eng_raio(nP);
  const float r2g = (float)M_PI / 180.0f;

  const float mx = gx + (rG + rM) * cosf(t1 * r2g), my = gy + (rG + rM) * sinf(t1 * r2g);
  const float sx = gx + (rG + rP) * cosf(t2 * r2g), sy = gy + (rG + rP) * sinf(t2 * r2g);

  const float fG = t1;                              // dente of grande apontando para a media
  const float fM = t1 + 180.0f + 180.0f / nM;       // vao of media no mesmo ponto
  const float pG = 360.0f / nG;
  const float dlt = t2 - (fG + roundf((t2 - fG) / pG) * pG);   // where the big tooth fell out
  const float fP = (t2 + 180.0f - dlt * rG / rP) + 180.0f / nP;

  draw_engrenagem(d, x, y, lado, gx, gy, nG, fG, escuro, fundo);
  draw_engrenagem(d, x, y, lado, mx, my, nM, fM, escuro, fundo);
  draw_engrenagem(d, x, y, lado, sx, sy, nP, fP, escuro, fundo);
}


// 'extern' is REQUIRED: in C++ a const in the scope of file has a link
// internal, and apps.cpp would not see this symbol.
// THE RHYTHM OF EACH PAGE. It was 500 ms in all: the interface woke up 2x/s with
// Ajustes open, mesmo parada in pagina of Tela (que so muda pelo touch, e o
// touch already asks for redraw). Now: 0 where nothing changes on its own, 500 ms is where
// algo anda depressa (procura of redes, varredura BLE, pendrive em uso,
// sondagem of Grove), 1 s no resto (estados of Wi-Fi/BLE/Mesh, clock of
// On). Each page continues comparing the signature before painting.
static uint32_t current_redraw_period(void)
{
  if (system_grove_probing()) { return 500; }
  switch (s_pag) {
    case PAG_TELA:      return 0;
    case PAG_APPS:      return 0;
    case PAG_WIFI:      return tab5_wifi_searching() ? 500 : 1000;
    case PAG_BLUETOOTH: return tab5_ble_scanning() ? 500 : 1000;
    case PAG_USB: {
      tab5_pendrive_estado_t e;
      tab5_pendrive_estado(&e);
      return e.fase != PENDRIVE_PARADO ? 500 : 1000;
    }
    default:            return 1000;
  }
}

extern const app_t APP_CONFIG = {
  .name  = "Settings",
  .color   = TFT_YELLOW,
  .profile = { .speaker = true },
  .uses_sd = false,
  .sleep_clock_floor_mhz = 0,
  .on_open  = on_open,
  .on_close = on_close,
  .draw  = draw,
  .on_touch  = on_touch,
  .on_touch_level = on_touch_level,
  .on_type = on_type,
  .redraw_period_ms = 1000,                // o default; periodo_agora decide por pagina
  .current_redraw_period = current_redraw_period,
  .height_content = 0,   // does not scroll completely: the list and pages scroll inside
  .icon = icon_config,
};
