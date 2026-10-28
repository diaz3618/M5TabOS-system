#include "tab5_power.hpp"
#include "tab5_time.hpp"   // tab5_now_ms
#include "tab5_marks.hpp"
#include "tab5_sensors.hpp"
#include "tab5_screen.hpp"
#include "tab5_bus.hpp"

#include "esp_log.h"
#include <math.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"   // esp_clk_cpu_freq()
#include <stdio.h>

// Necessario para chegar no handle of panel DPI (ver tab5_power_sleep).
#include "tab5_internal_dsi_panel.hpp"

static const char *TAG = "power";

// ---------------------------------------------------------------------------
// Mapa dos expansores of Tab5 (dois PI4IOE5V6408 no I2C interno).
// Levantado of proprio M5Unified: Power_Class.cpp, ramo board_M5Tab5.
//
// Expansor 0 (0x43)                     Expansor 1 (0x44)
//   7 HP_DET    (input)                 7 CHG_EN       (output, high)
//   6 CAM_RST   (output, high=active)       6 CHG_STAT     (input)
//   5 TP_RST    (output, high=touch on)    5 nCHG_QC_EN   (output)
//   4 LCD_RST   (output, high=panel on)   4 PWROFF_PULSE (output)
//   3 NC / Hat power no Tab5X             3 USB5V_EN     (input c/ pull)
//   2 EXT5V_EN  (input c/ pull)         2 NC
//   1 SPK_EN    (output)                   1 NC
//   0 RF_PTH    (output)                   0 WLAN_PWR_EN  (output, high)
//
// Detalhe que engana: EXT5V_EN e USB5V_EN NAO sao pins dirigidos. Ficam como
// input com pull interno e o enable of verdade e one resistor in placa; por
// isso o setExtOutput() of M5Unified inverte o PULL em vez of write level.
// E por isso os dois nascem ligados sem ninguem request.
// ---------------------------------------------------------------------------
static constexpr uint8_t IOE0_CAM_RST     = 6;
static constexpr uint8_t IOE1_WLAN_PWR_EN = 0;

// BMI270 no barramento interno. O M5Unified tenta 0x69 e 0x68.
static constexpr uint8_t BMI270_CHIP_ID   = 0x24;
static constexpr uint8_t BMI270_REG_ID    = 0x00;
static constexpr uint8_t BMI270_PWR_CONF  = 0x7C;
static constexpr uint8_t BMI270_PWR_CTRL  = 0x7D;

// ---------------------------------------------------------------------------

static bool s_imu_requested = false;
bool tab5_power_imu_requested(void) { return s_imu_requested; }

void tab5_power_config(m5::M5Unified::config_t& cfg, const tab5_profile_t& profile)
{
  cfg.internal_imu = profile.imu;
  s_imu_requested     = profile.imu;
  cfg.internal_mic = profile.microphone;
  cfg.internal_spk = profile.speaker;
  cfg.internal_rtc = profile.rtc;

  // output_power=true faz o M5.begin() chamar setExtOutput(true) com mascara
  // 0xFF, que cobre ext_PA (Grove) E ext_USB (USB-A) of uma vez. Ligamos aqui
  // so se ALGUM dos dois for request, e separamos in fase 2.
  cfg.output_power = (profile.grove_5v || profile.usb_a_5v);
}

// Quando o Tab5 mexeu pela latest vez no 5V of Grove / reaplicou o profile
// (esp_timer em ms; 0 = nunca). Serve on diario of reinicios of no Mesh.
static volatile uint32_t s_grove_touch_ms = 0, s_aplicar_ms = 0;
static int8_t s_grove_hw = -1;   // ultimo state request on trilho (-1 = nunca)
// So conta como "mexeu no trilho" when o state request MUDA: reapply o
// mesmo value aparece a parte, em tab5_power_apply_ms().
static void anotar_grove(bool turn_on)
{
  if (s_grove_hw == (int8_t)turn_on) { return; }
  s_grove_hw = (int8_t)turn_on;
  s_grove_touch_ms = tab5_now_ms() | 1u;
}
uint32_t tab5_power_grove_touch_ms(void) { return s_grove_touch_ms; }
uint32_t tab5_power_apply_ms(void)     { return s_aplicar_ms; }

void tab5_power_apply(const tab5_profile_t& profile)
{
  s_aplicar_ms = tab5_now_ms() | 1u;
  anotar_grove(profile.grove_5v);
  tab5_mark("profile de energia", profile.cpu_min_mhz);
  // --- trilhos of 5V, now one a one ---------------------------------------
  // Se output_power ligou os dois, aqui desligamos o que nao foi request.
  M5.Power.setExtOutput(profile.grove_5v, m5::ext_PA);
  M5.Power.setExtOutput(profile.usb_a_5v, m5::ext_USB);

  // --- camera -------------------------------------------------------------
  tab5_power_camera(profile.camera);

  // --- radio (ESP32-C6) ---------------------------------------------------
  // Nao chamamos tab5_power_radio(true) when ja esta on: o trilho ja
  // nasce high e one pulso desnecessario reiniciaria o C6.
  if (!profile.radio) { tab5_power_radio(false); }

  // --- backlight ----------------------------------------------------------
  M5.Display.setBrightness(profile.brightness);

  // --- gerenciamento of energia of CPU ------------------------------------
  if (profile.pm_dinamico)
  {
#if CONFIG_PM_ENABLE
    // O PISO NUNCA PODE PASSAR DO TETO - esp_pm_configure recusa com
    // ESP_ERR_INVALID_ARG se min > max, e SEM AVISO CLARO: e exatamente o bug
    // que 'energia sleep 20' revelou em ajustar_teto(), so que
    // aqui e o caminho que TODO app usa a cada abertura, nao one comando of
    // diagnostics. Hoje nenhum app declara cpu_max_mhz abaixo dos 40 of
    // cpu_min_mhz default, entao o caso nunca acontece of verdade - mas o
    // next app que quisesse one ceiling bottom cairia in mesma pegadinha sem
    // pista none of porque. Mesma correcao: o piso acompanha o ceiling when
    // o ceiling request for menor.
    const int piso = (profile.cpu_max_mhz < profile.cpu_min_mhz) ? profile.cpu_max_mhz
                                                                : profile.cpu_min_mhz;
    esp_pm_config_t pm = {
      .max_freq_mhz       = profile.cpu_max_mhz,
      .min_freq_mhz       = piso,
      .light_sleep_enable = profile.light_sleep_when_idle,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK)
    {
      // ESP_ERR_INVALID_ARG aqui quase sempre e frequencia invalidate:
      // no P4 rev >= 3.0 so valem 400, 200, 100 e XTAL/N.
      ESP_LOGW(TAG, "esp_pm_configure(%d/%d) failed: %s",
               profile.cpu_max_mhz, piso, esp_err_to_name(err));
    }
    else
    {
      ESP_LOGI(TAG, "clock variavel: %d a %d MHz conforme a carga; dormir quando idle: %s",
               piso, profile.cpu_max_mhz,
               profile.light_sleep_when_idle ? "sim" : "nao");
    }
#else
    ESP_LOGW(TAG, "pm_dinamico pedido, mas CONFIG_PM_ENABLE nao esta on. "
                  "Descomente a linha no sdkconfig.defaults e recompile.");
#endif
  }

  // So os nomes of que stays LIGADO: uma fileira of "=0/=1" obrigava a saber
  // a order dos campos para read.
  ESP_LOGI(TAG, "profile de energia: ligados [%s%s%s%s%s%s%s], brightness %d",
           profile.imu ? " IMU" : "", profile.microphone ? " microphone" : "",
           profile.speaker ? " high-falante" : "", profile.camera ? " camera" : "",
           profile.radio ? " radio" : "", profile.grove_5v ? " Grove-5V" : "",
           profile.usb_a_5v ? " USB-A-5V" : "", (int)profile.brightness);
}

