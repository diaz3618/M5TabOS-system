// APP: IMU - live viewer of BMI270 axes (accelerometer + gyroscope).
//
// WHAT IT IS FOR: no app for this system used the IMU yet. This is the
// playground - shows the 6 axes (3 of accelerometer, 3 of gyroscope) on
// alive, to feel what the chip measures in hand before deciding what to do
// with that later (the idea postponed and rotate the screen alone, like a cell phone).
//
// NO OWN TASK, ON PURPOSE: unlike Mesh/GPS, no one
// needs IMU with the app CLOSED - it only matters while someone is
// looking at the screen and moving the device. draw() reads the direct sensor, in
// OWN task of interface, which is the only one authorized to talk to the
// internal I2C bus (same of touch and of INA226 - see warning in
// tab5_power.hpp). A task and a queue here would be an abstraction without reason,
// in the same spirit as app Example: there is no background producer
// to feed.
//
// profile.imu = true AND A PHASE 1 FLAG (see tab5_power.hpp): since
// This app exists in the table, the BMI270 and enabled at boot for the SESSION
// ENTIRE - with active gyroscope and advanced power save disabled, the worst
// case of consumption of chip (it is M5Unified who decides so; it is not for
// change after M5.begin()). tab5_power_imu_accelerometer_only() already exists
// ready to cut this when nothing is using the gyroscope, but
// It's not called by anyone yet - stays out of this app of purpose:
// is a decision of policy of energy of SISTEMA (when enable/disable
// for ALL apps), not from this playground.

#include "app.h"
#include "paint.hpp"
#include <math.h>
#include <stdio.h>
#include "esp_log.h"
#include "tab5_ui.hpp"

static const char* TAG = "app.imu";

// OWN CONTRAST, NOT SIS_BORDA/SIS_LINE: frame and crosshead were
// TOO WEAK against black, SIS_LINHA and rgb(56,60,60), almost invisible).
// SIS_BORDA/SIS_LINHA are constants of SISTEMA (app.h), used by several
// apps for discrete edges of purpose - changing both would change the whole
// world. This app is an instrument display (level + axis bars),
// where the grid NEEDS to be seen by top of black background - hence IMU_BORDA
// only from here, clearer than the of system (the line of middle uses d.color565()
// directly at the points of drawing, rgb(72,76,80), only one a little lighter than the
// original SIS_LINE).
static const uint16_t IMU_BORDA = TFT_DARKGREY;   // 0x7BEF, rgb(128,130,128)

static bool on_open(void)
{
  if (!M5.Imu.isEnabled()) {
    // It does not refuse opening: draw() shows the warning on screen anyway.
    // This would only happen if profile.imu had been set to false by mistake -
    // see Sistema_profil_de_boot(), which joins the field of ALL apps.
    ESP_LOGW(TAG, "M5.Imu nao esta habilitada - perfil.imu chegou a false no boot?");
  }
  ESP_LOGI(TAG, "aberto");
  return true;
}

// BOTTOM OF THE LEVEL, CURLY - see the large notice below, next to
// draw_level(). Created on first call, released on close the app.
static M5Canvas* s_level_fundo   = nullptr;
static int       s_level_fundo_r = -1;   // radius with which the background was drawn

// AXIS BARS BACKGROUND, CURLY - see the large warning next to
// draw_bar(). Declared here (not next to the function, below) by
// same reason of level: ao_close() needs to release both.
static M5Canvas* s_barra_fundo   = nullptr;
static int       s_barra_fundo_w = -1, s_barra_fundo_h = -1;

static void on_close(void)
{
  tab5_sprite_release(&s_level_fundo);
  s_level_fundo_r = -1;
  tab5_sprite_release(&s_barra_fundo);
  s_barra_fundo_w = s_barra_fundo_h = -1;
  ESP_LOGI(TAG, "fechado");
}

