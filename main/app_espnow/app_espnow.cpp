// APP: ESP-NOW - a window of service services/tab5_espnow.
//
// A thin bar of information at the top (status, channel, MAC, who has been
// heard) and the conversation below - balloons, touch to write, keyboard. A
// conversation and the system component (kernel/conversation): this file only says
// WHAT messages exist and HOW to send. The radio, the history on the card and the
// warning with the app closed are of service.
//
// BROADCAST sending: every device on the same channel hears it (the Cardputer with the
// official firmware of ESP-NOW, for example). In broadcast there is no ACK - the
// green checkmark says "exited by antenna", not "someone received".
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

// In PSRAM: ~1 KB (draft, copy from it to "changed", keyboard) which is just the
// task of interface touches, and does not need to live in internal RAM with the app closed.
EXT_RAM_BSS_ATTR static conversation_t s_c;
static paint_t  s_p;
static char       s_status_text[128];
static int        s_status_state = -1;

// 88 px, "ESP-NOW" in 3.6 and the pill in 3.2 (it was 56/2.2, then 72/3.0 -
// too small to read at a distance of use).
static const int STATUS_BAR_HEIGHT = 88;

// THE CHANNEL SELECTOR, in bar: each touch moves to the next. 0 = o of
// router; 1, 6 and 11 are the channels that do not overlap in 2.4 GHz - and the
// defaults of most devices (one ESP32 alone stays at 1).
static const uint8_t CHANNEL_OPTIONS[] = { 0, 1, 6, 11 };
static int s_pilula_x0 = -1, s_pilula_x1 = -1;   // where it was drawn (x of area)

// Stable color per device: the same MAC always has the same color on the label, and
// at a table with three Cardputers to know who and who at a glance.
static uint16_t color_of_mac(const uint8_t mac[6])
{
  static const uint16_t DEVICE_COLORS[] = { TFT_SKYBLUE, TFT_ORANGE, TFT_GREENYELLOW, TFT_PINK,
                                    TFT_GOLD, TFT_CYAN, TFT_VIOLET, TFT_SILVER };
  const unsigned h = (unsigned)mac[3] * 31u + (unsigned)mac[4] * 7u + (unsigned)mac[5];
  return DEVICE_COLORS[h % (sizeof(DEVICE_COLORS) / sizeof(DEVICE_COLORS[0]))];
}

// ---- what the conversation component asks ----------------------------
static int count(void*) { return tab5_espnow_n(); }

static bool read(int i, conversation_msg_t* out, void*)
{
  static espnow_msg_t m, ant;   // outside of stack of interface
  if (!tab5_espnow_msg(i, &m)) { return false; }
  snprintf(out->text, sizeof(out->text), "%s", m.text);
  out->minha  = m.minha;
  out->when = m.when;
  out->send_state  = m.send_state;   // espnow_envio_t and conversation_envio_t have the same order
  out->name[0] = 0;
  // Label only when the sender changes - a sequence of the same device does not
  // repeats the name in each bubble.
  if (!m.minha) {
    const bool same_sender = (i > 0) && tab5_espnow_msg(i - 1, &ant) && !ant.minha
                    && memcmp(ant.mac, m.mac, 6) == 0;
    if (!same_sender) {
      snprintf(out->name, sizeof(out->name), "%02X%02X", m.mac[4], m.mac[5]);
      out->color_name = color_of_mac(m.mac);
    }
  }
  // The DIALECT of each received, small at the bottom: with Cardputer, StickC and Basic
  // in table, says who speaks the format of component esp-now and who sends pure text.
  out->meta[0] = 0;
  if (!m.minha) {
    snprintf(out->meta, sizeof(out->meta), "%s", m.espressif ? "esp-now Espressif" : "texto puro");
    out->color_meta = m.espressif ? TFT_SKYBLUE : TFT_DARKGREY;
  }
  return true;
}

static bool send(const char* text, void*) { return tab5_espnow_send(text); }