// ---------------------------------------------------------------------------

void tab5_power_radio(bool turn_on)
{
  M5.getIOExpander(1).digitalWrite(IOE1_WLAN_PWR_EN, turn_on);
  ESP_LOGI(TAG, "energia do C6 (WiFi/Bluetooth): %s", turn_on ? "ligada" : "desligada");
}

static bool s_grove_enabled = false;

void tab5_power_grove(bool turn_on)
{
  anotar_grove(turn_on);
  M5.Power.setExtOutput(turn_on, m5::ext_PA);
  s_grove_enabled = turn_on;
  ESP_LOGI(TAG, "5V da porta Grove: %s", turn_on ? "on" : "desligado");
}

bool tab5_power_grove_on(void) { return s_grove_enabled; }

void tab5_power_camera(bool turn_on)
{
  M5.getIOExpander(0).digitalWrite(IOE0_CAM_RST, turn_on);
}

bool tab5_power_imu_accelerometer_only(void)
{
  // O M5.Imu.sleep() NAO serve aqui: IMU_Base::sleep() e virtual devolvendo
  // false e so o MPU6886 sobrescreve. No BMI270 a chamada nao faz nada.
  for (uint8_t addr : { (uint8_t)0x69, (uint8_t)0x68 })
  {
    if (M5.In_I2C.readRegister8(addr, BMI270_REG_ID, 400000) != BMI270_CHIP_ID)
    {
      continue;
    }
    // PWR_CTRL: bit3 temp | bit2 acc | bit1 gyr | bit0 aux -> so acelerometro.
    // O giroscopio e who come a corrente (centenas of uA contra ~20uA of acel).
    M5.In_I2C.writeRegister8(addr, BMI270_PWR_CTRL, 0x04, 400000);
    // PWR_CONF: bit0 adv_power_save | bit1 fifo_self_wakeup. O M5Unified deixa
    // isso em 0x00 no begin(), ou seja, performance maxima o time todo.
    M5.In_I2C.writeRegister8(addr, BMI270_PWR_CONF, 0x03, 400000);
    ESP_LOGI(TAG, "IMU (BMI270, 0x%02x) em economia: giroscopio desligado", addr);
    return true;
  }
  ESP_LOGW(TAG, "BMI270 nao encontrado (profile.imu estava false?)");
  return false;
}

// ---------------------------------------------------------------------------

// Ultima measurement feita, guardada para who NAO pode ir on I2C.
static tab5_measurement_t s_ultima = { 0, 0, 0, -1 };

tab5_measurement_t tab5_power_latest(void) { return s_ultima; }

// Porcentagem of carga, nossa - o M5Unified nao serve aqui.
//
// O getBatteryLevel() dele faz interpolacao LINEAR entre 3300 e 4150 mV por
// celula (Power_Class.cpp:2341). Dois problemas medidos:
//
// 1. A curva of Li-Po NAO e linear. Ela stays quase plana entre 4,0 e 3,7 V -
//    que sao uns 70% of capacity - e depois despenca. Com reta, os primeiros
//    30% escorrem devagar e os ultimos 10% caem em segundos.
// 2. Nao ha compensacao of carga. A tensao afunda sob consumo e sobe no
//    carregador, e o intervalo inteiro tem so 800mV.
//
// A tabela abaixo tem a FORMA of uma descarga of Li-Po, anchored em 4150mV por
// celula = 100%, que e o pico real desta placa: ele mediu 8,24 a 8,30V com a
// battery cheia, ou seja 4,12 a 4,15V por celula. O carregador termina abaixo
// dos 4,20V of default, e ancorar em 4,20 faria "cheio" marcar 95%.
//
// SUBSTITUIDA pela curva of VERDADE - a tabela generica acima
// (12 pontos, "forma of descarga of Li-Po" chutada a mao) foi jogada fora.
//
// O QUE MUDOU: achamos no energia.csv uma descarga CONTINUA, DE UMA SESSAO SO,
// of 100% (cauda of carga terminando, 8,309V) ate quase morrer (6,574V, o
// aparelho entrou em loop of reinicio por brownout logo depois) - 21,5 horas
// sem interrupcao, sem emenda of sessoes diferentes (o problema que estragou
// as duas tentativas anteriores) e ja com a tensao gravada em %.3f (o outro
// problema, corrigido em energy_log.cpp antes desta analise).
//
// O METODO: Coulomb counting of verdade. Integramos mA*dt (trapezio) since o
// first instante of descarga real ate o end, o que of a capacity
// REALMENTE consumida nesta descarga - 1306,8 mAh, e nao mais uma suposicao.
// Cada sample dessa mesma descarga tem tensao E corrente medidas no MESMO
// instante, entao dá para casar "quanto realmente sobrava" (Coulomb) com "que
// tensao open o aparelho tinha" (V - I*R) point a point - sem chutar forma
// none, sem generalizar of outra battery.
//
// O ERRO DA TABELA VELHA, measured contra este dado: ate +30 PONTOS no storage of
// faixa (dizia 50% com 19,9% real). Pior of que a analise tinha
// achado (+25 pontos) - aquela vinha of sessoes emendadas com quantizacao of
// tensao, entao ainda estava otimista demais sobre o proprio erro.
//
// SOBRE A ETIQUETA DIZER 2400 mAh: a battery fisica traz "2400mAh" impresso,
// quase o dobro of 1306,8 measured aqui. Verificado ANTES of desconfiar of
// measurement: o shunt of INA226 (Power_Class.cpp, bloco dos expansores 0x43/0x44 -
// os mesmos enderecos of Tab5 documentados no topo deste file) esta em
// 0,005 ohm, o value certo - nao e o driver lendo corrente com constante
// trocada. Uma celula Li-Po tipica guarda uns 5-15% of carga entre 3,3V e
// 3,0V por celula (a faixa que o brownout nos impediu of measure aqui) - long e
// demais of sobra pra close 45% of diferenca. Ficamos com o numero MEDIDO:
// label of battery generica inflada e mais comum of que capacity real
// escondida abaixo of point onde o proprio aparelho ja nao liga mais.
//
// R interna (0.268 ohm) e mantido - foi usado para compensar CADA point desta
// nova tabela, entao qualquer erro nele já está "dentro" of curva nova, e
// remedir R exigiria steps of corrente limpos que este dataset já não
// isola bem (a maioria dos steps aqui vem of app abrindo/fechando, não of
// one teste controlado).
static const struct { int mv; int pct; } CURVA[] = {
  { 4155, 100 }, { 4076, 98 }, { 4063, 96 }, { 4053, 94 }, { 4049, 92 },
  { 4044,  90 }, { 4037, 88 }, { 4031, 86 }, { 4023, 84 }, { 4018, 82 },
  { 4014,  80 }, { 4009, 78 }, { 4005, 76 }, { 4001, 74 }, { 3995, 72 },
  { 3989,  70 }, { 3982, 68 }, { 3973, 66 }, { 3965, 64 }, { 3957, 62 },
  { 3950,  60 }, { 3945, 58 }, { 3938, 56 }, { 3933, 54 }, { 3928, 52 },
  { 3923,  50 }, { 3916, 48 }, { 3910, 46 }, { 3903, 44 }, { 3895, 42 },
  { 3886,  40 }, { 3879, 38 }, { 3868, 36 }, { 3863, 34 }, { 3856, 32 },
  { 3850,  30 }, { 3843, 28 }, { 3836, 26 }, { 3828, 24 }, { 3819, 22 },
  { 3810,  20 }, { 3801, 18 }, { 3794, 16 }, { 3786, 14 }, { 3778, 12 },
  { 3766,  10 }, { 3744,  8 }, { 3701,  6 }, { 3645,  4 }, { 3557,  2 },
  { 3302,   0 },
};