// -------------------------------------------------------------------------------
// LEVEL (BUBBLE)
// -------------------------------------------------------------------------------
// The point moves with the inclination read from accelerometer in sleep: 1g
// divided between the axes as the device tips over. Clamped at +-1g - German
// Therefore, the device is already on the side, and the stitch only needs to touch the edge.
//
// this redraws the ENTIRE box every frame (20Hz):
// erased everything and recomputed two circles + crosshead via algorithm of
// vector, just to move one point. It was a CONSCIOUS exchange (documented here
// same: "simpler than saving the previous position", "tens of KB per
// frame, nothing like the ~1.6MB that causes the blue blink") - no
// was wrong, it just hadn't been revisited yet in the process of
// optimization of energy for this session.
//
// The exchange now: the CIRCLE/CROSS (which never change, only their radius changes if
// the screen is resized) go to a small M5Canvas, designed only
// when the radius changes. At each frame, instead of recalculating the two circles and
// the two lines by algorithm, just do one pushSprite() of canvas (a copy
// of bytes, without trigonometry or Bresenham) and draws the ball by top.
// THE NUMBER OF BYTES WRITTEN ON THE SCREEN REMAINS THE SAME (the entire box is still
// and "revealed" with each frame, and this is how the trace of old ball
// remains correct without having to restore just the part where it was) - the
// gain and OS of CPU: a copy is always cheaper than recalculating the circle and
// line of zero, 20 times per second, while the app is open.
static void draw_level(lgfx::LovyanGFX& d, int cx, int cy, int r, float ax, float ay)
{
  const int lado = 2 * r + 12;

  if (!s_level_fundo) {
    // NO SPRITE, NO CACHE - iteration on old path (redraws everything by
    // vector). It would only happen without free PSRAM, which would already be a problem
    // bigger than this.
    s_level_fundo = tab5_sprite_novo(&d, lado, lado);
  }

  if (!s_level_fundo) {
    const uint16_t line = d.color565(72, 76, 80);   // see the warning of IMU_BORDA at the top of file
    d.fillRect(cx - r - 6, cy - r - 6, lado, lado, TFT_BLACK);
    d.drawCircle(cx, cy, r, IMU_BORDA);
    d.drawCircle(cx, cy, r / 2, line);
    d.drawFastHLine(cx - r, cy, 2 * r, line);
    d.drawFastVLine(cx, cy - r, 2 * r, line);
  } else {
    if (s_level_fundo_r != r) {
      // ONLY WHEN THE RADIUS CHANGES (never, in practice - the screen does not resize
      // with the app open - but it's the same care for the rest of the system: no
      // assume one fixed layout by luck).
      const uint16_t line = d.color565(72, 76, 80);
      s_level_fundo->fillSprite(TFT_BLACK);
      s_level_fundo->drawCircle(lado / 2, lado / 2, r, IMU_BORDA);
      s_level_fundo->drawCircle(lado / 2, lado / 2, r / 2, line);
      s_level_fundo->drawFastHLine(lado / 2 - r, lado / 2, 2 * r, line);
      s_level_fundo->drawFastVLine(lado / 2, lado / 2 - r, 2 * r, line);
      s_level_fundo_r = r;
    }
    s_level_fundo->pushSprite(&d, cx - lado / 2, cy - lado / 2);
  }

  float px = ax, py = ay;
  if (px >  1.0f) { px =  1.0f; }
  if (px < -1.0f) { px = -1.0f; }
  if (py >  1.0f) { py =  1.0f; }
  if (py < -1.0f) { py = -1.0f; }

  // Screen grows DOWN; tilt the top of the device "back" and Y
  // positive on the accelerometer, and should move the point UP in screen -
  // so the sign is changed in py.
  const int dx = cx + (int)(px * (r - 16));
  const int dy = cy - (int)(py * (r - 16));
  d.fillCircle(dx, dy, 11, TFT_GREENYELLOW);
  d.drawCircle(dx, dy, 11, TFT_BLACK);
}

