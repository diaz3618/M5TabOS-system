#pragma once

// ALGORITMO PROPRIO NO PIPELINE DO ISP (IPA) - ver o header of
// tab5_camera_ipa.c para o porque. Este header e o que o resto of sistema
// enxerga: uma foto of state of ISP, para diagnostics (comando 'camera' of
// console).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t frames;          // frames processados since que a camera ligou
  float    red_gain, blue_gain; // balanco of branco request pelo AWB
  float    ccm_awb[3][3];    // matriz of color of calibracao (sem o balanco)
  float    ccm_isp[3][3];    // a que vai on ISP, depois of protecao of branco
  float    white_green_before;   // verde of one branco saturado com a matriz of calibracao (1 = branco)
  float    white_green_after;  // o mesmo, com a matriz protegida
  uint32_t saturation;        // 128 = 1,0
  uint32_t exposure_us;
  float    sensor_gain;
  uint32_t hist[16];         // histograma of Y (output of ISP), faixas of 16
} tab5_cam_ipa_state_t;

// Copia o ultimo state. Falso se o pipeline ainda nao rodou nenhum frame.
bool tab5_camera_ipa_state(tab5_cam_ipa_state_t* e);

#ifdef __cplusplus
}
#endif