int tab5_power_pct(float volts, float mA)
{
  if (volts < 1.0f) { return -1; }          // sem leitura valid

  // Tensao em sleep, corrigida pela corrente. mA e assinado in nossa
  // convencao (+ loading), e a formula unica cobre os dois casos.
  const float open = volts - (mA / 1000.0f) * TAB5_INTERNAL_R;
  const int mv = (int)(open * 500.0f);    // por celula (2S), em mV

  if (mv >= CURVA[0].mv) { return 100; }
  const int n = sizeof(CURVA) / sizeof(CURVA[0]);
  if (mv <= CURVA[n-1].mv) { return 0; }

  for (int i = 1; i < n; ++i) {
    if (mv >= CURVA[i].mv) {
      // Interpola dentro of trecho - a curva e quebrada em retas curtas, que e
      // o que a torna fiel onde a linear failure.
      const int dv = CURVA[i-1].mv - CURVA[i].mv;
      const int dp = CURVA[i-1].pct - CURVA[i].pct;
      return CURVA[i].pct + ((mv - CURVA[i].mv) * dp) / dv;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// ESTIMADOR DE CARGA - a porcentagem que o sistema mostra.
// ---------------------------------------------------------------------------
// A curva acima e boa numa situacao so: descarga, longe of uma carga recente,
// in faixa of corrente em que foi calibrada. Reprocessando o energia.csv of
// (17 mil amostras, 48 trechos of descarga) contra a carga contada
// em mAh:
//   - descarga since cheio: curva e contagem batem em +-3 pontos - OK;
//   - DEPOIS DE UMA CARGA PARCIAL (tirar of tomada no storage): a curva dizia
//     89,8% com 52,6% of verdade - +37 pontos - e levava 5,5 h para voltar a
//     dizer a verdade. A tensao sob carga (e por horas depois dela) sobe
//     muito alem of I*R: e polarizacao quimica, nao resistencia. A correcao of
//     carga antiga ainda PRENDIA o numero in curva,
//     entao durante a carga ele tambem mostrava 94,6%;
//   - com a screen apagada (~30 mA, fora of faixa calibrada) a curva le high;
//   - em 2,7% dos minutos of descarga a % SUBIA (ate +2,4 pontos) when a
//     carga of aparelho mudava.
//
// O ESTIMADOR, o mesmo drawing of one medidor of battery of celular:
//   1. CONTA A CARGA nos dois sentidos: soc += mA * dt / capacity;
//   2. A CURVA SO CORRIGE, devagar (constante of 3 h; 30 min abaixo of 15%,
//      onde ela e ingreme e confiavel), e so when ela e confiavel:
//      descarga entre 45 e 160 mA, e pelo menos 1 h depois of latest carga;
//   3. FIM DE CARGA (corrente caindo com a tensao no ceiling) = 100%;
//   4. O NUMERO MOSTRADO NAO SOBE fora of carga.
// Contra o mesmo file: erro medio 1,8 -> 0,8 point, p95 9,9 -> 2,5,
// pior caso 43,7 -> 4,3, e none subida em descarga.
//
// O point of partida e a curva, no boot. Num reinicio por software (write,
// travamento, watchdog) a battery nao changed: o value sobrevive in RTC e o
// estimador continua of onde estava, em vez of recomecar pela tensao - que
// mente se o reinicio foi logo depois of uma carga.
static const float EST_RELAX_S      = 3600.0f;  // depois of carga, a tensao mente por horas
static const float EST_TAU_H        = 3.0f;
static const float EST_TAU_BAIXO_H  = 0.5f;
static const float EST_I_MIN_MA     = 45.0f;    // faixa of corrente of calibracao of curva
static const float EST_I_MAX_MA     = 160.0f;
static const uint32_t EST_MAGIA     = 0x50C0A7E1u;

// Corrente abaixo disto, com o carregador on E a tensao no ceiling, e o
// mesmo signal of "terminou" que o proprio carregador usa (C/20 a C/10).
static const float TAB5_CARGA_TERMINO_MA = 100.0f;

RTC_NOINIT_ATTR static struct { uint32_t magia; float soc; } s_rtc_soc;

static bool        s_est_ok      = false;
static float       s_soc         = 0.0f;     // o state, em % com fracao
static int64_t     s_est_us      = 0;
static int64_t     s_ult_carga_us = 0;
static bool        s_ja_carregou = false;
static int         s_exibido     = -1;
static const char* s_est_fonte   = "-";

static bool reinicio_of_software(void)
{
  switch (esp_reset_reason()) {
    case ESP_RST_SW: case ESP_RST_PANIC: case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT: case ESP_RST_WDT: return true;
    default: return false;
  }
}

// Chamada of DENTRO of tab5_power_measure() - uma vez por measurement.
static int estimar_pct(float volts, float mA, int pct_curva)
{
  const int64_t now = esp_timer_get_time();
  if (pct_curva < 0) { return s_exibido; }   // leitura invalidate: mantem

  if (!s_est_ok) {
    s_soc = (float)pct_curva;
    s_est_fonte = "curva no boot";
    // A RTC so vale se foi reinicio por software E se ainda e plausivel -
    // 20 pontos of folga cobrem o erro of curva logo depois of uma carga.
    if (reinicio_of_software() && s_rtc_soc.magia == EST_MAGIA
     && s_rtc_soc.soc >= 0.0f && s_rtc_soc.soc <= 100.0f
     && fabsf(s_rtc_soc.soc - (float)pct_curva) <= 20.0f) {
      s_soc = s_rtc_soc.soc;
      s_est_fonte = "continuado da RTC";
    }
    s_est_ok = true;
    s_est_us = now;
    s_exibido = (int)lroundf(s_soc);
    s_rtc_soc.magia = EST_MAGIA; s_rtc_soc.soc = s_soc;
    return s_exibido;
  }

  // 1. CONTAGEM. dt of clock, nao of cadencia. Um buraco of mais of 6 min
  //    nao e continuidade: recomeca pela curva.
  const float dt_h = (float)(now - s_est_us) / 3.6e9f;
  s_est_us = now;
  if (dt_h > 0.0f && dt_h < 0.1f) { s_soc += mA * dt_h / TAB5_CAPACITY_MAH * 100.0f; }
  else if (dt_h >= 0.1f)          { s_soc = (float)pct_curva; s_est_fonte = "curva (lacuna)"; }

  if (mA > 20.0f) { s_ult_carga_us = now; s_ja_carregou = true; }

  // 3. FIM DE CARGA: o par of sinais que o carregador usa para stop.
  if (mA > 0.0f && mA < TAB5_CARGA_TERMINO_MA && (int)(volts * 500.0f) >= (int)(CURVA[0].mv * 0.995f)) {
    s_soc = 100.0f;
  }

  // 2. CORRECAO PELA CURVA, lenta, so onde ela e confiavel.
  const float since_carga_s = s_ja_carregou ? (float)(now - s_ult_carga_us) / 1e6f : 1e9f;
  if (mA < -EST_I_MIN_MA && mA > -EST_I_MAX_MA && since_carga_s > EST_RELAX_S && dt_h > 0.0f && dt_h < 0.1f) {
    const float tau = (pct_curva < 15) ? EST_TAU_BAIXO_H : EST_TAU_H;
    float k = dt_h / tau;
    if (k > 1.0f) { k = 1.0f; }
    s_soc += ((float)pct_curva - s_soc) * k;
  }

  if (s_soc < 0.0f)   { s_soc = 0.0f; }
  if (s_soc > 100.0f) { s_soc = 100.0f; }

  // 4. Fora of carga, o numero mostrado nao sobe.
  int r = (int)lroundf(s_soc);
  if (mA <= 20.0f && s_exibido >= 0 && r > s_exibido) { r = s_exibido; }
  s_exibido = r;

  s_rtc_soc.magia = EST_MAGIA; s_rtc_soc.soc = s_soc;
  return r;
}

void tab5_power_debug_soc(float* soc, float* curva, const char** fonte, float* min_since_carga)
{
  if (soc)   { *soc = s_soc; }
  if (curva) { *curva = (float)tab5_power_pct(s_ultima.volts, s_ultima.mA); }
  if (fonte) { *fonte = s_est_fonte; }
  if (min_since_carga) {
    *min_since_carga = s_ja_carregou ? (float)(esp_timer_get_time() - s_ult_carga_us) / 6e7f : -1.0f;
  }
}

// Anel of history. Guardamos indice of escrita e contagem em vez of scroll
// o vetor: scroll 60 posicoes a cada point seria work a toa, e o point
// chega a cada 30 segundos por anos a fio.
static tab5_point_t s_hist[TAB5_HIST_N];
static int      s_hist_n = 0, s_hist_i = 0;
static uint32_t s_hist_ultimo = 0;

int tab5_power_history(tab5_point_t* output)
{
  if (!output) { return 0; }
  // Desenrola o anel para order cronologica: who desenha quer o time andando
  // of left para a right, nao a order interna of buffer.
  const int comeco = (s_hist_n < TAB5_HIST_N) ? 0 : s_hist_i;
  for (int k = 0; k < s_hist_n; ++k) {
    output[k] = s_hist[(comeco + k) % TAB5_HIST_N];
  }
  return s_hist_n;
}

tab5_measurement_t tab5_power_measure(void)
{
  tab5_measurement_t m;
  m.volts = M5.Power.Ina226.getBusVoltage();
  // NAO usar M5.Power.getBatteryCurrent() aqui: ele devolve int32_t, e o LSB of
  // corrente of INA226 e 61uA (max_expected_current 2.0A / 32768). Qualquer
  // leitura abaixo of 1mA e truncada para 0 - foi o que fez a corrente aparecer
  // como "0.0 mA" fixo. Lendo o float direto mantemos a resolucao.
  // Sinal: o shunt esta on of mode que carga le negativo, entao invertemos
  // para a convencao (+) loading / (-) descarregando.
  m.mA    = -1000.0f * M5.Power.Ina226.getShuntCurrent();
  // POTENCIA = TENSAO x CORRENTE, aqui mesmo. O registrador of potencia of
  // INA226 e exatamente essa conta feita pelo chip: le-lo era uma transacao
  // I2C a mais por segundo (awake e em sleep) para one numero que ja
  // temos. Sempre positiva, como o registrador.
  m.mW    = m.volts * fabsf(m.mA);
  // A % mostrada vem of ESTIMADOR (contagem of mAh corrigida devagar pela
  // curva) - ver estimar_pct(). A curva sozinha e tab5_power_pct().
  m.pct   = estimar_pct(m.volts, m.mA, tab5_power_pct(m.volts, m.mA));
  s_ultima = m;          // publica para as outras tarefas

  // Amostra o history a cada TAB5_HIST_SEG. A medicao acontece a 1Hz; guardar
  // everything daria 30s of window em vez of 30min.
  const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
  float c;
  const bool tem_temp = tab5_sensors_temp(&c);

  // NAO COMECA O HISTORICO SEM TEMPERATURA - o grafico of temperatura saia
  // com buracos.
  //
  // A PRIMEIRA chamada desta funcao acontece no boot, via tab5_power_log
  // ("boot") em main.cpp - e nesse instante tab5_sensors_init() AINDA NAO
  // rodou: ele mora dentro of tarefa of interface (vTaskInterface, ver
  // kernel/core.cpp), que sistema_iniciar() so cria lines DEPOIS, e main.cpp
  // chama o log of boot ANTES of sistema_iniciar(). tab5_sensors_temp()
  // devolvia false nesse momento, e a condicao "s_hist_ultimo==0" (a MESMA
  // que garante que a primeira measurement sempre entra no anel) fazia essa
  // sample SEMPRE virar o slot MAIS ANTIGO of history, com temp travada
  // em -273 - e ficava ali por 30 minutos INTEIROS of uptime, toda vez que o
  // aparelho reiniciava. Como o desenvolvimento reinicia o aparelho o time
  // todo (flash.sh), o grafico of temperatura SEMPRE parecia faltar
  // informacao logo in borda mais antiga.
  //
  // A correcao: se ainda nao ha o PRIMEIRO history (s_hist_ultimo==0) e a
  // temperatura ainda nao esta disponivel, esta sample so guarda a MEDIDA
  // on vivo (s_ultima, ja feito acima) e NAO liga o clock of history -
  // a next chamada of tab5_power_measure() (1s depois, ainda no boot) tenta
  // of novo, e ja deve achar o sensor ready. Nao muda nada para who chama
  // depois of boot: se tem_temp for false so DAQUI a 30 minutos running (o
  // sensor falhando of verdade), o comportamento antigo (-273 no slot) iteration
  // a valer, porque a essa height s_hist_ultimo!=0 e a condicao normal
  // assume.
  if (s_hist_ultimo != 0 || tem_temp) {
    if (s_hist_ultimo == 0 || (now - s_hist_ultimo) >= TAB5_HIST_SEG) {
      s_hist_ultimo = now;
      s_hist[s_hist_i].volts = m.volts;
      s_hist[s_hist_i].mA    = m.mA;
      s_hist[s_hist_i].pct   = (int16_t)m.pct;
      // -273 e o "nao measured": impossivel como temperatura, entao o grafico o
      // reconhece sem precisar of one field of validade on lado.
      s_hist[s_hist_i].temp = tem_temp ? c : -273.0f;
      s_hist_i = (s_hist_i + 1) % TAB5_HIST_N;
      if (s_hist_n < TAB5_HIST_N) { ++s_hist_n; }
    }
  }

  return m;
}

int tab5_power_cpu_mhz(void)
{
  // esp_clk_cpu_freq() devolve Hz e e atualizado pelo proprio DFS a cada troca
  // of frequencia - por isso ele serve para ver o PM trabalhando on vivo.
  return esp_clk_cpu_freq() / 1000000;
}

void tab5_power_log(const char* label)
{
  tab5_measurement_t m = tab5_power_measure();
  // Compacto of proposito: com o prefixo of log ("I (1606) power: ") a version
  // anterior passava of 90 colunas e quebrava em duas lines no terminal, o que
  // atrapalha justamente who esta comparando varias medidas seguidas.
  ESP_LOGI(TAG, "%-8s %dMHz %.2fV %.1fmA %.0fmW %d%%",
           label, tab5_power_cpu_mhz(), m.volts, m.mA, m.mW, m.pct);
}

void tab5_power_locks(const char* label)
{
#if CONFIG_PM_ENABLE
  printf("\n--- locks de PM (%s) ---\n", label);
  // stdout e nao ESP_LOG of proposito: o dump ja vem formatado em varias
  // lines, e o prefixo of log em cada uma tornaria a tabela ilegivel.
  esp_pm_dump_locks(stdout);
  printf("--- end (%s) ---\n\n", label);
#else
  (void)label;
#endif
}

// (tab5_power_mostrar exited: o panel of energia e drawn pelo
// proprio app Energia. O HAL mede; who desenha sao os apps.)


// ---------------------------------------------------------------------------
// MODO DE ESPERA
// ---------------------------------------------------------------------------

// O acesso aos membros protegidos of Panel_DSI mora now num point so,
// shared com tab5_screen.cpp - ver o aviso grande em
// tab5_internal_dsi_panel.hpp (consolidado; antes disto havia
// uma copia deste mesmo truque em cada file).
namespace {
  esp_lcd_panel_handle_t panel_dpi(void)
  {
    auto* p = tab5_internal_dsi_panel();
    return p ? p->get_handle() : nullptr;
  }

  bool s_em_sleep = false;
  bool s_desmontado = false; // panel e barramento DSI destruidos (ver desmontar_tela)
  bool s_so_luz = false;     // sleep of screen TDDI: so a luz apagou (ver tab5_panel)
  int  s_cpu_normal = TAB5_CPU_MAX_MHZ;   // ceiling a restaurar, guardado on entrar em sleep
}

bool tab5_power_is_sleeping(void) { return s_em_sleep; }

// ---- qual screen (ver tab5_power.hpp) -----------------------------------------
// O address of touch separa as familias sem ambiguidade: o GT911 responde em
// 0x14 (ou 0x5D) e o touch dos TDDI of Sitronix em 0x55. Entre os dois ST, o
// M5GFX nao guarda o modelo em lugar publico - mas programa cada one com o seu
// clock of pixel (ST7121 a 70 MHz, ST7123 a 80), value_read aqui antes of alguem
// mexer nele. Um ST que nao bate com nenhum dos dois continua sendo tratado
// como TDDI: o name exit "desconhecido", o comportamento e o conservador.
static int8_t s_panel = -1;   // -1 = ainda nao detectado

tab5_panel_t tab5_panel(void)
{
  if (s_panel >= 0) { return (tab5_panel_t)s_panel; }
  tab5_panel_t r = TAB5_PANEL_UNKNOWN;
  auto* tp = M5.Display.touch();
  auto* p  = tab5_internal_dsi_panel();
  if (tp && p) {
    const uint8_t end = tp->config().i2c_addr;
    const int mhz = p->config_detail().dpi_freq_mhz;
    if (end == 0x14 || end == 0x5D) { r = TAB5_PANEL_ILI9881C; }
    else if (mhz == 70)             { r = TAB5_PANEL_ST7121; }
    else if (mhz == 80)             { r = TAB5_PANEL_ST7123; }
    ESP_LOGI(TAG, "tela: %s (toque em 0x%02X, clock de fabrica %d MHz)",
             r == TAB5_PANEL_ILI9881C ? "ILI9881C + GT911" :
             r == TAB5_PANEL_ST7123   ? "ST7123 (TDDI)" :
             r == TAB5_PANEL_ST7121   ? "ST7121 (TDDI)" : "desconhecida", end, mhz);
  } else {
    ESP_LOGW(TAG, "tela: sem panel ou sem toque - chamado antes do M5.begin()?");
  }
  s_panel = (int8_t)r;
  return r;
}

const char* tab5_panel_name(void)
{
  switch (tab5_panel()) {
    case TAB5_PANEL_ILI9881C: return "ILI9881C + GT911";
    case TAB5_PANEL_ST7123:   return "ST7123";
    case TAB5_PANEL_ST7121:   return "ST7121";
    default:                   return "desconhecida";
  }
}

bool tab5_panel_touch_gt911(void) { return tab5_panel() == TAB5_PANEL_ILI9881C; }

// O sleep fundo (SLPIN + unmount o DSI) so onde o touch sobrevive a ele.
static bool sleep_fundo(void) { return tab5_panel_touch_gt911(); }

// Ajusta so o ceiling of clock. O piso stays em 40MHz (o "XTAL" of que fala o
// comentario antigo) - MAS SO QUANDO O TETO PEDIDO E MAIOR OU IGUAL A ISSO.
//
// BUG ENCONTRADO, pelo comando 'energia sleep' testando pela
// primeira vez one ceiling abaixo of 40: esp_pm_configure EXIGE min <= max, e o
// piso vinha fixo em 40 nesta funcao - entao "energia sleep 20" pedia
// max=20/min=40 e o proprio IDF recusava com ESP_ERR_INVALID_ARG, sempre,
// para qualquer ceiling abaixo of 40. Ninguem tinha pego isto porque nenhum
// chamador of verdade (tab5_power_sleep()/resume(), sempre >=40 ate hoje)
// jamais pediu one ceiling tao bottom - o comando of console foi o first.
//
// Quando o ceiling request stays ABAIXO of 40, o piso acompanha (min = max): nao
// ha mais faixa of DFS para variar, o clock stays fixo naquele value - que e
// exatamente a intencao of who esta testando "meu piso E este numero".
static bool ajustar_teto(int mhz)
{
#if CONFIG_PM_ENABLE
  const int piso = (mhz < 40) ? mhz : 40;
  esp_pm_config_t pm = { .max_freq_mhz = mhz, .min_freq_mhz = piso, .light_sleep_enable = false };
  esp_err_t err = esp_pm_configure(&pm);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_pm_configure(max=%d) failed: %s", mhz, esp_err_to_name(err));
    return false;
  }
  return true;
#else
  ESP_LOGW(TAG, "ceiling de clock ignorado: CONFIG_PM_ENABLE nao esta on");
  (void)mhz;
  return false;
#endif
}

// Ver o aviso grande em tab5_power.hpp. So reaproveita ajustar_teto() - o
// mesmo caminho que sleep()/resume() ja usam - MAS sem touch em panel,
// backlight ou fluxo DSI nenhum: a screen continua exatamente como estava,
// enabled e desenhando. E por isso que of para testar "panel devagar + CPU
// devagar, os dois ACORDADOS" - nenhum dos outros caminhos faz isso.
bool tab5_power_cpu_ceiling(int mhz)
{
  const int antes = tab5_power_cpu_mhz();
  const bool ok = ajustar_teto(mhz);
  ESP_LOGI(TAG, "ceiling do CPU (tela acordada): %d MHz -> %s",
           antes, ok ? "aceito" : "RECUSADO (log de erro acima)");
  return ok;
}

// Varia o brightness suavemente entre dois valores.
//
// Mexe no PAINEL direto, e nao em M5.Display.setBrightness(), of proposito: o
// LGFX guarda o brightness "oficial" num membro _brightness, e e ele que o
// wakeup() restaura. Se o fade usasse setBrightness(), o value salvo viraria 0
// e a screen nunca mais acenderia sozinha on wake.
static void (*s_gancho_rampa)(void) = nullptr;
void tab5_power_ramp_hook(void (*step)(void)) { s_gancho_rampa = step; }

static void esmaecer(uint8_t of, uint8_t para, int passos = 16, int ms = 12)
{
  auto* p = M5.Display.panel();
  if (!p) { return; }
  for (int i = 1; i <= passos; ++i) {
    const int b = (int)of + ((int)para - (int)of) * i / passos;
    p->setBrightness((uint8_t)b);
    if (s_gancho_rampa) { s_gancho_rampa(); }
    vTaskDelay(pdMS_TO_TICKS(ms));
  }
  p->setBrightness(para);
}

// DOIS CAMINHOS QUE NAO FUNCIONARAM - o registro, para ninguem repetir.
//
// 1. Parar o fluxo DSI com esp_lcd_dpi_panel_set_pattern() no sleep (e
//    MIPI_DSI_PATTERN_NONE no resume). Medido: depois of NONE a ponte DSI as
//    vezes nao iteration a alimentar o panel - screen acesa, palida, sem content,
//    por ate 15 minutos, com o log of despertar inteiro e normal. O
//    set_pattern foi feito para exibir default of teste, nao para pausar.
//
// 2. Baixar o clock com o fluxo on. Da underrun em todo frame, e o driver
//    loga com ESP_DRAM_LOGE (esp_rom_printf, ignora esp_log_level_set): ~6,5 ms
//    of UART bloqueante dentro of interrupcao a cada frame. E o proprio driver
//    avisa que o underrun pode deixar a screen azul sem iteration.
//
// O que funciona e o que esta abaixo: unmount o panel e o barramento.

// Desfaz one sleep que failed no storage, SEM wake of supetao. Usar
// M5.Display.wakeup() aqui era one erro: ele restaura o brightness in hora.
static void desfazer_sleep(void)
{
  auto* p = M5.Display.panel();
  if (p) { p->setSleep(false); }
  vTaskDelay(pdMS_TO_TICKS(120));
  tab5_power_wake_screen();
}

// ---------------------------------------------------------------------------
// DESMONTAR E REMONTAR A TELA
// ---------------------------------------------------------------------------
//
// O caminho que as duas chaves acima nao conseguiram open. Medido em
// com esp_pm_dump_locks(): o panel segura DUAS locks of PM, e
// none delas se released com set_pattern() nem com sleep():
//
//     dsi_dpi   CPU_FREQ_MAX     <- prende a CPU em 360MHz
//     dsi_phy   NO_LIGHT_SLEEP   <- proibe light sleep
//
// No fonte of IDF as duas sao adquiridas in CRIACAO e liberadas so no
// DESTRUTOR (esp_lcd_panel_dpi.c 291 -> 405; esp_lcd_mipi_dsi_bus.c 72 -> 151).
// Logo: para solta-las, destruir. Nao ha storage-termo.
//
// Destruir resolve os tres problemas of uma vez, e por isso vale o risco:
//   - sem fluxo DPI nao ha underrun, entao o clock pode cair of verdade;
//   - dsi_dpi released, o ceiling of clock passa a valer;
//   - dsi_phy released, light sleep deixa of ser impossivel.
//
// INVARIANTE QUE NAO PODE SER QUEBRADA: entre unmount e remontar, NINGUEM
// pode draw. O framebuffer pertence on panel DPI
// (esp_lcd_dpi_panel_get_frame_buffer em Panel_DSI.cpp:143) e e liberado junto
// com ele - draw ali e write em memoria devolvida on heap. Neste sistema
// isso se sustenta porque so a tarefa of interface desenha, e e ela who chama
// sleep()/resume(); o loop dela em sleep nao toca no display.

static bool desmontar_screen(void)
{
  auto* p = tab5_internal_dsi_panel();
  if (p == nullptr) { return false; }
  auto* bus = p->getBusDSI();
  if (bus == nullptr) { return false; }

  // o double buffer morre PRIMEIRO: o vetor of lines dela aponta para dentro
  // of framebuffer que o esp_lcd_panel_del() esta prestes a release.
  tab5_screen_shutdown();

  // Ordem: panel antes of barramento. O panel DPI foi criado A PARTIR of
  // barramento; erase o barramento first deixaria o panel com referencia
  // para algo que nao existe mais.
  esp_lcd_panel_handle_t dpi = p->get_handle();
  if (dpi != nullptr)
  {
    esp_err_t err = esp_lcd_panel_del(dpi);   // released dsi_dpi, libera o framebuffer
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "unmount: esp_lcd_panel_del failed: %s", esp_err_to_name(err));
      return false;
    }
    p->clear_handle();
  }

  // O lineArray e alocado pelo Panel_DSI::init com heap_alloc_dma e nunca
  // liberado por ninguem. Alem of vazamento, e ELE o guarda que faz o init
  // return_card false in segunda chamada ("if (_lines_buffer != nullptr) return
  // false"). Sem reset isto, remontar e impossivel.
  uint8_t** lines = p->get_lines();
  if (lines != nullptr) { lgfx::heap_free(lines); p->clear_lines(); }

  // O buffer ja foi liberado junto com o panel; deixar o ponteiro velho seria
  // one pendurado esperando o first drawing distraido.
  p->clear_buffer();

  // release() of M5GFX apaga o IO DBI, apaga o barramento DSI (soltando
  // dsi_phy) e devolve o channel of LDO que alimenta a PHY. Teardown complete,
  // e feito pela propria biblioteca - nao precisamos imitar nada.
  bus->release();
  return true;
}

