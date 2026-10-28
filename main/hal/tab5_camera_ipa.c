// PROTECAO DO BRANCO NO PIPELINE DO ISP (um IPA proprio, "tab5_ipa_cor").
//
// O DEFEITO. Branco estourado sai magenta, na tela ao vivo e na foto.
//
// A CAUSA, no silicio. O ISP do P4 abaixo da v3.0 nao tem o bloco de ganho de
// balanco de branco (WBG). O esp_video compensa multiplicando as colunas R e B
// da matriz de color (CCM) pelos ganhos do AWB (esp_video_isp_device.c,
// isp_init_ccm_param). Matematicamente da no mesmo - ate o sensor saturar. Num
// pixel estourado o sensor entrega (255,255,255) raw; o ganho, que deveria
// levar R e B alem de 255 e ser CORTADO ali (e o que o WBG da v3 faz), entra
// direto na CCM sem corte nenhum. A linha do verde da CCM tem coeficientes
// negativos para R e B (tipico: -0,40 R + 1,63 G - 0,23 B), entao:
//
//   G_saida = -0,40 * 255 * gR + 1,63 * 255 - 0,23 * 255 * gB
//
// com gR = 2,0 e gB = 1,7 (luz do dia) da 0,43 * 255. R e B saem cortados em
// 255: o branco vira (255, 110, 255) linear, (255, 154, 255) depois da gama.
// Em luz quente (gR 1,6, gB 2,0) fica pior: (255, 117, 255). Nao e memoria,
// nao e exposicao: e algebra.
//
// A CORRECAO. Chamando s = (gR, 1, gB), o branco saturado sai da linha do
// verde como m . s. Basta essa linha cumprir duas coisas:
//   1. m . (1,1,1) igual a da calibracao - cinza continua cinza;
//   2. m . s >= G_MIN - o branco saturado continua (quase) branco.
// Sao 2 restricoes em 3 incognitas: sobra um grau de liberdade, usado para
// ficar o mais perto possivel da linha original, medindo o erro nas 24
// manchas de uma carta de color (Macbeth) levadas para o dominio do sensor.
// Isso tem solucao fechada (minimos quadrados com restricao de igualdade):
//
//   m' = m - Ci A^T (A Ci A^T)^-1 (A m - b),   Ci = M^T K M
//
// com K = (T T^T)^-1 constante (T = as 24 manchas em sRGB linear). Linhas R e
// B ficam como estao: o branco saturado ja sai delas acima de 1 e e cortado
// em 255, que e o certo.
//
// O CUSTO. A linha do verde e a que mais separa verdes e magentas. Mexer nela
// tira um pouco de saturation dessas cores (simulado: dE medio ~5 na carta).
// Um reforco de 10% na saturation do ISP (dominio YUV, onde branco tem croma
// zero e nao e afetado) devolve parte disso. Na v3 (WBG de verdade) nada
// disto e preciso e o IPA nem entra no pipeline.
//
// COMO ENTRA NO PIPELINE SEM EDITAR O COMPONENTE. O esp_video pega a lista de
// algoritmos de esp_ipa_pipeline_get_config(); o link desvia essa chamada
// para ca (-Wl,--wrap, main/CMakeLists.txt) e devolvemos a mesma
// configuracao com "tab5_ipa_cor" no FIM da lista. O pipeline acha o
// algoritmo pelo name na secao .esp_ipa_detect (ESP_IPA_DETECT_FN) e o roda
// a cada quadro, DEPOIS do AWB e do ACC: ele recebe a CCM e os ganhos ja
// decididos e so ajusta a matriz antes de ela ir para o ISP.
//
// Arquivo em C, nao C++: o ESP_IPA_DETECT_FN usa inicializadores designados
// fora da ordem dos campos, o que o C++ recusa.

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_ipa.h"
#include "esp_ipa_detect.h"
#include "esp_video_caps.h"
#include "tab5_camera_ipa.h"

static const char *TAG = "tab5_ipa_cor";

