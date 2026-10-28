#pragma once

// ALGORITMO PROPRIO NO PIPELINE DO ISP (IPA) - ver o header de
// tab5_camera_ipa.c para o porque. Este header e o que o resto do sistema
// enxerga: uma foto do state do ISP, para diagnostico (comando 'camera' do
// console).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t frames;          // frames processados desde que a camera ligou
  float    red_gain, blue_gain; // balanco de branco pedido pelo AWB
  float    ccm_awb[3][3];    // matriz de color da calibracao (sem o balanco)
  float    ccm_isp[3][3];    // a que vai ao ISP, depois da protecao do branco
  float    white_green_before;   // verde de um branco saturado com a matriz da calibracao (1 = branco)
  float    white_green_after;  // o mesmo, com a matriz protegida
  uint32_t saturation;        // 128 = 1,0
  uint32_t exposure_us;
  float    sensor_gain;
  uint32_t hist[16];         // histograma do Y (output do ISP), faixas de 16
} tab5_cam_ipa_state_t;

// Copia o ultimo state. Falso se o pipeline ainda nao rodou nenhum quadro.
bool tab5_camera_ipa_state(tab5_cam_ipa_state_t* e);

#ifdef __cplusplus
}
#endif