// ---- information bar --------------------------------------------------
// 0 = without our firmware on C6, 1 = C6 without connection (WiFi never enabled on this
// boot), 2 = turning on, 3 = on air, 4 = radio stopped (WiFi disabled in Settings,
// or dropping out of range of router) - the link is still offline, the radio is not.
static int radio_status(void)
{
  if (!tab5_espnow_c6_compativel()) {
    return tab5_wifi_c6_link() ? 0 : 1;   // no link still not able to know the firmware
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

static void draw_status_bar(lgfx::LovyanGFX& d, const area_t& a, int est)
{
  const uint8_t fixo = tab5_espnow_canal_fixo();
  char canal[32];
  // Fixed: the channel that the C6 SAID it is on (the service checks) - if it
  // refused the exchange, the pill shows the request and the real, instead of lying.
  const uint8_t real = tab5_espnow_canal();
  // In standing, the bar is 720 px: the pill is short. "fixed" x "router" continues
  // said by her COLOR (orange = fixed), just without the word.
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
    // DOESN'T FIT THE MAC ON STANDING. After title and pill, there are ~150 px left, and
    // the entire line shrunk to the minimum was still over the edge. MAC and
    // the data that changes the least (and the one of Tab5 itself): exit it, stays what it says
    // if there is anyone nearby.
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
  const bool m1 = paint_changed_i(&s_p, &s_status_state, est);
  const bool m2 = paint_changed_s(&s_p, s_status_text, sizeof(s_status_text), everything);
  if (!m1 && !m2) { return; }

  const uint16_t FUNDO = d.color565(13, 17, 23);
  tab5_screen_clear(a.x, a.y, a.width, STATUS_BAR_HEIGHT, FUNDO);
  const int cy = a.y + STATUS_BAR_HEIGHT / 2;
  const uint16_t color = (est == 3) ? TFT_GREENYELLOW : (est == 2) ? TFT_ORANGE : TFT_DARKGREY;
  d.fillCircle(a.x + 34, cy, 15, color);

  d.setTextDatum(middle_left);
  tab5_text(d, 3.6f);
  d.setTextColor(TFT_WHITE, FUNDO);
  d.drawString("ESP-NOW", a.x + 64, cy);
  int x = a.x + 64 + (int)d.textWidth("ESP-NOW") + 28;

  // Pill of canal: it's a button (touch exchange), so it looks like a button.
  tab5_text(d, 3.2f);
  const int pl = (int)d.textWidth(canal) + 44;
  const int pa = STATUS_BAR_HEIGHT - 22;
  const uint16_t color_p = fixo ? d.color565(120, 70, 10) : d.color565(38, 60, 90);
  d.fillRoundRect(x, cy - pa / 2, pl, pa, pa / 2, color_p);
  d.setTextColor(TFT_WHITE, color_p);
  d.drawString(canal, x + 22, cy);
  s_pilula_x0 = x - a.x;
  s_pilula_x1 = x - a.x + pl;
  x += pl + 24;

  // The rest (MAC, ears, last) in the BIGGEST size that fits in what's left -
  // measured, not kicked: in portrait or with long MAC, it shrinks by itself in
  // rather than passing over the edge.
  const int sobra = a.x + a.width - 16 - x;
  float size = 3.2f;
  tab5_text(d, size);
  while (size > 1.8f && (int)d.textWidth(txt) > sobra) {
    size -= 0.2f;
    tab5_text(d, size);
  }
  // In the smallest size and it still doesn't fit: cuts at the end, per character (UTF-8),
  // instead of moving past the edge of the screen.
  tab5_cortar_on_caber(d, txt, sobra);
  d.setTextColor(TFT_LIGHTGREY, FUNDO);
  d.drawString(txt, x, cy);
  d.drawFastHLine(a.x, a.y + STATUS_BAR_HEIGHT - 1, a.width, d.color565(38, 42, 50));
  d.setTextDatum(top_left);
}

// ---- icon of home screen ------------------------------------------------
// In the line of the Espressif brand (owner of ESP-NOW, which does not have a logo
// own - the official page uses the company brand): its red, and in
// white the point of origin with three waves opening to the top and to the right -
// the signal coming out of one device, without a router in the middle. Everything proportional
// side: the same drawing works in any grid size.
static void draw_espnow_icon(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t vermelho = d.color565(231, 53, 44);   // the red of Espressif
  d.fillRoundRect(x, y, lado, lado, lado / 6, vermelho);

  const int cx = x + (lado * 28) / 100;
  const int cy = y + (lado * 72) / 100;
  int grosso = (lado * 7) / 100;
  if (grosso < 3) { grosso = 3; }

  d.fillCircle(cx, cy, (lado * 8) / 100, TFT_WHITE);
  // 270..360 degrees = the quadrant of top-right (y grows to bottom).
  static const int RAIO[3] = { 24, 40, 56 };   // % of side, to the edge of outside of wave
  for (int k = 0; k < 3; ++k) {
    const int r1 = (lado * RAIO[k]) / 100;
    d.fillArc(cx, cy, r1 - grosso, r1, 270, 360, TFT_WHITE);
  }
}

// ---- life cycle -----------------------------------------------------------------------
static bool on_open(void)
{
  s_status_state = -1;
  s_status_text[0] = 0;
  conversation_open(&s_c, &s_p, count, read, send, nullptr);
  tab5_espnow_marcar_lidas();
  tab5_espnow_window(true);    // conversation in front: service asks receiver enabled
  return true;
}

static void on_close(void) { tab5_espnow_window(false); conversation_close(&s_c); }

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  paint_start(&s_p);
  const int est = radio_status();
  draw_status_bar(d, a, est);
  const area_t r = { a.x, a.y + STATUS_BAR_HEIGHT, a.width, a.height - STATUS_BAR_HEIGHT };
  conversation_draw(&s_c, d, r, paint_complete(&s_p), est == 3, reason(est));
}

static void on_notice(uint32_t)
{
  // With the window open, what arrives is already read.
  tab5_espnow_marcar_lidas();
  conversation_changed(&s_c);
}

static void on_touch(int x, int y)
{
  // The channel pill passes on next: router -> 1 -> 6 -> 11 -> router.
  if (y < STATUS_BAR_HEIGHT && s_pilula_x0 >= 0 && x >= s_pilula_x0 && x < s_pilula_x1) {
    const uint8_t current = tab5_espnow_canal_fixo();
    int k = 0;
    while (k < (int)sizeof(CHANNEL_OPTIONS) && CHANNEL_OPTIONS[k] != current) { ++k; }
    tab5_espnow_set_canal(CHANNEL_OPTIONS[(k + 1) % (int)sizeof(CHANNEL_OPTIONS)]);
    system_request_redraw();
    return;
  }
  if (y < STATUS_BAR_HEIGHT) { return; }
  conversation_touch(&s_c, x, y);
}
static void on_touch_level(int x, int y, bool p)       { conversation_touch_level(&s_c, x, y, p); }
static void on_type(const char* t, uint8_t c, uint8_t m) { conversation_type(&s_c, t, c, m); }

extern const app_t APP_ESPNOW = {
  .name  = "ESP-NOW",
  .color   = TFT_CYAN,
  // The radio and WiFi service (system adjustment maintains). The IMU and for
  // the screen follows the device.
  .profile = { .imu = true },
  .uses_sd = false,          // history and service
  // Standing or lying down depending on the device; with the keyboard docked, lying down
  // (same on Mesh). Conversation is the component of the system and is already measured by
  // width that is enough - only the top bar needs to know about portrait.
  .orientation = ORIENTATION_FOLLOWS_IMU,
  .sleep_clock_floor_mhz = 0,
  .on_open  = on_open,
  .on_close = on_close,
  .draw  = draw,
  .on_touch  = on_touch,
  .on_touch_level = on_touch_level,   // key lights up under the finger; scroll
  .on_type = on_type,             // physical keyboard of ExtPort1
  .redraw_period_ms = 1000,                 // the "last ha Ns" of barra
  .notices = AVISO_ESPNOW,
  .on_notice = on_notice,
  .icon = draw_espnow_icon,
};