// -------------------------------------------------------------------------------
// ZERO CENTERED BAR
// -------------------------------------------------------------------------------
// 'frac' already comes in [-1, 1] (the value divided by the scale chosen by whoever
// calls). Fill in the center to the side indicated by the sign - the same
// drawing of one meter of balance of audio, which is the right metaphor here:
// What matters is the REMOVAL of zero, in both directions.
//
// FUNDO CACHEADO, (s_barra_fundo, declared there in top next to
// s_nivel_fundo) - found in the same scan that led to the fix of level
// (desenhar_nivel(), see the big warning there): this function runs 6 times per
// frame at 20Hz (one per axis) and erased + redrawn border and line of
// center EVERY TIME, with all SIX calls using exactly the same w/h - the
// same still image, six times per frame, always recalculated from zero
// by algorithm of rectangle/line. A single sprite, of size of ONE bar,
// serves all six (all identical) - just the filling (which in fact changes by
// axis) continues drawn directly on screen.
static void draw_axis_bar(lgfx::LovyanGFX& d, int x, int y, int w, int h,
                           float frac, uint16_t color)
{
  const int cx = x + w / 2;

  if (s_barra_fundo && (s_barra_fundo_w != w || s_barra_fundo_h != h)) {
    tab5_sprite_release(&s_barra_fundo);
  }
  if (!s_barra_fundo) {
    // NO PSRAM FOR SPRITE (nullptr) - iteration on old path, same
    // defense of level.
    s_barra_fundo = tab5_sprite_novo(&d, w, h);
    if (s_barra_fundo) {
      s_barra_fundo->fillSprite(TFT_BLACK);
      s_barra_fundo->drawRect(0, 0, w, h, IMU_BORDA);
      s_barra_fundo->drawFastVLine(w / 2, 0, h, d.color565(72, 76, 80));
      s_barra_fundo_w = w;
      s_barra_fundo_h = h;
    }
  }

  if (s_barra_fundo) {
    s_barra_fundo->pushSprite(&d, x, y);
  } else {
    d.fillRect(x, y, w, h, TFT_BLACK);
    d.drawRect(x, y, w, h, IMU_BORDA);
    d.drawFastVLine(cx, y, h, d.color565(72, 76, 80));
  }

  if (frac >  1.0f) { frac =  1.0f; }
  if (frac < -1.0f) { frac = -1.0f; }
  const int px = (int)(frac * (w / 2 - 3));
  if (px >= 0) { d.fillRect(cx, y + 3, px, h - 6, color); }
  else         { d.fillRect(cx + px, y + 3, -px, h - 6, color); }
}

typedef struct {
  const char* rotulo;
  float       valor;
  float       escala;    // value that fills the entire bar (1.0 == full scale)
  uint16_t    color;
  const char* unidade;
} eixo_t;

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  static paint_t s_p;
  paint_start(&s_p);

  // The system tries to raise a new IMU that failed to boot (tab5_imu_measure):
  // when it appears, the warning exits on a complete repaint.
  static bool s_imu_unavailable = false;
  if (!M5.Imu.isEnabled()) {
    if (paint_complete(&s_p) || !s_imu_unavailable) {
      s_imu_unavailable = true;
      tab5_text(d, 2.3f);
      d.setTextDatum(middle_center);
      d.setTextColor(TFT_RED, TFT_BLACK);
      d.drawString("IMU did not respond", a.x + a.width / 2, a.y + a.height / 2);
    }
    return;
  }
  if (s_imu_unavailable) {
    s_imu_unavailable = false;
    system_request_redraw_complete();
    return;
  }

  // ONE read per frame, in task of interface - same rule as INA226 and
  // of keyboard (see the big warning in tab5_power.hpp about the bus
  // internal without mutex). M5.Imu.update() and who talks to the BMI270; getAccel/
  // getGyro only reads the cache it has just filled.
  //
  // WHY NOT tab5_imu_latest() (see hal/tab5_imu.hpp): that
  // cache exists for OTHER apps to ask "how the device is doing" without
  // touch on I2C - but it is only powered 1x/second, too fast to
  // energy/temperature, too slowly for one level bubble not to appear
  // locked. This app KEEP reading straight from purpose - and the only place
  // that needs quick reading, and is already safe (same task).
  M5.Imu.update();
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  M5.Imu.getAccel(&ax, &ay, &az);
  M5.Imu.getGyro(&gx, &gy, &gz);

  const int level_r  = 130;
  const int level_cx = a.x + a.width / 2;
  const int level_cy = a.y + 24 + level_r;
  draw_level(d, level_cx, level_cy, level_r, ax, ay);

  if (paint_complete(&s_p)) {
    tab5_text(d, 2.3f);
    d.setTextDatum(top_center);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString("incline o aparelho", level_cx, level_cy + level_r + 6);
  }

  // Scale of accelerometer in g (1g = sleep in vertical) and of gyroscope in
  // degrees/second. The numbers are ceiling of SCREEN, not ceiling of sensor: pass
  // of them only touch the edge bar, the numerical value remains exact.
  const eixo_t eixos[6] = {
    { "AX", ax,   2.0f, TFT_RED,     "g"   },
    { "AY", ay,   2.0f, TFT_GREEN,   "g"   },
    { "AZ", az,   2.0f, TFT_SKYBLUE, "g"   },
    { "GX", gx, 250.0f, TFT_ORANGE,  "dps" },
    { "GY", gy, 250.0f, TFT_YELLOW,  "dps" },
    { "GZ", gz, 250.0f, TFT_MAGENTA, "dps" },
  };

  const int y0        = level_cy + level_r + 46;
  const int alt_line  = 72;
  const int barra_x    = a.x + 100;
  const int barra_w    = a.width - 100 - 150;
  const int barra_h    = 36;

  for (int i = 0; i < 6; ++i) {
    const int cy = y0 + i * alt_line;

    // Label: never changes, only enters in pass complete.
    if (paint_complete(&s_p)) {
      tab5_text(d, 2.3f);
      d.setTextDatum(middle_left);
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      d.drawString(eixos[i].rotulo, a.x + 16, cy + barra_h / 2);
    }

    draw_axis_bar(d, barra_x, cy, barra_w, barra_h,
                   eixos[i].valor / eixos[i].escala, eixos[i].color);

    char buf[24];
    snprintf(buf, sizeof(buf), "%+6.2f %s", (double)eixos[i].valor, eixos[i].unidade);
    tab5_text(d, 2.3f);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(buf, barra_x + barra_w + 12, cy + barra_h / 2);
  }
}

