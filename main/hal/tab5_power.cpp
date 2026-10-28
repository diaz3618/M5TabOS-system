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

// Necessario para chegar no handle do panel DPI (ver tab5_power_sleep).
#include "tab5_internal_dsi_panel.hpp"

static const char *TAG = "power";

// ---------------------------------------------------------------------------
// Mapa dos expansores do Tab5 (dois PI4IOE5V6408 no I2C interno).
// Levantado do proprio M5Unified: Power_Class.cpp, ramo board_M5Tab5.
//
// Expansor 0 (0x43)                     Expansor 1 (0x44)
//   7 HP_DET    (input)                 7 CHG_EN       (output, high)
//   6 CAM_RST   (output, high=ativa)       6 CHG_STAT     (input)
//   5 TP_RST    (output, high=touch on)    5 nCHG_QC_EN   (output)
//   4 LCD_RST   (output, high=panel on)   4 PWROFF_PULSE (output)
//   3 NC / Hat power no Tab5X             3 USB5V_EN     (input c/ pull)
//   2 EXT5V_EN  (input c/ pull)         2 NC
//   1 SPK_EN    (output)                   1 NC
//   0 RF_PTH    (output)                   0 WLAN_PWR_EN  (output, high)
//
// Detalhe que engana: EXT5V_EN e USB5V_EN NAO sao pins dirigidos. Ficam como
// input com pull interno e o enable de verdade e um resistor na placa; por
// isso o setExtOutput() do M5Unified inverte o PULL em vez de escrever nivel.
// E por isso os dois nascem ligados sem ninguem pedir.
// ---------------------------------------------------------------------------
static constexpr uint8_t IOE0_CAM_RST     = 6;
static constexpr uint8_t IOE1_WLAN_PWR_EN = 0;

// BMI270 no barramento interno. O M5Unified tenta 0x69 e 0x68.
static constexpr uint8_t BMI270_CHIP_ID   = 0x24;
static constexpr uint8_t BMI270_REG_ID    = 0x00;
static constexpr uint8_t BMI270_PWR_CONF  = 0x7C;
static constexpr uint8_t BMI270_PWR_CTRL  = 0x7D;

// ---------------------------------------------------------------------------

static bool s_imu_pedida = false;
bool tab5_power_imu_requested(void) { return s_imu_pedida; }

void tab5_power_config(m5::M5Unified::config_t& cfg, const tab5_profile_t& profile)
{
  cfg.internal_imu = profile.imu;
  s_imu_pedida     = profile.imu;
  cfg.internal_mic = profile.microphone;
  cfg.internal_spk = profile.speaker;
  cfg.internal_rtc = profile.rtc;

  // output_power=true faz o M5.begin() chamar setExtOutput(true) com mascara
  // 0xFF, que cobre ext_PA (Grove) E ext_USB (USB-A) de uma vez. Ligamos aqui
  // so se ALGUM dos dois for pedido, e separamos na fase 2.
  cfg.output_power = (profile.grove_5v || profile.usb_a_5v);
}

// Quando o Tab5 mexeu pela latest vez no 5V do Grove / reaplicou o profile
// (esp_timer em ms; 0 = nunca). Serve ao diario de reinicios do no Mesh.
static volatile uint32_t s_grove_toque_ms = 0, s_aplicar_ms = 0;
static int8_t s_grove_hw = -1;   // ultimo state pedido ao trilho (-1 = nunca)
// So conta como "mexeu no trilho" quando o state pedido MUDA: reaplicar o
// mesmo value aparece a parte, em tab5_power_apply_ms().
static void anotar_grove(bool turn_on)
{
  if (s_grove_hw == (int8_t)turn_on) { return; }
  s_grove_hw = (int8_t)turn_on;
  s_grove_toque_ms = tab5_now_ms() | 1u;
}
uint32_t tab5_power_grove_touch_ms(void) { return s_grove_toque_ms; }
uint32_t tab5_power_apply_ms(void)     { return s_aplicar_ms; }

