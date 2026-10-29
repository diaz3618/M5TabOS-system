// IMU (BMI270) - shared reading, in the SAME spirit of tab5_power.hpp
// (measure() vs latest()) and tab5_sensors.hpp (cache instead of queue).
//
// ------------------------------------------------------------------------
// WHY THIS EXISTS - find on review "ready for more of one
// consumer", the same exercise that corrected tab5_gps.cpp.
// ------------------------------------------------------------------------
// app_imu.cpp always read the direct BMI270 (M5.Imu.update()/getAccel/getGyro),
// and that is SAFE today - but not because anyone thought about competition: and
// because the only caller is the draw() of app itself, which only runs in
// task of interface, the ONLY one that can talk on the internal I2C bus
// (rule documented in tab5_power.hpp: "the I2C_Class of M5Unified does not have
// mutex and two interleaving tasks read the changed register without giving an error").
// No other app had a way to ask "what is the inclination of the device
// now" without duplicating that code AND without running the risk of accessing the
// bus of a wrong task.
//
// THE SOLUTION, EXACTLY AS FOR ENERGY AND TEMPERATURE: a function that MEASURE
// of truth (so of task of interface) and another that READS THE CACHE (any
// task, protected by mutex - see warning about tab5_gps.cpp that prompted
// this).
//
// ------------------------------------------------------------------------
// DIFFERENT CADENCES ON PURPOSE - this is NOT just "copying the default of INA226"
// ------------------------------------------------------------------------
// tab5_power_measure()/tab5_sensors_measure() run 1x/second because power and
// temperature changes slowly - 1Hz is suitable for any consumer, including
// who "and owner" of them. The IMU is different: app_imu.cpp itself needs
// reading EVERY FRAME so that the level indicator does not appear stuck -
// sampling just 1x/second would worsen the ONLY screen that actually uses this today.
//
// That's why app_imu.cpp CONTINUES reading directly (M5.Imu.update()/getAccel/
// getGyro), without going through here - it is already safe (same task) and already
// quickly. tab5_imu_measure() below runs in parallel, 1x/second, feeding
// ONLY the cache that OTHER apps will read - "the device is offline or
// lying down", not "the bubble level needs to be fluid". A consumer who
// needing more frequency than this in the future needs the SAME discussion
// that this file documents, not just increase one number.
#pragma once

#include <stdbool.h>

typedef struct {
  bool  valid;   // false until the first measurement (or if the IMU never went up)
  float ax, ay, az;   // g (acceleration, including gravity)
  float gx, gy, gz;   // degrees/s (gyroscope)
} tab5_imu_data_t;

// Reads the REAL BMI270 and updates the cache. INTERFACE TASK OS -
// same rule of INA226/temperature/keyboard, see the big warning above.
// Silent no-op if M5.Imu.isEnabled() is false (profile.imu=false no
// boot of this app, or no app that needs IMU has run yet).
void tab5_imu_measure(void);

// The ULTIMATE reading, no touch on I2C - what any task should use.
// Returns false (with 'valid'=false in '*output') if the IMU never measured.
bool tab5_imu_latest(tab5_imu_data_t* output);
