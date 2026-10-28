#include "tab5_sensors.hpp"

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char* TAG = "sensores";

static temperature_sensor_handle_t s_tsens = nullptr;
static float s_celsius = 0.0f;
static bool  s_valida  = false;
// MEDIA DESDE O BOOT, da leitura CRUA (o filtro acima e para o olho, nao para
// a conta). Double: a 1 amostra por segundo, um float perderia a resolucao do
// meio grau em poucos dias ligado.
static double   s_soma = 0.0;
static uint32_t s_n    = 0;

void tab5_sensores_iniciar(void)
{
  // Faixa 10..80 C. O sensor do ESP32 troca de escala interna conforme a faixa
  // pedida, e uma faixa larga demais perde resolucao. Esta cobre desde uma sala
  // fria ate o chip trabalhando duro, que e todo o intervalo que este aparelho
  // ve na pratica.
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);

  esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "temperature_sensor_install falhou: %s", esp_err_to_name(err));
    s_tsens = nullptr;
    return;
  }
  err = temperature_sensor_enable(s_tsens);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "temperature_sensor_enable falhou: %s", esp_err_to_name(err));
    temperature_sensor_uninstall(s_tsens);
    s_tsens = nullptr;
    return;
  }
  ESP_LOGI(TAG, "sensor de temperatura do P4 no ar");
}

void tab5_sensores_medir(void)
{
  if (!s_tsens) { return; }
  float c = 0.0f;
  if (temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) {
    // FILTRO SUAVE. A leitura crua pula meio grau entre amostras, e um numero
    // na barra de status que treme o tempo todo obriga o olho a persegui-lo.
    // Media movel simples: 7/8 do anterior e 1/8 do novo - acompanha uma
    // mudanca real em poucos segundos e ignora o chiado.
    s_celsius = s_valida ? (s_celsius * 0.875f + c * 0.125f) : c;
    s_valida  = true;
    s_soma += c;
    ++s_n;
  }
}

bool tab5_sensores_temp(float* celsius)
{
  if (!s_valida) { return false; }
  if (celsius) { *celsius = s_celsius; }
  return true;
}

bool tab5_sensores_temp_media(float* celsius)
{
  if (!s_n) { return false; }
  if (celsius) { *celsius = (float)(s_soma / s_n); }
  return true;
}