static bool montar_screen(void)
{
  auto* p = tab5_internal_dsi_panel();
  if (p == nullptr) { return false; }
  auto* bus = p->getBusDSI();
  if (bus == nullptr) { return false; }

  // ATENCAO, QUEM CHAMAR ISTO: A LUZ VOLTA APAGADA.
  //
  // O Panel_Device::init() la dentro faz _light->init(0) (Panel_Device.cpp:67).
  // Ou seja, mount o panel zera o PWM of backlight - e o _brightness of LGFX
  // NAO acompanha, porque ele mora no LGFX_Device e nao no panel
  // (LGFXBase.hpp:1519). Resultado: M5.Display.getBrightness() continua
  // respondendo 80 com a lampada em 0. A screen stays enabled, desenhando certo, e
  // invisivel.
  //
  // No caminho of sleep isso e DE PROPOSITO e nao pode mudar: o resume()
  // devolve o panel no escuro justamente para who chamou repintar antes of
  // wake, e so entao chamar tab5_power_wake_screen(). Quem usar montar_tela()
  // por outro reason tem que wake por conta propria - foi exatamente o que
  // faltou in primeira version of troca of clock, e a screen nascia preta ate o
  // first sleep.
  //
  // Cala o LEDC durante a remontagem, e SO durante ela.
  //
  // Panel_Device::init() faz _light->init(0) (Panel_Device.cpp:67), ou seja
  // reconfigura o PWM of backlight. O GPIO 22 ja esta reservado since o boot,
  // entao o driver avisa "GPIO 22 is not usable, maybe conflict with others" a
  // CADA vez que a screen acorda. E benigno - o brightness continua funcionando, o
  // wake() logo em seguida prova - mas repetido a cada touch vira ruido que
  // esconde aviso of verdade. Restaurado no end: fora desta window one aviso of
  // LEDC iteration a significar alguma coisa.
  esp_log_level_set("ledc", ESP_LOG_ERROR);

  // Redundante of proposito: o Panel_Device::init() la dentro tambem chama
  // _bus->init() (line 74), e o Bus_DSI::init() tem guarda para nao create duas
  // vezes. Chamamos aqui para que uma failure of barramento seja reportada como
  // failure of BARRAMENTO, e nao como one "Panel_DSI::init failed" generico.
  if (!bus->init())
  {
    ESP_LOGE(TAG, "mount: Bus_DSI::init failed");
    esp_log_level_set("ledc", ESP_LOG_WARN);
    return false;
  }
  // use_reset=true: manda SWRESET e reexecuta a lista of inicializacao of
  // panel. Como o barramento acabou of nascer, o panel tem que ser
  // reprogramado of zero - nao of para presumir state nenhum.
  const bool ok = p->init(true);
  esp_log_level_set("ledc", ESP_LOG_WARN);
  if (!ok)
  {
    ESP_LOGE(TAG, "mount: Panel_DSI::init failed");
    return false;
  }
  return true;
}

