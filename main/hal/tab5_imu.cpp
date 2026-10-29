#include "tab5_imu.hpp"
#include "tab5_time.hpp"   // tab5_now_ms
#include "tab5_mutex.hpp"
#include <M5Unified.h>
#include "tab5_power.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "imu";

static tab5_imu_data_t s_cache = { };

// Lazy-create, never destroyed - same template of services/notification.cpp and
// tab5_gps.cpp: the first use always comes before any dispute
// true (boot calls tab5_imu_measure() before any app can read).
static SemaphoreHandle_t s_trava = nullptr;

static inline void lock(void)
{
  tab5_mutex_lock(&s_trava);   // created without race in first time
}
static inline void destravar(void) { if (s_trava) { xSemaphoreGive(s_trava); } }

// THE IMU THAT DIDN'T BOOT. M5.begin() starts the BMI270 once, and
// this start loads one firmware of 8 KB onto the chip via I2C; from time to time
// it fails (the chip still coming out of reset, a lost transaction) and the IMU
// was off the entire session - the app showed "IMU not available in this
// boot" until it restarts. Here the system tries again, sometimes,
// spaced: each attempt occupies the bus for about 200 ms.
#define IMU_TENTATIVAS   5
#define IMU_RETRY_INTERVAL_MS 3000

static bool retry_imu_start(void)
{
  static int      s_tentativas = 0;
  static uint32_t s_next_ms = 0;
  if (!tab5_power_imu_requested() || s_tentativas >= IMU_TENTATIVAS) { return false; }
  const uint32_t now = tab5_now_ms();
  if ((int32_t)(now - s_next_ms) < 0) { return false; }
  s_next_ms = now + IMU_RETRY_INTERVAL_MS;
  ++s_tentativas;
  const bool ok = M5.Imu.begin(&M5.In_I2C, M5.getBoard());
  if (ok) { ESP_LOGI(TAG, "IMU subiu na tentativa %d depois do boot", s_tentativas); }
  else    { ESP_LOGW(TAG, "IMU ainda fora (tentativa %d de %d)", s_tentativas, IMU_TENTATIVAS); }
  return ok;
}

void tab5_imu_measure(void)
{
  if (!M5.Imu.isEnabled() && !retry_imu_start()) { return; }

  M5.Imu.update();
  tab5_imu_data_t d = { };
  d.valid = true;
  M5.Imu.getAccel(&d.ax, &d.ay, &d.az);
  M5.Imu.getGyro(&d.gx, &d.gy, &d.gz);

  lock();
  s_cache = d;
  destravar();
}

bool tab5_imu_latest(tab5_imu_data_t* output)
{
  if (!output) { return false; }
  lock();
  *output = s_cache;
  destravar();
  return output->valid;
}
