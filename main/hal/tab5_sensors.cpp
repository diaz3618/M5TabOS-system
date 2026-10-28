#include "tab5_sensors.hpp"

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char* TAG = "sensors";

static temperature_sensor_handle_t s_tsens = nullptr;
static float s_celsius = 0.0f;
static bool  s_valid   = false;
// AVERAGE SINCE BOOT, using raw samples (the filter above is for the eye, not
// the calculation). Use double: at one sample per second, float would lose
// sub-degree resolution after only a few days.
static double   s_sum = 0.0;
static uint32_t s_count = 0;

void tab5_sensors_init(void)
{
  // Range 10..80 C. The ESP32 sensor changes its internal scale to match the
  // requested range, and an overly broad range loses resolution. This covers
  // everything the device sees in practice, from a cold room to a busy chip.
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);

  esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "temperature_sensor_install failed: %s", esp_err_to_name(err));
    s_tsens = nullptr;
    return;
  }
  err = temperature_sensor_enable(s_tsens);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "temperature_sensor_enable failed: %s", esp_err_to_name(err));
    temperature_sensor_uninstall(s_tsens);
    s_tsens = nullptr;
    return;
  }
  ESP_LOGI(TAG, "P4 temperature sensor is online");
}

void tab5_sensors_measure(void)
{
  if (!s_tsens) { return; }
  float c = 0.0f;
  if (temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) {
    // GENTLE FILTER. Raw samples jump by half a degree, and a status-bar number
    // that constantly twitches makes the eye chase it. Simple moving average:
    // 7/8 previous and 1/8 new, following real changes within a few seconds
    // while ignoring noise.
    s_celsius = s_valid ? (s_celsius * 0.875f + c * 0.125f) : c;
    s_valid = true;
    s_sum += c;
    ++s_count;
  }
}

bool tab5_sensors_temp(float* celsius)
{
  if (!s_valid) { return false; }
  if (celsius) { *celsius = s_celsius; }
  return true;
}

bool tab5_sensors_average_temp(float* celsius)
{
  if (!s_count) { return false; }
  if (celsius) { *celsius = (float)(s_sum / s_count); }
  return true;
}
