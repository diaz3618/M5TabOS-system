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

// Lazy-create, nunca destruida - mesmo molde de services/notification.cpp e
// tab5_gps.cpp: o primeiro uso sempre vem antes de qualquer disputa de
// verdade (o boot chama tab5_imu_measure() antes de qualquer app poder read).
static SemaphoreHandle_t s_trava = nullptr;

static inline void travar(void)
{
  tab5_mutex_travar(&s_trava);   // criado sem corrida na primeira vez
}
static inline void destravar(void) { if (s_trava) { xSemaphoreGive(s_trava); } }

// A IMU QUE NAO SUBIU NO BOOT. O M5.begin() inicia o BMI270 uma vez so, e
// esse inicio carrega um firmware de 8 KB no chip pelo I2C; de vez em quando
// ele failure (o chip ainda saindo do reset, uma transacao perdida) e a IMU
// ficava fora a sessao inteira - o app mostrava "IMU nao disponivel neste
// boot" ate reiniciar. Aqui o sistema tenta de novo, algumas vezes,
// espacado: cada tentativa ocupa o barramento por uns 200 ms.
#define IMU_TENTATIVAS   5
#define IMU_ESPERA_MS 3000

static bool subir_de_novo(void)
{
  static int      s_tentativas = 0;
  static uint32_t s_proxima_ms = 0;
  if (!tab5_power_imu_requested() || s_tentativas >= IMU_TENTATIVAS) { return false; }
  const uint32_t agora = tab5_now_ms();
  if ((int32_t)(agora - s_proxima_ms) < 0) { return false; }
  s_proxima_ms = agora + IMU_ESPERA_MS;
  ++s_tentativas;
  const bool ok = M5.Imu.begin(&M5.In_I2C, M5.getBoard());
  if (ok) { ESP_LOGI(TAG, "IMU subiu na tentativa %d depois do boot", s_tentativas); }
  else    { ESP_LOGW(TAG, "IMU ainda fora (tentativa %d de %d)", s_tentativas, IMU_TENTATIVAS); }
  return ok;
}

void tab5_imu_measure(void)
{
  if (!M5.Imu.isEnabled() && !subir_de_novo()) { return; }

  M5.Imu.update();
  tab5_imu_data_t d = { };
  d.valid = true;
  M5.Imu.getAccel(&d.ax, &d.ay, &d.az);
  M5.Imu.getGyro(&d.gx, &d.gy, &d.gz);

  travar();
  s_cache = d;
  destravar();
}

bool tab5_imu_latest(tab5_imu_data_t* output)
{
  if (!output) { return false; }
  travar();
  *output = s_cache;
  destravar();
  return output->valid;
}
