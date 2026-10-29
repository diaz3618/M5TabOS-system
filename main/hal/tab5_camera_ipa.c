// WHITE PROTECTION ON THE ISP PIPELINE (one IPA itself, "tab5_ipa_cor").
//
// THE DEFECT. White burst exit magenta, in screen on live and in photo.
//
// THE CAUSE, in silicon. The ISP of P4 below v3.0 does not have the block of gain of
// white balance (WBG). esp_video compensates by multiplying columns R and B
// of matrix of color (CCM) by gains of AWB (esp_video_isp_device.c,
// isp_init_ccm_param). Mathematically, it's the same - until the sensor saturates. Num
// pixel burst the sensor delivers (255,255,255) raw; the gain, which should
// taking R and B beyond 255 and being CUT there (and what the WBG of v3 does), enters
// straight into CCM without any cutting. The line of green of CCM has coefficients
// negative for R and B (typical: -0.40 R + 1.63 G - 0.23 B), then:
//
// G_output = -0.40 * 255 * gR + 1.63 * 255 - 0.23 * 255 * gB
//
// with gR = 2.0 and gB = 1.7 (daylight) of 0.43 * 255. R and B are cut into
// 255: white becomes (255, 110, 255) linear, (255, 154, 255) after gamma.
// In warm light (gR 1.6, gB 2.0) stays worse: (255, 117, 255). It's not memory,
// is not exposition: it is algebra.
//
// THE FIX. By calling s = (gR, 1, gB), the saturated white exit of line of
// green as m . s. This line just needs to do two things:
// 1. m . (1,1,1) equal to of calibration - gray remains gray;
// 2. m . s >= G_MIN - saturated white remains (almost) white.
// There are 2 restrictions on 3 unknowns: there remains one degree of freedom, used to
// stay as close as possible to the original line, measuring the error in the 24
// stains of a color chart (Macbeth) taken to the domain of sensor.
// This has a closed solution (least squares with restriction of equality):
//
// m' = m - Ci A^T (A Ci A^T)^-1 (A m - b), Ci = M^T K M
//
// with K = (T T^T)^-1 constant (T = the 24 patches in linear sRGB). R lines and
// B remain as they are: the saturated white is already above 1 and is cut off
// at 255, which is correct.
//
// THE COST. The line of green is the one that most separates greens and magentas. Move it
// takes a little bit of saturation out of these colors (simulated: average dE ~5 in letter).
// A boost of 10% in saturation of ISP (YUV domain, where white has chroma
// zero and is not affected) returns part of this. In v3 (WBG of truth) nothing
// this is necessary and the IPA does not even enter the pipeline.
//
// HOW TO ENTER THE PIPELINE WITHOUT EDITTING THE COMPONENT. esp_video gets the list of
// algorithms of esp_ipa_pipeline_get_config(); the link diverts this call
// to ca (-Wl,--wrap, main/CMakeLists.txt) and return the same
// configuration with "tab5_ipa_cor" at the END of list. The pipeline finds the
// algorithm by name in section .esp_ipa_detect (ESP_IPA_DETECT_FN) and runs it
// every frame, AFTER AWB and ACC: it receives the CCM and the gains already
// decided and only adjusts the matrix before it goes to the ISP.
//
// C file, not C++: ESP_IPA_DETECT_FN uses designated initializers
// out of order of the fields, which C++ refuses.

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_ipa.h"
#include "esp_ipa_detect.h"
#include "esp_video_caps.h"
#include "tab5_camera_ipa.h"

static const char *TAG = "tab5_ipa_cor";

// Green minimum of one saturated white (1 = pure white). 0.92 exit, after of
// range, such as ~243 of 255: imperceptible on side of R and B at 255.
#define G_MIN        0.92f
// Boost of saturation when the matrix was protected (percentage).
#define SAT_REFORCO  110
#define SAT_PADRAO   128   // 1.0 in format of ISP

// (T T^T)^-1 of 24 letter patches in linear sRGB (D65).
static const float K[3][3] = {
  {  0.80074f, -0.90448f,  0.09025f },
  { -0.90448f,  2.03918f, -0.97943f },
  {  0.09025f, -0.97943f,  1.19777f },
};