bool tab5_power_sleep(int cpu_mhz)
{
  if (s_em_sleep) { return true; }

  if (panel_dpi() == nullptr) {
    ESP_LOGE(TAG, "sleep: nao achei o handle do panel DPI");
    return false;
  }

  // 1. Escurece SUAVE.
  tab5_mark("sleep: apagando", 0);
  esmaecer(M5.Display.getBrightness(), 0);

  // TELA TDDI (ST7123/ST7121): para aqui, so com a luz apagada. Painel
  // awake e fluxo DSI of pe - o touch, que mora no chip of screen, continua
  // scanning e acorda a screen como sempre. Custa o clock high of panel on;
  // e o preco of nao arriscar one aparelho que nao acorda mais. Ver tab5_panel().
  if (!sleep_fundo()) {
    s_so_luz = true;
    s_em_sleep = true;
    ESP_LOGI(TAG, "sleep: luz apagada (tela %s: panel segue awake para o toque)",
             tab5_panel_name());
    return true;
  }

  // 2. Adormece o panel (o brightness ja esta em zero).
  tab5_mark("sleep: SLPIN", 0);
  M5.Display.sleep();
  M5.Display.waitDisplay();         // espera o BARRAMENTO, nao o panel
  vTaskDelay(pdMS_TO_TICKS(200));   // SLPIN leva ate ~120ms para valer

  s_cpu_normal = tab5_power_cpu_mhz();

  // 3. Desmonta panel e barramento. E o que released as locks of PM - ver o
  //    bloco grande acima. Depois daqui NAO EXISTE FRAMEBUFFER: ninguem pode
  //    draw ate o montar_tela() of resume().
  const uint32_t t0 = tab5_now_ms();
  if (!desmontar_screen()) {
    ESP_LOGE(TAG, "sleep: unmount failed - voltando ao state aceso");
    desfazer_sleep();
    return false;
  }
  s_desmontado = true;
  ESP_LOGI(TAG, "sleep: DSI desmontado em %lu ms", (unsigned long)(tab5_now_ms() - t0));

  // 4. Baixa o clock - sem panel nao ha underrun, entao pode.
  if (!ajustar_teto(cpu_mhz)) {
    ESP_LOGE(TAG, "sleep: nao consegui baixar o clock para %d MHz", cpu_mhz);
    montar_screen();
    s_desmontado = false;
    desfazer_sleep();
    return false;
  }

  s_em_sleep = true;
  ESP_LOGI(TAG, "sleep: DSI desmontado, clock %d -> %d MHz", s_cpu_normal, tab5_power_cpu_mhz());
  // A PROVA. Se a desmontagem fez o que promete, dsi_dpi e dsi_phy somem of
  // lista of locks - e ai o clock cair of verdade deixa of ser esperanca.
  // UMA VEZ POR BOOT: 'energia locks' mostra when alguem pede.
  static bool s_provou = false;
  if (!s_provou) { s_provou = true; tab5_power_locks("primeiro sleep"); }
  return true;
}

