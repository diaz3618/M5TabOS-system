// SCREEN ORIENTATION - see orientation.hpp.
#include "orientation.hpp"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "virtual_screen.hpp"
#include "tab5_imu.hpp"
#include "tab5_keyboard.hpp"
#include "tab5_time.hpp"   // tab5_now_ms - the same clock of laco of interface
#include "esp_log.h"

static const char* TAG = "orientacao";

// The two rotations of M5GFX that exist in the system. 3 and not 1: it is the one that leaves
// o ExtPort1 - where the keyboard fits - at the edge of the bottom, next to who
// types. With 1 the screen was upside down in relation to the keyboard.
static const uint8_t ROT_UPRIGHT = 0;
static const uint8_t ROT_DEITADO = 3;

// -------------------------------------------------------------------------------
// THE DECISION
// -------------------------------------------------------------------------------
// Only the SCREEN PLANE matters (ax, ay). The y-axis of IMU runs along the side
// long of panel (the same reference as level of app IMU), then:
//
// gravity almost all in y -> in pe
// almost all gravity in x -> lying down
//
// THE SIGNAL DOESN'T COME IN, on purpose. The two states are one per axis, and the side
// opposite of each one (of head to bottom, or lying down with the keyboard on top)
// falls into the same state - the screen doesn't turn to the other side, it just stays. That and the
// that you want, and you still don't need to know the polarity of each axis of BMI270
// on this device: getting one sign wrong here would invert the entire rule in
// silence.
//
// TWO DEAD ZONES, where nothing changes with the OPEN app:
// - device in table: gravity almost entirely in z, the plane of screen with
// less than ~0.4 g (tilted less than ~24 degrees). Do not put the device down
// can rotate the screen. It was 0.5 g (30 degrees): holding the device lying down
// in hand, standing up, the reading was below that and one app opened lying down
// never corrected himself.
// - in diagonal: one axis must pass the other through PLANO_RAZAO (tangent
// of 60 degrees). Between 30 and 60 degrees of rotation, it stays like this - the hysteresis
// which prevents the screen from jumping from one state to another in border.
static const float SCREEN_PLANE_MIN_G2 = 0.16f;   // (0.4 g)^2
static const float SCREEN_PLANE_AXIS_RATIO = 1.73f;

// IN THE OPENING THERE IS NO PREVIOUS STATE TO PROTECT: hysteresis exists not to
// jumping from one side to the other, and opening there is no "other side" yet. It's worth
// axis that dominates, with small clearance (tangent of ~50 degrees), and one plane
// smaller (0.3 g, ~17 degrees of tilt). Outside of that - in mesa, or
// exactly diagonal - opens in the position the screen is already in (the home and
// always standing), never on a fixed guess.
static const float OPENING_PLANE_MIN_G2 = 0.09f;   // (0.3 g)^2
static const float OPENING_AXIS_RATIO = 1.2f;

// TIME. One sample every 200 ms (5 Hz) and the pose must be repeated in 3
// in a row: ~0.6 s stopped in new posture before rotating. A swing of hand
// does not rotate; Turn the device on purpose and it turns right away. After rotating, 1.5 s
// of deficiency: pass complete takes one frame, and switch of new in the middle
// of gesture would be worse than wait.
static const uint32_t ORIENTATION_SAMPLE_MS = 200;
static const int      SEGUIDAS     = 3;
static const uint32_t CARENCIA_MS  = 1500;

static orientation_t s_modo      = ORIENTATION_PORTRAIT;   // o of app open
static uint32_t     s_ultima_ms = 0;
static uint32_t     s_girou_ms  = 0;
static int          s_cand      = -1;          // the rotation that is asking for passage
static int          s_seguidas  = 0;

// The rotation that the posture of AGORA requires, or -1 (dead zone, IMU no reading).
static int rotation_requested(tab5_imu_data_t* m)
{
  // Docked physical keyboard: always lying down. Standing he would hang
  // on the side, and rotating on your lap while typing would be the worst defect.
  if (tab5_keyboard_present()) { return ROT_DEITADO; }
  if (!tab5_imu_latest(m)) { return -1; }
  const float ax = fabsf(m->ax), ay = fabsf(m->ay);
  if (ax * ax + ay * ay < SCREEN_PLANE_MIN_G2) { return -1; }
  if (ay > ax * SCREEN_PLANE_AXIS_RATIO) { return ROT_UPRIGHT; }
  if (ax > ay * SCREEN_PLANE_AXIS_RATIO) { return ROT_DEITADO; }
  return -1;
}