typedef struct {
  bool     tem_ccm, tem_sat;
  float    ccm_awb[3][3];      // the latest that ACC sent (without protection)
  uint32_t sat_awb;
  float    ccm_enviada[3][3];  // the latest that went to the ISP
  uint32_t sat_enviada;
} estado_t;

static estado_t s_e;

static portMUX_TYPE          s_trava = portMUX_INITIALIZER_UNLOCKED;
static tab5_cam_ipa_state_t s_foto;
static bool                  s_tem_foto;

static float ganho_valido(float g) { return (g > 0.1f && g < 8.0f) ? g : 1.0f; }

// A line of green protected (see header). Returns the green of white
// saturated before protection; 'output' receives the entire array.
static float proteger(const float M[3][3], float gR, float gB, float output[3][3])
{
  memcpy(output, M, sizeof(float) * 9);
  const float s[3] = { gR, 1.0f, gB };
  const float *m = M[1];
  const float g_antes = m[0] * s[0] + m[1] * s[1] + m[2] * s[2];
  if (g_antes >= G_MIN) { return g_antes; }

  // Ci = M^T K M (symmetric)
  float KM[3][3], Ci[3][3];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      KM[i][j] = K[i][0] * M[0][j] + K[i][1] * M[1][j] + K[i][2] * M[2][j];
    }
  }
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      Ci[i][j] = M[0][i] * KM[0][j] + M[1][i] * KM[1][j] + M[2][i] * KM[2][j];
    }
  }

  // A = [ (1,1,1) ; s ].  c0 = Ci a0, c1 = Ci a1.
  float c0[3], c1[3];
  for (int i = 0; i < 3; ++i) {
    c0[i] = Ci[i][0] + Ci[i][1] + Ci[i][2];
    c1[i] = Ci[i][0] * s[0] + Ci[i][1] * s[1] + Ci[i][2] * s[2];
  }
  const float p00 = c0[0] + c0[1] + c0[2];
  const float p01 = c1[0] + c1[1] + c1[2];
  const float p10 = s[0] * c0[0] + s[1] * c0[1] + s[2] * c0[2];
  const float p11 = s[0] * c1[0] + s[1] * c1[1] + s[2] * c1[2];
  const float det = p00 * p11 - p01 * p10;

  // r = A m - b: the sum of line does not change (r0 = 0), the blank goes up to G_MIN.
  const float r1 = g_antes - G_MIN;
  if (fabsf(det) < 1e-9f) {
    const float soma = m[0] + m[1] + m[2];
    output[1][0] = 0.0f; output[1][1] = soma; output[1][2] = 0.0f;
    return g_antes;
  }
  const float l0 = (-p01 * r1) / det;
  const float l1 = ( p00 * r1) / det;
  for (int i = 0; i < 3; ++i) { output[1][i] = m[i] - (c0[i] * l0 + c1[i] * l1); }
  return g_antes;
}