void tab5_power_apply(const tab5_profile_t& profile)
{
  s_aplicar_ms = tab5_now_ms() | 1u;
  anotar_grove(profile.grove_5v);
  tab5_mark("profile de energia", profile.cpu_min_mhz);
  // --- trilhos de 5V, agora um a um ---------------------------------------
  // Se output_power ligou os dois, aqui desligamos o que nao foi pedido.
  M5.Power.setExtOutput(profile.grove_5v, m5::ext_PA);
  M5.Power.setExtOutput(profile.usb_a_5v, m5::ext_USB);

  // --- camera -------------------------------------------------------------
  tab5_power_camera(profile.camera);

  // --- radio (ESP32-C6) ---------------------------------------------------
  // Nao chamamos tab5_power_radio(true) quando ja esta on: o trilho ja
  // nasce high e um pulso desnecessario reiniciaria o C6.
  if (!profile.radio) { tab5_power_radio(false); }

  // --- backlight ----------------------------------------------------------
  M5.Display.setBrightness(profile.brightness);

  // --- gerenciamento de energia da CPU ------------------------------------
  if (profile.pm_dinamico)
  {
#if CONFIG_PM_ENABLE
    // O PISO NUNCA PODE PASSAR DO TETO - esp_pm_configure recusa com
    // ESP_ERR_INVALID_ARG se min > max, e SEM AVISO CLARO: e exatamente o bug
    // que 'energia sleep 20' revelou em ajustar_teto(), so que
    // aqui e o caminho que TODO app usa a cada abertura, nao um comando de
    // diagnostico. Hoje nenhum app declara cpu_max_mhz abaixo dos 40 do
    // cpu_min_mhz padrao, entao o caso nunca acontece de verdade - mas o
    // proximo app que quisesse um ceiling baixo cairia na mesma pegadinha sem
    // pista nenhuma do porque. Mesma correcao: o piso acompanha o ceiling quando
    // o ceiling pedido for menor.
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
      // ESP_ERR_INVALID_ARG aqui quase sempre e frequencia invalida:
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

  // So os nomes do que fica LIGADO: uma fileira de "=0/=1" obrigava a saber
  // a ordem dos campos para read.
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

static bool s_grove_ligado = false;

void tab5_power_grove(bool turn_on)
{
  anotar_grove(turn_on);
  M5.Power.setExtOutput(turn_on, m5::ext_PA);
  s_grove_ligado = turn_on;
  ESP_LOGI(TAG, "5V da porta Grove: %s", turn_on ? "on" : "desligado");
}

bool tab5_power_grove_on(void) { return s_grove_ligado; }

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
    // O giroscopio e who come a corrente (centenas de uA contra ~20uA do acel).
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

// Ultima measurement feita, guardada para who NAO pode ir ao I2C.
static tab5_measurement_t s_ultima = { 0, 0, 0, -1 };

tab5_measurement_t tab5_power_latest(void) { return s_ultima; }

// Porcentagem de carga, nossa - o M5Unified nao serve aqui.
//
// O getBatteryLevel() dele faz interpolacao LINEAR entre 3300 e 4150 mV por
// celula (Power_Class.cpp:2341). Dois problemas medidos:
//
// 1. A curva do Li-Po NAO e linear. Ela fica quase plana entre 4,0 e 3,7 V -
//    que sao uns 70% da capacity - e depois despenca. Com reta, os primeiros
//    30% escorrem devagar e os ultimos 10% caem em segundos.
// 2. Nao ha compensacao de carga. A tensao afunda sob consumo e sobe no
//    carregador, e o intervalo inteiro tem so 800mV.
//
// A tabela abaixo tem a FORMA de uma descarga de Li-Po, ancorada em 4150mV por
// celula = 100%, que e o pico real desta placa: ele mediu 8,24 a 8,30V com a
// bateria cheia, ou seja 4,12 a 4,15V por celula. O carregador termina abaixo
// dos 4,20V do padrao, e ancorar em 4,20 faria "cheio" marcar 95%.
//
// SUBSTITUIDA pela curva de VERDADE - a tabela generica acima
// (12 pontos, "forma de descarga de Li-Po" chutada a mao) foi jogada fora.
//
// O QUE MUDOU: achamos no energia.csv uma descarga CONTINUA, DE UMA SESSAO SO,
// de 100% (cauda de carga terminando, 8,309V) ate quase morrer (6,574V, o
// aparelho entrou em loop de reinicio por brownout logo depois) - 21,5 horas
// sem interrupcao, sem emenda de sessoes diferentes (o problema que estragou
// as duas tentativas anteriores) e ja com a tensao gravada em %.3f (o outro
// problema, corrigido em energy_log.cpp antes desta analise).
//
// O METODO: Coulomb counting de verdade. Integramos mA*dt (trapezio) desde o
// primeiro instante de descarga real ate o end, o que da a capacity
// REALMENTE consumida nesta descarga - 1306,8 mAh, e nao mais uma suposicao.
// Cada amostra dessa mesma descarga tem tensao E corrente medidas no MESMO
// instante, entao dá para casar "quanto realmente sobrava" (Coulomb) com "que
// tensao open o aparelho tinha" (V - I*R) point a point - sem chutar forma
// nenhuma, sem generalizar de outra bateria.
//
// O ERRO DA TABELA VELHA, medido contra este dado: ate +30 PONTOS no storage da
// faixa (dizia 50% com 19,9% real). Pior do que a analise tinha
// achado (+25 pontos) - aquela vinha de sessoes emendadas com quantizacao de
// tensao, entao ainda estava otimista demais sobre o proprio erro.
//
// SOBRE A ETIQUETA DIZER 2400 mAh: a bateria fisica traz "2400mAh" impresso,
// quase o dobro do 1306,8 medido aqui. Verificado ANTES de desconfiar da
// measurement: o shunt do INA226 (Power_Class.cpp, bloco dos expansores 0x43/0x44 -
// os mesmos enderecos do Tab5 documentados no topo deste file) esta em
// 0,005 ohm, o value certo - nao e o driver lendo corrente com constante
// trocada. Uma celula Li-Po tipica guarda uns 5-15% da carga entre 3,3V e
// 3,0V por celula (a faixa que o brownout nos impediu de measure aqui) - long e
// demais de sobra pra fechar 45% de diferenca. Ficamos com o numero MEDIDO:
// label de bateria generica inflada e mais comum do que capacity real
// escondida abaixo do point onde o proprio aparelho ja nao liga mais.
//
// R interna (0.268 ohm) e mantido - foi usado para compensar CADA point desta
// nova tabela, entao qualquer erro nele já está "dentro" da curva nova, e
// remedir R exigiria steps de corrente limpos que este dataset já não
// isola bem (a maioria dos steps aqui vem de app abrindo/fechando, não de
// um teste controlado).
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

  // Tensao em sleep, corrigida pela corrente. mA e assinado na nossa
  // convencao (+ carregando), e a formula unica cobre os dois casos.
  const float aberto = volts - (mA / 1000.0f) * TAB5_INTERNAL_R;
  const int mv = (int)(aberto * 500.0f);    // por celula (2S), em mV

  if (mv >= CURVA[0].mv) { return 100; }
  const int n = sizeof(CURVA) / sizeof(CURVA[0]);
  if (mv <= CURVA[n-1].mv) { return 0; }

  for (int i = 1; i < n; ++i) {
    if (mv >= CURVA[i].mv) {
      // Interpola dentro do trecho - a curva e quebrada em retas curtas, que e
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
// A curva acima e boa numa situacao so: descarga, longe de uma carga recente,
// na faixa de corrente em que foi calibrada. Reprocessando o energia.csv de
// (17 mil amostras, 48 trechos de descarga) contra a carga contada
// em mAh:
//   - descarga desde cheio: curva e contagem batem em +-3 pontos - OK;
//   - DEPOIS DE UMA CARGA PARCIAL (tirar da tomada no storage): a curva dizia
//     89,8% com 52,6% de verdade - +37 pontos - e levava 5,5 h para voltar a
//     dizer a verdade. A tensao sob carga (e por horas depois dela) sobe
//     muito alem do I*R: e polarizacao quimica, nao resistencia. A correcao de
//     carga antiga ainda PRENDIA o numero na curva,
//     entao durante a carga ele tambem mostrava 94,6%;
//   - com a tela apagada (~30 mA, fora da faixa calibrada) a curva le high;
//   - em 2,7% dos minutos de descarga a % SUBIA (ate +2,4 pontos) quando a
//     carga do aparelho mudava.
//
// O ESTIMADOR, o mesmo desenho de um medidor de bateria de celular:
//   1. CONTA A CARGA nos dois sentidos: soc += mA * dt / capacity;
//   2. A CURVA SO CORRIGE, devagar (constante de 3 h; 30 min abaixo de 15%,
//      onde ela e ingreme e confiavel), e so quando ela e confiavel:
//      descarga entre 45 e 160 mA, e pelo menos 1 h depois da latest carga;
//   3. FIM DE CARGA (corrente caindo com a tensao no ceiling) = 100%;
//   4. O NUMERO MOSTRADO NAO SOBE fora da carga.
// Contra o mesmo file: erro medio 1,8 -> 0,8 point, p95 9,9 -> 2,5,
// pior caso 43,7 -> 4,3, e nenhuma subida em descarga.
//
// O point de partida e a curva, no boot. Num reinicio por software (write,
// travamento, watchdog) a bateria nao mudou: o value sobrevive na RTC e o
// estimador continua de onde estava, em vez de recomecar pela tensao - que
// mente se o reinicio foi logo depois de uma carga.
static const float EST_RELAX_S      = 3600.0f;  // depois de carga, a tensao mente por horas
static const float EST_TAU_H        = 3.0f;
static const float EST_TAU_BAIXO_H  = 0.5f;
static const float EST_I_MIN_MA     = 45.0f;    // faixa de corrente da calibracao da curva
static const float EST_I_MAX_MA     = 160.0f;
static const uint32_t EST_MAGIA     = 0x50C0A7E1u;

// Corrente abaixo disto, com o carregador on E a tensao no ceiling, e o
// mesmo signal de "terminou" que o proprio carregador usa (C/20 a C/10).
static const float TAB5_CARGA_TERMINO_MA = 100.0f;

RTC_NOINIT_ATTR static struct { uint32_t magia; float soc; } s_rtc_soc;

static bool        s_est_ok      = false;
static float       s_soc         = 0.0f;     // o state, em % com fracao
static int64_t     s_est_us      = 0;
static int64_t     s_ult_carga_us = 0;
static bool        s_ja_carregou = false;
static int         s_exibido     = -1;
static const char* s_est_fonte   = "-";

static bool reinicio_de_software(void)
{
  switch (esp_reset_reason()) {
    case ESP_RST_SW: case ESP_RST_PANIC: case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT: case ESP_RST_WDT: return true;
    default: return false;
  }
}

// Chamada de DENTRO de tab5_power_measure() - uma vez por measurement.
static int estimar_pct(float volts, float mA, int pct_curva)
{
  const int64_t agora = esp_timer_get_time();
  if (pct_curva < 0) { return s_exibido; }   // leitura invalida: mantem

  if (!s_est_ok) {
    s_soc = (float)pct_curva;
    s_est_fonte = "curva no boot";
    // A RTC so vale se foi reinicio por software E se ainda e plausivel -
    // 20 pontos de folga cobrem o erro da curva logo depois de uma carga.
    if (reinicio_de_software() && s_rtc_soc.magia == EST_MAGIA
     && s_rtc_soc.soc >= 0.0f && s_rtc_soc.soc <= 100.0f
     && fabsf(s_rtc_soc.soc - (float)pct_curva) <= 20.0f) {
      s_soc = s_rtc_soc.soc;
      s_est_fonte = "continuado da RTC";
    }
    s_est_ok = true;
    s_est_us = agora;
    s_exibido = (int)lroundf(s_soc);
    s_rtc_soc.magia = EST_MAGIA; s_rtc_soc.soc = s_soc;
    return s_exibido;
  }

  // 1. CONTAGEM. dt do clock, nao da cadencia. Um buraco de mais de 6 min
  //    nao e continuidade: recomeca pela curva.
  const float dt_h = (float)(agora - s_est_us) / 3.6e9f;
  s_est_us = agora;
  if (dt_h > 0.0f && dt_h < 0.1f) { s_soc += mA * dt_h / TAB5_CAPACITY_MAH * 100.0f; }
  else if (dt_h >= 0.1f)          { s_soc = (float)pct_curva; s_est_fonte = "curva (lacuna)"; }

  if (mA > 20.0f) { s_ult_carga_us = agora; s_ja_carregou = true; }

  // 3. FIM DE CARGA: o par de sinais que o carregador usa para stop.
  if (mA > 0.0f && mA < TAB5_CARGA_TERMINO_MA && (int)(volts * 500.0f) >= (int)(CURVA[0].mv * 0.995f)) {
    s_soc = 100.0f;
  }

  // 2. CORRECAO PELA CURVA, lenta, so onde ela e confiavel.
  const float desde_carga_s = s_ja_carregou ? (float)(agora - s_ult_carga_us) / 1e6f : 1e9f;
  if (mA < -EST_I_MIN_MA && mA > -EST_I_MAX_MA && desde_carga_s > EST_RELAX_S && dt_h > 0.0f && dt_h < 0.1f) {
    const float tau = (pct_curva < 15) ? EST_TAU_BAIXO_H : EST_TAU_H;
    float k = dt_h / tau;
    if (k > 1.0f) { k = 1.0f; }
    s_soc += ((float)pct_curva - s_soc) * k;
  }

  if (s_soc < 0.0f)   { s_soc = 0.0f; }
  if (s_soc > 100.0f) { s_soc = 100.0f; }

  // 4. Fora da carga, o numero mostrado nao sobe.
  int r = (int)lroundf(s_soc);
  if (mA <= 20.0f && s_exibido >= 0 && r > s_exibido) { r = s_exibido; }
  s_exibido = r;

  s_rtc_soc.magia = EST_MAGIA; s_rtc_soc.soc = s_soc;
  return r;
}

void tab5_power_debug_soc(float* soc, float* curva, const char** fonte, float* min_desde_carga)
{
  if (soc)   { *soc = s_soc; }
  if (curva) { *curva = (float)tab5_power_pct(s_ultima.volts, s_ultima.mA); }
  if (fonte) { *fonte = s_est_fonte; }
  if (min_desde_carga) {
    *min_desde_carga = s_ja_carregou ? (float)(esp_timer_get_time() - s_ult_carga_us) / 6e7f : -1.0f;
  }
}

// Anel do history. Guardamos indice de escrita e contagem em vez de deslocar
// o vetor: deslocar 60 posicoes a cada point seria trabalho a toa, e o point
// chega a cada 30 segundos por anos a fio.
static tab5_point_t s_hist[TAB5_HIST_N];
static int      s_hist_n = 0, s_hist_i = 0;
static uint32_t s_hist_ultimo = 0;

int tab5_power_history(tab5_point_t* output)
{
  if (!output) { return 0; }
  // Desenrola o anel para ordem cronologica: who desenha quer o time andando
  // da esquerda para a direita, nao a ordem interna do buffer.
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
  // NAO usar M5.Power.getBatteryCurrent() aqui: ele devolve int32_t, e o LSB de
  // corrente do INA226 e 61uA (max_expected_current 2.0A / 32768). Qualquer
  // leitura abaixo de 1mA e truncada para 0 - foi o que fez a corrente aparecer
  // como "0.0 mA" fixo. Lendo o float direto mantemos a resolucao.
  // Sinal: o shunt esta on de mode que carga le negativo, entao invertemos
  // para a convencao (+) carregando / (-) descarregando.
  m.mA    = -1000.0f * M5.Power.Ina226.getShuntCurrent();
  // POTENCIA = TENSAO x CORRENTE, aqui mesmo. O registrador de potencia do
  // INA226 e exatamente essa conta feita pelo chip: le-lo era uma transacao
  // I2C a mais por segundo (awake e em sleep) para um numero que ja
  // temos. Sempre positiva, como o registrador.
  m.mW    = m.volts * fabsf(m.mA);
  // A % mostrada vem do ESTIMADOR (contagem de mAh corrigida devagar pela
  // curva) - ver estimar_pct(). A curva sozinha e tab5_power_pct().
  m.pct   = estimar_pct(m.volts, m.mA, tab5_power_pct(m.volts, m.mA));
  s_ultima = m;          // publica para as outras tarefas

  // Amostra o history a cada TAB5_HIST_SEG. A medicao acontece a 1Hz; guardar
  // tudo daria 30s de janela em vez de 30min.
  const uint32_t agora = (uint32_t)(esp_timer_get_time() / 1000000);
  float c;
  const bool tem_temp = tab5_sensors_temp(&c);

  // NAO COMECA O HISTORICO SEM TEMPERATURA - o grafico de temperatura saia
  // com buracos.
  //
  // A PRIMEIRA chamada desta funcao acontece no boot, via tab5_power_log
  // ("boot") em main.cpp - e nesse instante tab5_sensors_init() AINDA NAO
  // rodou: ele mora dentro da tarefa de interface (vTaskInterface, ver
  // kernel/core.cpp), que sistema_iniciar() so cria linhas DEPOIS, e main.cpp
  // chama o log de boot ANTES de sistema_iniciar(). tab5_sensors_temp()
  // devolvia false nesse momento, e a condicao "s_hist_ultimo==0" (a MESMA
  // que garante que a primeira measurement sempre entra no anel) fazia essa
  // amostra SEMPRE virar o slot MAIS ANTIGO do history, com temp travada
  // em -273 - e ficava ali por 30 minutos INTEIROS de uptime, toda vez que o
  // aparelho reiniciava. Como o desenvolvimento reinicia o aparelho o time
  // todo (flash.sh), o grafico de temperatura SEMPRE parecia faltar
  // informacao logo na borda mais antiga.
  //
  // A correcao: se ainda nao ha o PRIMEIRO history (s_hist_ultimo==0) e a
  // temperatura ainda nao esta disponivel, esta amostra so guarda a MEDIDA
  // ao vivo (s_ultima, ja feito acima) e NAO liga o clock do history -
  // a proxima chamada de tab5_power_measure() (1s depois, ainda no boot) tenta
  // de novo, e ja deve achar o sensor ready. Nao muda nada para who chama
  // depois do boot: se tem_temp for false so DAQUI a 30 minutos rodando (o
  // sensor falhando de verdade), o comportamento antigo (-273 no slot) volta
  // a valer, porque a essa height s_hist_ultimo!=0 e a condicao normal
  // assume.
  if (s_hist_ultimo != 0 || tem_temp) {
    if (s_hist_ultimo == 0 || (agora - s_hist_ultimo) >= TAB5_HIST_SEG) {
      s_hist_ultimo = agora;
      s_hist[s_hist_i].volts = m.volts;
      s_hist[s_hist_i].mA    = m.mA;
      s_hist[s_hist_i].pct   = (int16_t)m.pct;
      // -273 e o "nao medido": impossivel como temperatura, entao o grafico o
      // reconhece sem precisar de um campo de validade ao lado.
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
  // de frequencia - por isso ele serve para ver o PM trabalhando ao vivo.
  return esp_clk_cpu_freq() / 1000000;
}

void tab5_power_log(const char* label)
{
  tab5_measurement_t m = tab5_power_measure();
  // Compacto de proposito: com o prefixo do log ("I (1606) power: ") a version
  // anterior passava de 90 colunas e quebrava em duas linhas no terminal, o que
  // atrapalha justamente who esta comparando varias medidas seguidas.
  ESP_LOGI(TAG, "%-8s %dMHz %.2fV %.1fmA %.0fmW %d%%",
           label, tab5_power_cpu_mhz(), m.volts, m.mA, m.mW, m.pct);
}

void tab5_power_locks(const char* label)
{
#if CONFIG_PM_ENABLE
  printf("\n--- locks de PM (%s) ---\n", label);
  // stdout e nao ESP_LOG de proposito: o dump ja vem formatado em varias
  // linhas, e o prefixo de log em cada uma tornaria a tabela ilegivel.
  esp_pm_dump_locks(stdout);
  printf("--- end (%s) ---\n\n", label);
#else
  (void)label;
#endif
}

// (tab5_power_mostrar saiu: o panel de energia e desenhado pelo
// proprio app Energia. O HAL mede; who desenha sao os apps.)


// ---------------------------------------------------------------------------
// MODO DE ESPERA
// ---------------------------------------------------------------------------

// O acesso aos membros protegidos de Panel_DSI mora agora num point so,
// shared com tab5_screen.cpp - ver o aviso grande em
// tab5_internal_dsi_panel.hpp (consolidado; antes disto havia
// uma copia deste mesmo truque em cada file).
namespace {
  esp_lcd_panel_handle_t painel_dpi(void)
  {
    auto* p = tab5_internal_dsi_panel();
    return p ? p->get_handle() : nullptr;
  }

  bool s_em_repouso = false;
  bool s_desmontado = false; // panel e barramento DSI destruidos (ver desmontar_tela)
  bool s_so_luz = false;     // sleep de tela TDDI: so a luz apagou (ver tab5_panel)
  int  s_cpu_normal = TAB5_CPU_MAX_MHZ;   // ceiling a restaurar, guardado ao entrar em sleep
}

bool tab5_power_is_sleeping(void) { return s_em_repouso; }

// ---- qual tela (ver tab5_power.hpp) -----------------------------------------
// O address do toque separa as familias sem ambiguidade: o GT911 responde em
// 0x14 (ou 0x5D) e o toque dos TDDI da Sitronix em 0x55. Entre os dois ST, o
// M5GFX nao guarda o modelo em lugar publico - mas programa cada um com o seu
// clock de pixel (ST7121 a 70 MHz, ST7123 a 80), value_read aqui antes de alguem
// mexer nele. Um ST que nao bate com nenhum dos dois continua sendo tratado
// como TDDI: o name sai "desconhecido", o comportamento e o conservador.
static int8_t s_painel = -1;   // -1 = ainda nao detectado

tab5_panel_t tab5_panel(void)
{
  if (s_painel >= 0) { return (tab5_panel_t)s_painel; }
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
  s_painel = (int8_t)r;
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

// O sleep fundo (SLPIN + unmount o DSI) so onde o toque sobrevive a ele.
static bool repouso_fundo(void) { return tab5_panel_touch_gt911(); }

// Ajusta so o ceiling de clock. O piso fica em 40MHz (o "XTAL" de que fala o
// comentario antigo) - MAS SO QUANDO O TETO PEDIDO E MAIOR OU IGUAL A ISSO.
//
// BUG ENCONTRADO, pelo comando 'energia sleep' testando pela
// primeira vez um ceiling abaixo de 40: esp_pm_configure EXIGE min <= max, e o
// piso vinha fixo em 40 nesta funcao - entao "energia sleep 20" pedia
// max=20/min=40 e o proprio IDF recusava com ESP_ERR_INVALID_ARG, sempre,
// para qualquer ceiling abaixo de 40. Ninguem tinha pego isto porque nenhum
// chamador de verdade (tab5_power_sleep()/resume(), sempre >=40 ate hoje)
// jamais pediu um ceiling tao baixo - o comando de console foi o primeiro.
//
// Quando o ceiling pedido fica ABAIXO de 40, o piso acompanha (min = max): nao
// ha mais faixa de DFS para variar, o clock fica fixo naquele value - que e
// exatamente a intencao de who esta testando "meu piso E este numero".
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
// mesmo caminho que sleep()/resume() ja usam - MAS sem tocar em panel,
// backlight ou fluxo DSI nenhum: a tela continua exatamente como estava,
// ligada e desenhando. E por isso que da para testar "panel devagar + CPU
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
// Mexe no PAINEL direto, e nao em M5.Display.setBrightness(), de proposito: o
// LGFX guarda o brightness "oficial" num membro _brightness, e e ele que o
// wakeup() restaura. Se o fade usasse setBrightness(), o value salvo viraria 0
// e a tela nunca mais acenderia sozinha ao acordar.
static void (*s_gancho_rampa)(void) = nullptr;
void tab5_power_ramp_hook(void (*step)(void)) { s_gancho_rampa = step; }

static void esmaecer(uint8_t de, uint8_t para, int passos = 16, int ms = 12)
{
  auto* p = M5.Display.panel();
  if (!p) { return; }
  for (int i = 1; i <= passos; ++i) {
    const int b = (int)de + ((int)para - (int)de) * i / passos;
    p->setBrightness((uint8_t)b);
    if (s_gancho_rampa) { s_gancho_rampa(); }
    vTaskDelay(pdMS_TO_TICKS(ms));
  }
  p->setBrightness(para);
}

// DOIS CAMINHOS QUE NAO FUNCIONARAM - o registro, para ninguem repetir.
//
// 1. Parar o fluxo DSI com esp_lcd_dpi_panel_set_pattern() no sleep (e
//    MIPI_DSI_PATTERN_NONE no resume). Medido: depois do NONE a ponte DSI as
//    vezes nao volta a alimentar o panel - tela acesa, palida, sem conteudo,
//    por ate 15 minutos, com o log do despertar inteiro e normal. O
//    set_pattern foi feito para exibir padrao de teste, nao para pausar.
//
// 2. Baixar o clock com o fluxo on. Da underrun em todo frame, e o driver
//    loga com ESP_DRAM_LOGE (esp_rom_printf, ignora esp_log_level_set): ~6,5 ms
//    de UART bloqueante dentro de interrupcao a cada frame. E o proprio driver
//    avisa que o underrun pode deixar a tela azul sem volta.
//
// O que funciona e o que esta abaixo: unmount o panel e o barramento.

// Desfaz um sleep que failed no storage, SEM wake de supetao. Usar
// M5.Display.wakeup() aqui era um erro: ele restaura o brightness na hora.
static void desfazer_repouso(void)
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
// O caminho que as duas chaves acima nao conseguiram abrir. Medido em
// com esp_pm_dump_locks(): o panel segura DUAS locks de PM, e
// nenhuma delas se solta com set_pattern() nem com sleep():
//
//     dsi_dpi   CPU_FREQ_MAX     <- prende a CPU em 360MHz
//     dsi_phy   NO_LIGHT_SLEEP   <- proibe light sleep
//
// No fonte do IDF as duas sao adquiridas na CRIACAO e liberadas so no
// DESTRUTOR (esp_lcd_panel_dpi.c 291 -> 405; esp_lcd_mipi_dsi_bus.c 72 -> 151).
// Logo: para solta-las, destruir. Nao ha storage-termo.
//
// Destruir resolve os tres problemas de uma vez, e por isso vale o risco:
//   - sem fluxo DPI nao ha underrun, entao o clock pode cair de verdade;
//   - dsi_dpi solta, o ceiling de clock passa a valer;
//   - dsi_phy solta, light sleep deixa de ser impossivel.
//
// INVARIANTE QUE NAO PODE SER QUEBRADA: entre unmount e remontar, NINGUEM
// pode desenhar. O framebuffer pertence ao panel DPI
// (esp_lcd_dpi_panel_get_frame_buffer em Panel_DSI.cpp:143) e e liberado junto
// com ele - desenhar ali e escrever em memoria devolvida ao heap. Neste sistema
// isso se sustenta porque so a tarefa de interface desenha, e e ela who chama
// sleep()/resume(); o loop dela em sleep nao toca no display.

static bool desmontar_tela(void)
{
  auto* p = tab5_internal_dsi_panel();
  if (p == nullptr) { return false; }
  auto* bus = p->getBusDSI();
  if (bus == nullptr) { return false; }

  // o double buffer morre PRIMEIRO: o vetor de linhas dela aponta para dentro
  // do framebuffer que o esp_lcd_panel_del() esta prestes a release.
  tab5_screen_shutdown();

  // Ordem: panel antes do barramento. O panel DPI foi criado A PARTIR do
  // barramento; erase o barramento primeiro deixaria o panel com referencia
  // para algo que nao existe mais.
  esp_lcd_panel_handle_t dpi = p->get_handle();
  if (dpi != nullptr)
  {
    esp_err_t err = esp_lcd_panel_del(dpi);   // solta dsi_dpi, libera o framebuffer
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "unmount: esp_lcd_panel_del failed: %s", esp_err_to_name(err));
      return false;
    }
    p->clear_handle();
  }

  // O lineArray e alocado pelo Panel_DSI::init com heap_alloc_dma e nunca
  // liberado por ninguem. Alem do vazamento, e ELE o guarda que faz o init
  // return_card false na segunda chamada ("if (_lines_buffer != nullptr) return
  // false"). Sem zerar isto, remontar e impossivel.
  uint8_t** lines = p->get_lines();
  if (lines != nullptr) { lgfx::heap_free(lines); p->clear_lines(); }

  // O buffer ja foi liberado junto com o panel; deixar o ponteiro velho seria
  // um pendurado esperando o primeiro desenho distraido.
  p->clear_buffer();

  // release() do M5GFX apaga o IO DBI, apaga o barramento DSI (soltando
  // dsi_phy) e devolve o channel do LDO que alimenta a PHY. Teardown complete,
  // e feito pela propria biblioteca - nao precisamos imitar nada.
  bus->release();
  return true;
}

static bool montar_tela(void)
{
  auto* p = tab5_internal_dsi_panel();
  if (p == nullptr) { return false; }
  auto* bus = p->getBusDSI();
  if (bus == nullptr) { return false; }

  // ATENCAO, QUEM CHAMAR ISTO: A LUZ VOLTA APAGADA.
  //
  // O Panel_Device::init() la dentro faz _light->init(0) (Panel_Device.cpp:67).
  // Ou seja, mount o panel zera o PWM do backlight - e o _brightness do LGFX
  // NAO acompanha, porque ele mora no LGFX_Device e nao no panel
  // (LGFXBase.hpp:1519). Resultado: M5.Display.getBrightness() continua
  // respondendo 80 com a lampada em 0. A tela fica ligada, desenhando certo, e
  // invisivel.
  //
  // No caminho do sleep isso e DE PROPOSITO e nao pode mudar: o resume()
  // devolve o panel no escuro justamente para who chamou repintar antes de
  // wake, e so entao chamar tab5_power_wake_screen(). Quem usar montar_tela()
  // por outro motivo tem que wake por conta propria - foi exatamente o que
  // faltou na primeira version da troca de clock, e a tela nascia preta ate o
  // primeiro sleep.
  //
  // Cala o LEDC durante a remontagem, e SO durante ela.
  //
  // Panel_Device::init() faz _light->init(0) (Panel_Device.cpp:67), ou seja
  // reconfigura o PWM do backlight. O GPIO 22 ja esta reservado desde o boot,
  // entao o driver avisa "GPIO 22 is not usable, maybe conflict with others" a
  // CADA vez que a tela acorda. E benigno - o brightness continua funcionando, o
  // wake() logo em seguida prova - mas repetido a cada toque vira ruido que
  // esconde aviso de verdade. Restaurado no end: fora desta janela um aviso do
  // LEDC volta a significar alguma coisa.
  esp_log_level_set("ledc", ESP_LOG_ERROR);

  // Redundante de proposito: o Panel_Device::init() la dentro tambem chama
  // _bus->init() (linha 74), e o Bus_DSI::init() tem guarda para nao criar duas
  // vezes. Chamamos aqui para que uma failure de barramento seja reportada como
  // failure de BARRAMENTO, e nao como um "Panel_DSI::init failed" generico.
  if (!bus->init())
  {
    ESP_LOGE(TAG, "mount: Bus_DSI::init failed");
    esp_log_level_set("ledc", ESP_LOG_WARN);
    return false;
  }
  // use_reset=true: manda SWRESET e reexecuta a lista de inicializacao do
  // panel. Como o barramento acabou de nascer, o panel tem que ser
  // reprogramado do zero - nao da para presumir state nenhum.
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
  if (s_em_repouso) { return true; }

  if (painel_dpi() == nullptr) {
    ESP_LOGE(TAG, "sleep: nao achei o handle do panel DPI");
    return false;
  }

  // 1. Escurece SUAVE.
  tab5_mark("sleep: apagando", 0);
  esmaecer(M5.Display.getBrightness(), 0);

  // TELA TDDI (ST7123/ST7121): para aqui, so com a luz apagada. Painel
  // awake e fluxo DSI de pe - o toque, que mora no chip da tela, continua
  // scanning e acorda a tela como sempre. Custa o clock high do panel on;
  // e o preco de nao arriscar um aparelho que nao acorda mais. Ver tab5_panel().
  if (!repouso_fundo()) {
    s_so_luz = true;
    s_em_repouso = true;
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

  // 3. Desmonta panel e barramento. E o que solta as locks de PM - ver o
  //    bloco grande acima. Depois daqui NAO EXISTE FRAMEBUFFER: ninguem pode
  //    desenhar ate o montar_tela() do resume().
  const uint32_t t0 = tab5_now_ms();
  if (!desmontar_tela()) {
    ESP_LOGE(TAG, "sleep: unmount failed - voltando ao state aceso");
    desfazer_repouso();
    return false;
  }
  s_desmontado = true;
  ESP_LOGI(TAG, "sleep: DSI desmontado em %lu ms", (unsigned long)(tab5_now_ms() - t0));

  // 4. Baixa o clock - sem panel nao ha underrun, entao pode.
  if (!ajustar_teto(cpu_mhz)) {
    ESP_LOGE(TAG, "sleep: nao consegui baixar o clock para %d MHz", cpu_mhz);
    montar_tela();
    s_desmontado = false;
    desfazer_repouso();
    return false;
  }

  s_em_repouso = true;
  ESP_LOGI(TAG, "sleep: DSI desmontado, clock %d -> %d MHz", s_cpu_normal, tab5_power_cpu_mhz());
  // A PROVA. Se a desmontagem fez o que promete, dsi_dpi e dsi_phy somem da
  // lista de locks - e ai o clock cair de verdade deixa de ser esperanca.
  // UMA VEZ POR BOOT: 'energia locks' mostra quando alguem pede.
  static bool s_provou = false;
  if (!s_provou) { s_provou = true; tab5_power_locks("primeiro sleep"); }
  return true;
}

bool tab5_power_resume(void)
{
  if (!s_em_repouso) { return true; }

  // Repouso de tela TDDI: nada foi desligado alem da luz, e a luz who acende
  // e o tab5_power_wake_screen(), depois do redesenho.
  if (s_so_luz) {
    s_so_luz = false;
    s_em_repouso = false;
    ESP_LOGI(TAG, "resume: panel ja estava no ar (sleep so de luz)");
    return true;
  }

  // 1. Sobe o clock ANTES de qualquer coisa visivel, e CONFIRMA: o
  //    montar_tela() reprograma o panel e cria o fluxo DPI, e fazer isso a
  //    90MHz seria comecar ja sem banda de PSRAM.
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
  bool ok = montar_tela();
  if (!ok) {
    // Uma segunda chance: a primeira failure pode ser um LDO ou uma PHY que
    // precisou de mais time para assentar depois do release.
    ESP_LOGE(TAG, "resume: remontagem failed - tentando mais uma vez");
    vTaskDelay(pdMS_TO_TICKS(100));
    ok = montar_tela();
  }
  if (!ok) {
    // Sem panel o aparelho e um tijolo escuro e nao existe caminho de code
    // que o traga de volta. Reiniciar e a menos ruim: os ajustes estao na NVS.
    ESP_LOGE(TAG, "resume: remontagem failed DUAS vezes - reiniciando");
    vTaskDelay(pdMS_TO_TICKS(200));   // deixa a UART esvaziar antes do reset
    esp_restart();
  }
  s_desmontado = false;

  // A PRIORIDADE NO AXI, REAFIRMADA. Os registradores do ICM nao pertencem ao
  // panel e nao deveriam ser tocados pela destruicao dele - mas "nao
  // deveriam" nao e garantia escrita em lugar nenhum, e o cost sao quatro
  // escritas em registrador contra acordar no padrao de fabrica, que e
  // exatamente a condicao da piscada azul.
  tab5_bus_prioritize_screen(tab5_bus_is_prioritized());

  s_em_repouso = false;
  ESP_LOGI(TAG, "resume: DSI remontado em %lu ms, clock em %d MHz",
           (unsigned long)(tab5_now_ms() - t0), mhz);
  return true;
}

// ---------------------------------------------------------------------------
// RELOGIO DE PIXEL DO PAINEL - a alavanca permanente contra a piscada azul
// ---------------------------------------------------------------------------
// A CONTA, que e o argumento inteiro.
//
// O panel e 720x1280 e o M5GFX o programa com portico de 140+40+40 na
// horizontal e 20+4+20 na vertical (M5GFX.cpp:3214, ramo do ILI9881C). Entao
// cada frame custa, em ciclos de pixel:
//
//   width total = 720 + 220 = 940
//   height total  = 1280 +  44 = 1324
//   ciclos/quadro = 940 * 1324 = 1.244.560
//
// A 80MHz isso da 64,3 frames por segundo. Cada frame le 720*1280*2 bytes =
// 1,84MB da PSRAM. Ou seja: o panel puxa 118,5 MB/s da PSRAM O TEMPO TODO, e
// nao ha nada que o code faca que reduza isso - e o hardware scanning.
//
// A 60MHz sao 48,2 frames por segundo e 88,9 MB/s: VINTE E CINCO POR CENTO a
// menos, para sempre, em toda transicao, em todo app, sem ninguem precisar
// lembrar de nada. E a folga por linha cresce junto: a linha ativa passa de
// 9,0us para 12,0us e o intervalo de recarga do FIFO de 2,75us para 3,67us.
//
// O que se perde: 64Hz viram 48Hz. Nesta interface - listas, botoes, um jogo de
// cobrinha em grade - nao ha nada que precise de 64 frames por segundo.
//
// POR QUE 60 E NAO OUTRO NUMERO: e o padrao do proprio Panel_DSI do M5GFX
// (Panel_DSI.hpp:47, dpi_freq_mhz = 60). O 80 vem de uma linha especifica do
// M5GFX para este panel; os porticos sao contados em PIXELS e escalam junto
// com o clock, entao a geometria continua valid.
//
// HONESTIDADE: nao consegui measure isto em bancada. Por isso existem duas networks
// de seguranca: se a remontagem falhar, voltamos sozinhos para o value antigo;
// e o console muda o value em execucao ('tela clock 80'), sem regravar.
static uint16_t s_dpi_mhz_pedido = 0;   // 0 = nunca mexemos

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
  // Fora da faixa, LIMITA em vez de recusar: who pede 84 quer
  // "o maximo", nao um erro. Mesma faixa de SETTINGS_PANEL_CLOCK_MIN_MHZ/MAX.
  if (mhz > 80) { mhz = 80; }
  if (mhz < 24) { mhz = 24; }

  // Tela TDDI: o clock fica no de fabrica (ver tab5_panel). Pedir o mesmo
  // value que ja esta e sucesso; qualquer outro, recusa sem mexer em nada.
  if (!tab5_panel_touch_gt911()) {
    if (mhz == tab5_power_dpi_clock()) { return true; }
    ESP_LOGW(TAG, "clock do panel: fixo em %d MHz na tela %s",
             tab5_power_dpi_clock(), tab5_panel_name());
    return false;
  }

  // ACHADO: chamar isto com a tela EM REPOUSO e perigoso e nao
  // tinha guarda nenhuma. Em sleep fundo o panel ja esta
  // desmontado - _disp_panel_handle e _lines_buffer zerados -, mas o OBJETO
  // Panel_DSI continua vivo (so os ponteiros internos morrem), entao
  // tab5_internal_dsi_panel() abaixo devolve nao-nulo e o code seguia em frente. O
  // trocar() chamaria desmontar_tela() e bus->release() UMA SEGUNDA VEZ sobre
  // um barramento que ja foi liberado - o Bus_DSI do M5GFX nao promete ser
  // idempotente a um release() repetido, e essa e a receita mais provavel do
  // travamento observado ao testar 'tela clock'/'energia ceiling' com a tela
  // dormindo. A output: recusar aqui, tela por tela, e return_card o controle para
  // who chamou - a mesma politica de kernel/core.cpp para so acordar remexer no
  // panel.
  if (s_em_repouso) {
    ESP_LOGE(TAG, "clock do panel: a tela esta em sleep (panel desmontado) - "
                  "acorde primeiro e tente de novo");
    return false;
  }

  auto* p = tab5_internal_dsi_panel();
  if (p == nullptr) { return false; }

  const uint16_t antigo = p->config_detail().dpi_freq_mhz;
  if (antigo == (uint16_t)mhz) { return true; }

  // O caminho e o MESMO do sleep: unmount e mount. Ele ja roda a cada
  // despertar, entao nao e um caminho novo - e o mais exercitado do modulo.
  // Reaproveitar em vez de inventar outro e o que mantem uma unica maneira de
  // recriar o panel, e portanto um unico lugar para consertar quando quebrar.
  // A LUZ VOLTA JUNTO, e mora DENTRO do lambda de proposito.
  //
  // Ha dois caminhos que remontam aqui: o clock novo e a volta ao antigo
  // quando ele nao sobe. Acendendo so no caminho de sucesso, um panel que
  // recusasse 60MHz voltaria para 80 funcionando e INVISIVEL - o pior desfecho
  // possivel, porque o log diria "voltei ao anterior" e a tela continuaria
  // preta. Toda remontagem bem-sucedida acende, sem excecao.
  //
  // Sem risco de mostrar lixo: o esp_lcd_new_panel_dpi aloca os framebuffers
  // com heap_caps_calloc (esp_lcd_panel_dpi.c:238), entao o panel novo nasce
  // scanning PRETO.
  auto trocar = [&](uint16_t v) -> bool {
    auto det = p->config_detail();
    det.dpi_freq_mhz = v;
    p->config_detail(det);
    if (!desmontar_tela()) { return false; }
    if (!montar_tela())    { return false; }
    // Reescrever o mesmo numero e o que empurra o brightness para o hardware de
    // novo - ver o aviso no high do montar_tela().
    M5.Display.setBrightness(M5.Display.getBrightness());
    return true;
  };

  ESP_LOGI(TAG, "clock do panel: %u -> %d MHz", (unsigned)antigo, mhz);
  if (!trocar((uint16_t)mhz))
  {
    // VOLTA SOZINHO. Um panel que nao sobe deixaria o aparelho cego, e o
    // usuario sem storage de descobrir por que - o value novo e justamente o que
    // nao consigo testar aqui.
    ESP_LOGE(TAG, "o panel nao subiu a %d MHz - voltando para %u", mhz, (unsigned)antigo);
    if (!trocar(antigo)) {
      ESP_LOGE(TAG, "nem no value antigo o panel voltou - reiniciando");
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
    }
    return false;
  }

  s_dpi_mhz_pedido = (uint16_t)mhz;

  // As paginas morreram junto com o panel velho (o desmontar_tela chama
  // tab5_screen_shutdown). O compositor percebe pela generation e manda a tela
  // inteira para o panel novo - nao ha nada a fazer aqui.
  //
  // A PRIORIDADE NO AXI, sim, precisa ser reafirmada. Os registradores do ICM
  // sobrevivem a destruicao do panel, mas depender disso e depender de um
  // detalhe que nunca foi prometido. Custa quatro escritas.
  tab5_bus_prioritize_screen(tab5_bus_is_prioritized());
  return true;
}

void tab5_power_wake_screen(void)
{
  uint8_t alvo = M5.Display.getBrightness();

  // REDE DE SEGURANCA: se o brightness guardado pelo LGFX for zero, wake com ele
  // deixaria a tela ligada e escura para sempre - panel mostrando conteudo sem
  // luz nenhuma, que e o "esbranquicado apagado". Nunca aceitar zero aqui.
  if (alvo == 0) {
    ESP_LOGW(TAG, "wake: brightness guardado era 0 - usando 80 como piso");
    alvo = 80;
    M5.Display.setBrightness(alvo);   // corrige tambem o value guardado
  }

  // PARTIDA A FRIO. Com o backlight vindo de zero, um duty muito baixo nao
  // acende: o driver de LED precisa de um pulso minimo para sair do sleep,
  // e depois que ja esta aceso aguenta duty bem menor (por isso a barra de
  // Ajustes mostrava luz no minimo e o despertar voltava preto). Quando o alvo
  // e baixo, sobe primeiro a um nivel seguro, segura um instante e so entao
  // desce ao alvo - o olho ve um brightness que se acomoda, nao uma tela morta.
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

// tab5_power_diagnostico() (le registradores crus do INA226) foi removida em
// nasceu para a investigacao da calibracao de bateria, que
// fechou (ver o registro em memoria "tab5-bateria-
// calibracao"), e ninguem chamava mais (nem console, nem app nenhum).
