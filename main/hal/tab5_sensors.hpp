// Environmental sensors in the device itself.
//
// THE TAB5 HAS NO HUMIDITY SENSOR. M5Unified declares none for this board, and
// the internal bus contains only touch (GT911), the INA226 power monitor, the
// IMU, and the expanders. Humidity would require a Grove module, and Grove
// already has an owner.
//
// TEMPERATURE exists, with a caveat that must accompany the number: this is the
// ESP32-P4 SILICON temperature, not air temperature. The chip heats itself, so
// it reads above ambient, often well above it with the screen on. It shows the
// device warming up (and the effect of DFS), not whether the room is cold.
//
// This was chosen over IMU temperature because it costs no I2C transaction
// (it is an internal ADC) and does not depend on the IMU being awake.
//
// A MODULE, NOT A QUEUE API, FOR NOW. The status bar is the only consumer today;
// a queue for one reader would be structure without payload. The reading is
// already cached. Future consumers can call the same function, and a queue can
// arrive when someone waits for events instead of polling.
#pragma once

#include <stdbool.h>

// Enables the sensor once at boot. Reads return false until this is called.
void tab5_sensors_init(void);

// REALLY reads the sensor and updates the cache. UI task only, alongside power
// sampling. It is cheap, but not free.
void tab5_sensors_measure(void);

// Latest reading. false if the sensor failed to start or has not been sampled.
bool tab5_sensors_temp(float* celsius);

// Average of all readings since boot: this device's "normal" for the current
// session, giving the current value something to compare against. false until
// at least one sample exists.
bool tab5_sensors_average_temp(float* celsius);
