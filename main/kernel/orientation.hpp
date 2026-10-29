// SCREEN ORIENTATION - who decides whether the open app stays standing up or lying down.
//
// The app declares (app_t.orientation, see kernel/app.h); this module applies. For
// ORI_EM_PE and ORI_DEITADO and only the right rotation in opening. For
// ORI_SEGUE_IMU it also monitors gravity while the app is open and
// rotates between the TWO states that exist - standing (rotation 0) and lying down
// (rotation 3, ExtPort1 below). Never the opposite side of any of them.
//
// IS NOT A TASK. The IMU stays on internal I2C, which is the task of interfacing
// can use (see hal/tab5_imu.hpp); so the watchman is a function that the loop of
// interface calls every iteration. With no app following the IMU it returns in
// first line: Those who don't use it don't pay for reading none.
//
// EVERYTHING HERE AND FROM THE INTERFACE TASK.
#pragma once

#include <stdint.h>
#include "app.h"

// Applies the initial orientation of the app that is opening. Call BEFORE of profile
// and of any drawing: rotate changes width and height, and everything else
// calculated from them needs to see the new value.
void orientation_on_open(const app_t* app);

// The app closed: iteration in pe (home is always in pe) and to watch.
void orientation_on_close(void);

// An iteration of watchman. 'pode_girar' = no fingers on screen, nothing in front of
// app (blocking), app already open indeed. Sampling continues even when
// can't - only the exchange waits.
//
// Returns true when TURNED on this call: the caller asks for the pass
// complete and notify the app (when rotating).
bool orientation_vigiar(uint32_t now_ms, bool pode_rotate);

// How much is left until the next sample, in ms - for the stopped interface, no
// sleep beyond it. UINT32_MAX when no app follows the IMU.
uint32_t orientation_time_until_sample_ms(uint32_t now_ms);