// Verde minimo de um branco saturado (1 = branco puro). 0,92 sai, depois da
// gama, como ~243 de 255: imperceptivel ao lado de R e B em 255.
#define G_MIN        0.92f
// Reforco da saturation quando a matriz foi protegida (percentual).
#define SAT_REFORCO  110
#define SAT_PADRAO   128   // 1,0 no formato do ISP

// (T T^T)^-1 das 24 manchas da carta em sRGB linear (D65).
static const float K[3][3] = {
  {  0.80074f, -0.90448f,  0.09025f },
  { -0.90448f,  2.03918f, -0.97943f },
  {  0.09025f, -0.97943f,  1.19777f },
};

typedef struct {
  bool     tem_ccm, tem_sat;
  float    ccm_awb[3][3];      // a latest que o ACC mandou (sem protecao)
  uint32_t sat_awb;
  float    ccm_enviada[3][3];  // a latest que foi para o ISP
  uint32_t sat_enviada;
} estado_t;

static estado_t s_e;

static portMUX_TYPE          s_trava = portMUX_INITIALIZER_UNLOCKED;
static tab5_cam_ipa_state_t s_foto;
static bool                  s_tem_foto;

static float ganho_valido(float g) { return (g > 0.1f && g < 8.0f) ? g : 1.0f; }

// A linha do verde protegida (ver o header). Devolve o verde do branco
// saturado antes da protecao; 'output' recebe a matriz inteira.
static float proteger(const float M[3][3], float gR, float gB, float output[3][3])
{
  memcpy(output, M, sizeof(float) * 9);
  const float s[3] = { gR, 1.0f, gB };
  const float *m = M[1];
  const float g_antes = m[0] * s[0] + m[1] * s[1] + m[2] * s[2];
  if (g_antes >= G_MIN) { return g_antes; }

  // Ci = M^T K M (simetrica)
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

  // r = A m - b: a soma da linha nao muda (r0 = 0), o branco sobe ate G_MIN.
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
  if (!s_e.tem_ccm) { return; }   // ISP ainda na matriz identidade: branco ja e branco

  const float gR = ganho_valido(md->red_gain);
  const float gB = ganho_valido(md->blue_gain);
  float nova[3][3];
  const float g_antes = proteger(s_e.ccm_awb, gR, gB, nova);
  const bool  protegida = g_antes < G_MIN;

  // Manda a matriz quando o ACC mandou uma (a dele esta em md->ccm e precisa
  // ser trocada) ou quando o resultado mudou (ganhos novos do AWB).
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

  // Foto para o diagnostico. Copia curta (~200 B) sob trava de giro: who le
  // e o console, em outra tarefa.
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

static esp_err_t ipa_iniciar(esp_ipa_t *ipa, const esp_ipa_sensor_t *sensor, esp_ipa_metadata_t *md)
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

static void ipa_destruir(esp_ipa_t *ipa) { }   // objeto estatico: nada a release

static const esp_ipa_ops_t s_ops = {
  .init    = ipa_iniciar,
  .process = ipa_processar,
  .destroy = ipa_destruir,
};

static esp_ipa_t s_ipa = { .name = "tab5_ipa_cor", .ops = &s_ops };

ESP_IPA_DETECT_FN(tab5_ipa_cor, "tab5_ipa_cor")
{
  ESP_LOGI(TAG, "protecao do branco no pipeline (verde minimo do branco saturado %.2f)", (double)G_MIN);
  return &s_ipa;
}

// ---------------------------------------------------------------------------
// O DESVIO DA LISTA DE ALGORITMOS
// ---------------------------------------------------------------------------
#define MAX_IPAS 16

const esp_ipa_config_t *__real_esp_ipa_pipeline_get_config(const char *name);

const esp_ipa_config_t *__wrap_esp_ipa_pipeline_get_config(const char *name)
{
  const esp_ipa_config_t *orig = __real_esp_ipa_pipeline_get_config(name);
#if ESP_VIDEO_ISP_DEVICE_WBG
  return orig;   // v3: o ISP corta o ganho antes da CCM, nada a proteger
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