// A READ YOU CAN TRUST, for the opening. The cache of IMU and
// updated 1x/s and may be old; and the first reading after the chip
// being quiet may be reset (M5Unified only updates the values when there is
// new data, and in the economy mode of BMI270 the new data takes time). A reading
// when reset fell into the dead zone, and the screen opened when the device was lying down.
// Here: up to 6 readings, 10 ms between them, until the gravity module does
// sense (0.6 to 1.4 g - stationary, just gravity). ~60 ms worst case once
// by opening of app.
static bool read_postura_fresca(tab5_imu_data_t* m)
{
  for (int k = 0; k < 6; ++k) {
    tab5_imu_measure();
    if (tab5_imu_latest(m)) {
      const float g2 = m->ax * m->ax + m->ay * m->ay + m->az * m->az;
      if (g2 > 0.36f && g2 < 1.96f) { return true; }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

// The rotation to OPEN (see ABRIR_RAZAO), or -1 to stay as the screen is.
static int rotation_para_open(tab5_imu_data_t* m)
{
  if (tab5_keyboard_present()) { return ROT_DEITADO; }
  if (!read_postura_fresca(m)) { return -1; }
  const float ax = fabsf(m->ax), ay = fabsf(m->ay);
  if (ax * ax + ay * ay < OPENING_PLANE_MIN_G2) { return -1; }
  if (ay > ax * OPENING_AXIS_RATIO) { return ROT_UPRIGHT; }
  if (ax > ay * OPENING_AXIS_RATIO) { return ROT_DEITADO; }
  return -1;
}

static void aplicar(uint8_t rot)
{
  if (M5.Display.getRotation() == rot) { return; }
  M5.Display.setRotation(rot);
  virtual_screen_follow_rotation();
}

void orientation_on_open(const app_t* app)
{
  s_modo     = app->orientation;
  s_cand     = -1;
  s_seguidas = 0;
  s_ultima_ms = s_girou_ms = tab5_now_ms();

  uint8_t rot = ROT_UPRIGHT;
  tab5_imu_data_t m = { };
  int p = -1;
  if (s_modo == ORIENTATION_LANDSCAPE) {
    rot = ROT_DEITADO;
  } else if (s_modo == ORIENTATION_FOLLOWS_IMU) {
    // The posture of now, measurement now (see ler_postura_fresca). No decision
    // clearly, it opens as the screen already is - whoever opens one app of home is looking
    // for her in pe. Before, the default was flat, and that was what always appeared
    // that the reading failed.
    p = rotation_para_open(&m);
    const uint8_t now = M5.Display.getRotation();
    rot = (p >= 0) ? (uint8_t)p : (now == ROT_DEITADO ? ROT_DEITADO : ROT_UPRIGHT);
  }
  aplicar(rot);
  if (s_modo == ORIENTATION_FOLLOWS_IMU) {
    // Gravity together, as in the spin: confirm in the log why it opened like that.
    ESP_LOGI(TAG, "'%s' abre %s, seguindo o aparelho (%dx%d)  ax=%+.2f ay=%+.2f az=%+.2f%s", app->name,
             rot == ROT_UPRIGHT ? "em pe" : "deitado", M5.Display.width(), M5.Display.height(),
             (double)m.ax, (double)m.ay, (double)m.az,
             tab5_keyboard_present() ? "  (teclado encaixado)" : p < 0 ? "  (sem decisao: a posicao da tela)" : "");
  } else {
    ESP_LOGI(TAG, "'%s' abre %s (%dx%d)", app->name, rot == ROT_UPRIGHT ? "em pe" : "deitado",
             M5.Display.width(), M5.Display.height());
  }
}

void orientation_on_close(void)
{
  s_modo = ORIENTATION_PORTRAIT;
  if (M5.Display.getRotation() != ROT_UPRIGHT) {
    aplicar(ROT_UPRIGHT);
    ESP_LOGI(TAG, "tela de volta em pe (%dx%d)", M5.Display.width(), M5.Display.height());
  }
}

uint32_t orientation_time_until_sample_ms(uint32_t now)
{
  if (s_modo != ORIENTATION_FOLLOWS_IMU) { return UINT32_MAX; }
  const uint32_t dec = now - s_ultima_ms;
  return (dec < ORIENTATION_SAMPLE_MS) ? (ORIENTATION_SAMPLE_MS - dec) : 0;
}

bool orientation_vigiar(uint32_t now, bool pode_rotate)
{
  if (s_modo != ORIENTATION_FOLLOWS_IMU) { return false; }
  if ((now - s_ultima_ms) < ORIENTATION_SAMPLE_MS) { return false; }
  s_ultima_ms = now;

  tab5_imu_measure();
  tab5_imu_data_t m = { };
  const int p = rotation_requested(&m);
  if (p < 0 || p == (int)M5.Display.getRotation()) {
    s_cand = -1;
    s_seguidas = 0;
    return false;
  }
  if (p == s_cand) { ++s_seguidas; }
  else             { s_cand = p; s_seguidas = 1; }

  if (s_seguidas < SEGUIDAS || !pode_rotate || (now - s_girou_ms) < CARENCIA_MS) { return false; }

  aplicar((uint8_t)p);
  s_girou_ms = now;
  s_cand = -1;
  s_seguidas = 0;
  // Gravity together: and what confirms in the log, without guessing, that the axis and the
  // threshold is correct in this device.
  ESP_LOGI(TAG, "girou para %s (%dx%d)  ax=%+.2f ay=%+.2f az=%+.2f%s",
           p == ROT_UPRIGHT ? "em pe" : "deitado", M5.Display.width(), M5.Display.height(),
           (double)m.ax, (double)m.ay, (double)m.az, tab5_keyboard_present() ? "  (teclado encaixado)" : "");
  return true;
}