bool tab5_power_resume(void)
{
  if (!s_em_sleep) { return true; }

  // Repouso of screen TDDI: nada foi disabled alem of luz, e a luz who acende
  // e o tab5_power_wake_screen(), depois of redraw.
  if (s_so_luz) {
    s_so_luz = false;
    s_em_sleep = false;
    ESP_LOGI(TAG, "resume: panel ja estava no ar (sleep so de luz)");
    return true;
  }

  // 1. Sobe o clock ANTES of qualquer coisa visivel, e CONFIRMA: o
  //    montar_tela() reprograma o panel e cria o fluxo DPI, e fazer isso a
  //    90MHz seria comecar ja sem banda of PSRAM.
  ajustar_teto(s_cpu_normal);
  int mhz = 0;
  for (int i = 0; i < 20; ++i) {          // ate 100ms, checando a cada 5ms
    mhz = tab5_power_cpu_mhz();
    if (mhz >= s_cpu_normal) { break; }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (mhz < s_cpu_normal) {
    // Falha segura: nao acende. Melhor escuro que rasgando.
    ESP_LOGE(TAG, "resume: clock ficou em %d MHz (esperado %d) - tela segue apagada",
             mhz, s_cpu_normal);
    return false;
  }

  // 2. Remonta panel e barramento. Painel recem-criado ja nasce awake (o
  //    init(true) manda SWRESET e reexecuta a inicializacao) e com a LUZ
  //    APAGADA: who chamou redesenha no escuro e so entao chama
  //    tab5_power_wake_screen().
  const uint32_t t0 = tab5_now_ms();
  bool ok = montar_screen();
  if (!ok) {
    // Uma segunda chance: a primeira failure pode ser one LDO ou uma PHY que
    // precisou of mais time para assentar depois of release.
    ESP_LOGE(TAG, "resume: remontagem failed - tentando mais uma vez");
    vTaskDelay(pdMS_TO_TICKS(100));
    ok = montar_screen();
  }
  if (!ok) {
    // Sem panel o aparelho e one tijolo escuro e nao existe caminho of code
    // que o traga of iteration. Reiniciar e a menos ruim: os ajustes estao in NVS.
    ESP_LOGE(TAG, "resume: remontagem failed DUAS vezes - reiniciando");
    vTaskDelay(pdMS_TO_TICKS(200));   // deixa a UART esvaziar antes of reset
    esp_restart();
  }
  s_desmontado = false;

  // A PRIORIDADE NO AXI, REAFIRMADA. Os registradores of ICM nao pertencem on
  // panel e nao deveriam ser tocados pela destruicao dele - mas "nao
  // deveriam" nao e garantia escrita em lugar nenhum, e o cost sao quatro
  // escritas em registrador contra wake no default of fabrica, que e
  // exatamente a condicao of piscada azul.
  tab5_bus_prioritize_screen(tab5_bus_is_prioritized());

  s_em_sleep = false;
  ESP_LOGI(TAG, "resume: DSI remontado em %lu ms, clock em %d MHz",
           (unsigned long)(tab5_now_ms() - t0), mhz);
  return true;
}

// ---------------------------------------------------------------------------
// RELOGIO DE PIXEL DO PAINEL - a alavanca permanente contra a piscada azul
// ---------------------------------------------------------------------------
// A CONTA, que e o argumento inteiro.
//
// O panel e 720x1280 e o M5GFX o programa com portico of 140+40+40 in
// horizontal e 20+4+20 in vertical (M5GFX.cpp:3214, ramo of ILI9881C). Entao
// cada frame custa, em ciclos of pixel:
//
//   width total = 720 + 220 = 940
//   height total  = 1280 +  44 = 1324
//   ciclos/frame = 940 * 1324 = 1.244.560
//
// A 80MHz isso of 64,3 frames por segundo. Cada frame le 720*1280*2 bytes =
// 1,84MB of PSRAM. Ou seja: o panel puxa 118,5 MB/s of PSRAM O TEMPO TODO, e
// nao ha nada que o code faca que reduza isso - e o hardware scanning.
//
// A 60MHz sao 48,2 frames por segundo e 88,9 MB/s: VINTE E CINCO POR CENTO a
// menos, para sempre, em toda transicao, em todo app, sem ninguem precisar
// lembrar of nada. E a folga por line cresce junto: a line active passa of
// 9,0us para 12,0us e o intervalo of recarga of FIFO of 2,75us para 3,67us.
//
// O que se perde: 64Hz viram 48Hz. Nesta interface - listas, botoes, one jogo of
// cobrinha em grade - nao ha nada que precise of 64 frames por segundo.
//
// POR QUE 60 E NAO OUTRO NUMERO: e o default of proprio Panel_DSI of M5GFX
// (Panel_DSI.hpp:47, dpi_freq_mhz = 60). O 80 vem of uma line especifica of
// M5GFX para este panel; os porticos sao contados em PIXELS e escalam junto
// com o clock, entao a geometria continua valid.
//
// HONESTIDADE: nao consegui measure isto em bancada. Por isso existem duas networks
// of seguranca: se a remontagem falhar, voltamos sozinhos para o value antigo;
// e o console muda o value em execucao ('screen clock 80'), sem regravar.
static uint16_t s_dpi_mhz_request = 0;   // 0 = nunca mexemos

int tab5_power_dpi_clock(void)
{
  auto* p = tab5_internal_dsi_panel();
  if (p == nullptr) { return 0; }
  return (int)p->config_detail().dpi_freq_mhz;
}

double tab5_power_frames_per_second(void)
{
  auto* p = tab5_internal_dsi_panel();
  if (p == nullptr) { return 0; }
  const auto d = p->config_detail();
  const double l = p->width() + d.hsync_back_porch + d.hsync_pulse_width + d.hsync_front_porch;
  const double a = p->height()  + d.vsync_back_porch + d.vsync_pulse_width + d.vsync_front_porch;
  return (l > 0 && a > 0) ? d.dpi_freq_mhz * 1e6 / (l * a) : 0;
}

bool tab5_power_dpi_clock(int mhz)
{
  // Fora of faixa, LIMITA em vez of recusar: who pede 84 quer
  // "o maximum", nao one erro. Mesma faixa of SETTINGS_PANEL_CLOCK_MIN_MHZ/MAX.
  if (mhz > 80) { mhz = 80; }
  if (mhz < 24) { mhz = 24; }

  // Tela TDDI: o clock stays no of fabrica (ver tab5_panel). Pedir o mesmo
  // value que ja esta e sucesso; qualquer outro, recusa sem mexer em nada.
  if (!tab5_panel_touch_gt911()) {
    if (mhz == tab5_power_dpi_clock()) { return true; }
    ESP_LOGW(TAG, "clock do panel: fixo em %d MHz na tela %s",
             tab5_power_dpi_clock(), tab5_panel_name());
    return false;
  }

  // ACHADO: chamar isto com a screen EM REPOUSO e perigoso e nao
  // tinha guarda none. Em sleep fundo o panel ja esta
  // desmontado - _disp_panel_handle e _lines_buffer zerados -, mas o OBJETO
  // Panel_DSI continua vivo (so os ponteiros internos morrem), entao
  // tab5_internal_dsi_panel() abaixo devolve nao-nulo e o code seguia em frente. O
  // switch() chamaria desmontar_tela() e bus->release() UMA SEGUNDA VEZ sobre
  // one barramento que ja foi liberado - o Bus_DSI of M5GFX nao promete ser
  // idempotente a one release() repetido, e essa e a receita mais provavel of
  // travamento observado on testar 'screen clock'/'energia ceiling' com a screen
  // dormindo. A output: recusar aqui, screen por screen, e return_card o controle para
  // who chamou - a mesma politica of kernel/core.cpp para so wake remexer no
  // panel.
  if (s_em_sleep) {
    ESP_LOGE(TAG, "clock do panel: a tela esta em sleep (panel desmontado) - "
                  "acorde primeiro e tente de novo");
    return false;
  }

  auto* p = tab5_internal_dsi_panel();
  if (p == nullptr) { return false; }

  const uint16_t antigo = p->config_detail().dpi_freq_mhz;
  if (antigo == (uint16_t)mhz) { return true; }

  // O caminho e o MESMO of sleep: unmount e mount. Ele ja roda a cada
  // despertar, entao nao e one caminho novo - e o mais exercitado of modulo.
  // Reaproveitar em vez of inventar outro e o que mantem uma unica maneira of
  // recriar o panel, e portanto one unico lugar para consertar when break.
  // A LUZ VOLTA JUNTO, e mora DENTRO of lambda of proposito.
  //
  // Ha dois caminhos que remontam aqui: o clock novo e a iteration on antigo
  // when ele nao sobe. Acendendo so no caminho of sucesso, one panel que
  // recusasse 60MHz voltaria para 80 funcionando e INVISIVEL - o pior desfecho
  // possivel, porque o log diria "voltei on anterior" e a screen continuaria
  // preta. Toda remontagem bem-sucedida acende, sem excecao.
  //
  // Sem risco of mostrar lixo: o esp_lcd_new_panel_dpi aloca os framebuffers
  // com heap_caps_calloc (esp_lcd_panel_dpi.c:238), entao o panel novo nasce
  // scanning PRETO.
  auto apply_clock = [&](uint16_t v) -> bool {
    auto det = p->config_detail();
    det.dpi_freq_mhz = v;
    p->config_detail(det);
    if (!desmontar_screen()) { return false; }
    if (!montar_screen())    { return false; }
    // Reescrever o mesmo numero e o que empurra o brightness para o hardware of
    // novo - ver o aviso no high of montar_tela().
    M5.Display.setBrightness(M5.Display.getBrightness());
    return true;
  };

  ESP_LOGI(TAG, "clock do panel: %u -> %d MHz", (unsigned)antigo, mhz);
  if (!apply_clock((uint16_t)mhz))
  {
    // VOLTA SOZINHO. Um panel que nao sobe deixaria o aparelho cego, e o
    // user sem storage of descobrir por que - o value novo e justamente o que
    // nao consigo testar aqui.
    ESP_LOGE(TAG, "o panel nao subiu a %d MHz - voltando para %u", mhz, (unsigned)antigo);
    if (!apply_clock(antigo)) {
      ESP_LOGE(TAG, "nem no value antigo o panel voltou - reiniciando");
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
    }
    return false;
  }

  s_dpi_mhz_request = (uint16_t)mhz;

  // As paginas morreram junto com o panel velho (o desmontar_tela chama
  // tab5_screen_shutdown). O compositor percebe pela generation e manda a screen
  // inteira para o panel novo - nao ha nada a fazer aqui.
  //
  // A PRIORIDADE NO AXI, sim, precisa ser reafirmada. Os registradores of ICM
  // sobrevivem a destruicao of panel, mas depender disso e depender of one
  // detalhe que nunca foi prometido. Custa quatro escritas.
  tab5_bus_prioritize_screen(tab5_bus_is_prioritized());
  return true;
}

void tab5_power_wake_screen(void)
{
  uint8_t alvo = M5.Display.getBrightness();

  // REDE DE SEGURANCA: se o brightness guardado pelo LGFX for zero, wake com ele
  // deixaria a screen enabled e escura para sempre - panel mostrando content sem
  // luz none, que e o "esbranquicado apagado". Nunca aceitar zero aqui.
  if (alvo == 0) {
    ESP_LOGW(TAG, "wake: brightness guardado era 0 - usando 80 como piso");
    alvo = 80;
    M5.Display.setBrightness(alvo);   // corrige tambem o value guardado
  }

  // PARTIDA A FRIO. Com o backlight vindo of zero, one duty muito bottom nao
  // acende: o driver of LED precisa of one pulso minimum para exit of sleep,
  // e depois que ja esta aceso aguenta duty bem menor (por isso a barra of
  // Ajustes mostrava luz no minimum e o despertar voltava preto). Quando o alvo
  // e bottom, sobe first a one level seguro, segura one instante e so entao
  // desce on alvo - o olho ve one brightness que se acomoda, nao uma screen morta.
  static const uint8_t PISO_PARTIDA = 28;
  if (alvo < PISO_PARTIDA) {
    ESP_LOGI(TAG, "wake: partida a frio %d -> %d -> %d", 0, (int)PISO_PARTIDA, (int)alvo);
    esmaecer(0, PISO_PARTIDA, 8, 8);
    vTaskDelay(pdMS_TO_TICKS(80));
    esmaecer(PISO_PARTIDA, alvo, 12, 12);
    return;
  }

  ESP_LOGI(TAG, "wake: rampa 0 -> %d", (int)alvo);
  esmaecer(0, alvo);
}

// tab5_power_diagnostico() (le registradores crus of INA226) foi removida em
// nasceu para a investigacao of calibracao of battery, que
// fechou (ver o registro em memoria "tab5-battery-
// calibracao"), e ninguem chamava mais (nem console, nem app nenhum).