// -------------------------------------------------------------------------------
// HOME SCREEN ICON
// -------------------------------------------------------------------------------
// Three axes, common 3D tool convention: red X to the right, Y
// green for top, Z blue as a dotted circle (the axis that "exit from
// screen" there is no way to turn it into a flat arrow).
static void icon_imu(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(32, 24, 40);
  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  const int cx = x + lado / 2;
  const int cy = y + lado / 2;
  const int r  = (int)(0.34f * lado);

  for (int ang = 0; ang < 360; ang += 30) {
    const float rad = (float)ang * 3.14159265f / 180.0f;
    const int px = cx + (int)(r * 0.55f * cosf(rad));
    const int py = cy + (int)(r * 0.55f * sinf(rad));
    d.fillCircle(px, py, lado / 60, TFT_SKYBLUE);
  }

  tab5_line_grossa(d, cx, cy, cx + r, cy, lado / 30.0f, TFT_RED);
  d.fillTriangle(cx + r, cy - lado / 22, cx + r, cy + lado / 22, cx + r + lado / 14, cy, TFT_RED);

  tab5_line_grossa(d, cx, cy, cx, cy - r, lado / 30.0f, TFT_GREEN);
  d.fillTriangle(cx - lado / 22, cy - r, cx + lado / 22, cy - r, cx, cy - r - lado / 14, TFT_GREEN);

  d.fillCircle(cx, cy, lado / 26, TFT_WHITE);
}

extern const app_t APP_IMU = {
  .name  = "IMU",
  .color   = TFT_PURPLE,
  // Just the IMU. AND PHASE 1 (see the large warning at the top of file): once
  // registered, the BMI270 stays enabled the entire boot, not just while it is
  // app is in the foreground.
  .profile = { .imu = true },
  .uses_sd = false,
  .orientation = ORIENTATION_PORTRAIT,
  .sleep_clock_floor_mhz = 0,
  .on_open  = on_open,
  .on_close = on_close,
  .draw  = draw,
  .on_touch  = nullptr,
  .on_touch_level = nullptr,
  .on_type = nullptr,
  // 50ms = 20Hz. Fluid enough to follow a hand gesture without wasting
  // more I2C than you need - the same reasoning as TECLADO_MS in core.cpp.
  .redraw_period_ms = 50,
  .height_content = 0,
  .icon = icon_imu,
};
