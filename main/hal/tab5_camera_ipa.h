// Camera ISP diagnostics exposed by tab5_camera_ipa.c.
//
// The implementation adjusts the ISP color matrix to protect the green
// channel from the P4 ISP's missing white-balance-gain block. This header is
// the read-only state snapshot visible to the rest of the system and to the
// camera diagnostics command.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t frames;                 // frames processed since the camera started
  uint32_t exposure_us;            // current exposure time
  float    sensor_gain;             // current sensor gain
  float    red_gain, blue_gain;    // white-balance gains requested by AWB
  float    ccm_awb[3][3];          // color-calibration matrix before protection
  float    ccm_isp[3][3];          // matrix sent to the ISP after protection
  float    white_green_before;     // green response for saturated white before calibration
  float    white_green_after;      // the same response after matrix protection
  uint32_t saturation;              // ISP saturation setting
  uint32_t hist[16];                // luminance histogram bins
} tab5_cam_ipa_state_t;

#ifdef __cplusplus
extern "C" {
#endif

// Copies the latest state. Returns false until the pipeline has processed a frame.
bool tab5_camera_ipa_state(tab5_cam_ipa_state_t* state);

#ifdef __cplusplus
}
#endif