static void ajustar(esp_ipa_metadata_t *md, const esp_ipa_stats_t *stats, const esp_ipa_sensor_t *sensor)
{
  if (md->flags & IPA_METADATA_FLAGS_CCM) {
    memcpy(s_e.ccm_awb, md->ccm.matrix, sizeof(s_e.ccm_awb));
    s_e.tem_ccm = true;
  }
  if (md->flags & IPA_METADATA_FLAGS_ST) {
    s_e.sat_awb = md->saturation;
    s_e.tem_sat = true;
  }
  if (!s_e.tem_ccm) { return; }   // ISP still in identity matrix: white is already white

  const float gR = ganho_valido(md->red_gain);
  const float gB = ganho_valido(md->blue_gain);
  float nova[3][3];
  const float g_antes = proteger(s_e.ccm_awb, gR, gB, nova);
  const bool  protegida = g_antes < G_MIN;

  // Send the matrix when ACC sent one (his is in md->ccm and needs
  // be changed) or when the result changed (new gains of AWB).
  if ((md->flags & IPA_METADATA_FLAGS_CCM) || memcmp(nova, s_e.ccm_enviada, sizeof(nova)) != 0) {
    memcpy(md->ccm.matrix, nova, sizeof(nova));
    md->flags |= IPA_METADATA_FLAGS_CCM;
    memcpy(s_e.ccm_enviada, nova, sizeof(nova));
  }

  uint32_t sat = s_e.tem_sat ? s_e.sat_awb : SAT_PADRAO;
  if (protegida) { sat = sat * SAT_REFORCO / 100; }
  if (sat > 255) { sat = 255; }
  if ((md->flags & IPA_METADATA_FLAGS_ST) || sat != s_e.sat_enviada) {
    md->saturation = sat;
    md->flags |= IPA_METADATA_FLAGS_ST;
    s_e.sat_enviada = sat;
  }

  // Photo for diagnostics. Short copy (~200 B) under spin lock: who le
  // and the console, in another task.
  taskENTER_CRITICAL(&s_trava);
  s_foto.frames++;
  s_foto.red_gain = gR;
  s_foto.blue_gain = gB;
  memcpy(s_foto.ccm_awb, s_e.ccm_awb, sizeof(s_foto.ccm_awb));
  memcpy(s_foto.ccm_isp, nova, sizeof(s_foto.ccm_isp));
  s_foto.white_green_before  = g_antes;
  s_foto.white_green_after = nova[1][0] * gR + nova[1][1] + nova[1][2] * gB;
  s_foto.saturation = sat;
  if (sensor) {
    s_foto.exposure_us = sensor->cur_exposure;
    s_foto.sensor_gain = sensor->cur_gain;
  }
  if (stats) {
    for (int i = 0; i < 16 && i < ISP_HIST_SEGMENT_NUMS; ++i) { s_foto.hist[i] = stats->hist_stats[i].value; }
  }
  s_tem_foto = true;
  taskEXIT_CRITICAL(&s_trava);
}

bool tab5_camera_ipa_state(tab5_cam_ipa_state_t *e)
{
  taskENTER_CRITICAL(&s_trava);
  const bool tem = s_tem_foto;
  if (tem) { *e = s_foto; }
  taskEXIT_CRITICAL(&s_trava);
  return tem;
}

static esp_err_t ipa_start(esp_ipa_t *ipa, const esp_ipa_sensor_t *sensor, esp_ipa_metadata_t *md)
{
  memset(&s_e, 0, sizeof(s_e));
  taskENTER_CRITICAL(&s_trava);
  memset(&s_foto, 0, sizeof(s_foto));
  s_tem_foto = false;
  taskEXIT_CRITICAL(&s_trava);
  ajustar(md, NULL, sensor);
  return ESP_OK;
}

static void ipa_processar(esp_ipa_t *ipa, const esp_ipa_stats_t *stats, const esp_ipa_sensor_t *sensor,
                          esp_ipa_metadata_t *md)
{
  ajustar(md, stats, sensor);
}

static void ipa_destruir(esp_ipa_t *ipa) { }   // static object: nothing to release

static const esp_ipa_ops_t s_ops = {
  .init    = ipa_start,
  .process = ipa_processar,
  .destroy = ipa_destruir,
};

static esp_ipa_t s_ipa = { .name = "tab5_ipa_cor", .ops = &s_ops };

ESP_IPA_DETECT_FN(tab5_ipa_color, "tab5_ipa_cor")
{
  ESP_LOGI(TAG, "protecao do branco no pipeline (verde minimo do branco saturado %.2f)", (double)G_MIN);
  return &s_ipa;
}

// -------------------------------------------------------------------------------
// THE ALGORITHM LIST BYPASS
// -------------------------------------------------------------------------------
#define MAX_IPAS 16

const esp_ipa_config_t *__real_esp_ipa_pipeline_get_config(const char *name);

const esp_ipa_config_t *__wrap_esp_ipa_pipeline_get_config(const char *name)
{
  const esp_ipa_config_t *orig = __real_esp_ipa_pipeline_get_config(name);
#if ESP_VIDEO_ISP_DEVICE_WBG
  return orig;   // v3: ISP cuts gain before CCM, nothing to protect
#else
  if (!orig || orig->nums >= MAX_IPAS) { return orig; }

  static const char       *nomes[MAX_IPAS];
  static esp_ipa_config_t  copia;
  for (int i = 0; i < orig->nums; ++i) { nomes[i] = orig->names[i]; }
  nomes[orig->nums] = "tab5_ipa_cor";
  copia       = *orig;
  copia.names = nomes;
  copia.nums  = orig->nums + 1;
  return &copia;
#endif
}
