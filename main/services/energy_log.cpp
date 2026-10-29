#include "energy_log.h"
#include "tab5_power.hpp"
#include "tab5_sensors.hpp"
#include "tab5_mesh.hpp"
#include <time.h>
#include "tab5_sd.hpp"
#include "tab5_time.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "registro";

// One sample per minute: a complete discharge (5 to 15 hours) yields 300 to
// 900 points, enough resolution to plot the curve and calibrate the battery.
// Data is sent to the card every 10 minutes.
static const uint32_t SAMPLE_INTERVAL_MS = 60000;
static const int      FLUSH_BATCH_SIZE = 5; // samples per discharge (was 10; the TODAY graph of the Energia app is from here)
static const int      CAPACIDADE = 32;      // slack: if the card fails, it holds longer

typedef struct {
  uint32_t t;        // seconds since boot
  uint32_t epoch;    // sample's UTC time; 0 = clock not set
  float    volts;
  float    mA;
  float    mW;
  int16_t  pct;
  int16_t  mhz;
  uint8_t  sleep;
  float    temp;     // P4 silicon temperature, -273 when not measured
  char     app[12];
} sample_t;

static sample_t s_buf[CAPACIDADE];
static int       s_n = 0;
static uint32_t  s_gravadas = 0;
static uint32_t  s_write_failures = 0;
static char      s_app[12] = "home";

void registro_app_current(const char* name)
{
  // Copy by value: the app pointer may become invalid when the app closes.
  snprintf(s_app, sizeof(s_app), "%s", name ? name : "home");
}

void registro_estado(int* in_ram, uint32_t* gravadas, uint32_t* write_failures)
{
  if (in_ram)   { *in_ram   = s_n; }
  if (gravadas) { *gravadas = s_gravadas; }
  if (write_failures) { *write_failures = s_write_failures; }
}

// -------------------------------------------------------------------------------
// TASK AND HEAP HEALTH - see the full explanation in energy_log.h.
// -------------------------------------------------------------------------------
// Up to 32 simultaneous tasks, matching the limit already used by the Tasks
// app (tasks/app_tasks.cpp). Repeated here intentionally rather than shared:
// this is a CONSTANT, not state, and the two readings are independent—the app
// may not even be open.
// (The snapshot of tasks in tasks.csv, with each download, exited in
// the system monitor (services/tab5_monitor) measures the tasks to be
// every second, with spikes and events, and records what matters on the card.)

// ONE FILE PER DAY, WITH REAL TIME. Previously one energy.csv grew forever,
// with timestamps in seconds since BOOT, making a single day impossible to
// extract. Now:
// System/energy/energy-YYYY-MM-DD.csv (LOCAL date - the day of use)
// ENE3,<epoch UTC>,<HH:MM local>,<V>,<mA>,<mW>,<pct>,<MHz>,<sleep>,<temp>,<app>
// Without a valid clock (no GPS/mesh since boot), samples go to
// energy-sem-data.csv with epoch 0. The old energy.csv remains untouched.
static void energy_filename_for_day(uint32_t epoch, char* out, size_t size)
{
  tab5_day_filename(epoch, "energia-", ".csv", out, size);
}

// The system writer (hal/tab5_sd.hpp) receives each sample as a line; the
// "card" task handles mounting, opening, and writing. The header is included
// and written only when the day's file is empty.
static void descarregar(void)
{
  if (s_n == 0) { return; }
  int ok = 0;
  for (int i = 0; i < s_n; ++i) {
    const sample_t* a = &s_buf[i];
    char name[32];
    energy_filename_for_day(a->epoch, name, sizeof(name));
    char hora[8] = "--:--";
    if (a->epoch) { struct tm lt; tab5_local_time(a->epoch, &lt); snprintf(hora, sizeof(hora), "%02d:%02d", lt.tm_hour, lt.tm_min); }
    char line[160];
    snprintf(line, sizeof(line), "ENE3,%lu,%s,%.3f,%.1f,%.1f,%d,%d,%d,%.1f,%s\n",
             (unsigned long)a->epoch, hora, a->volts, a->mA, a->mW,
             (int)a->pct, (int)a->mhz, (int)a->sleep, a->temp, a->app);
    if (tab5_sd_enqueue_append(TAB5_SD_POWER, name, line,
                       "tipo,epoch,hora,volts,mA,mW,pct,mhz,repouso,temp_c,app\n")) { ++ok; }
  }
  if (ok == 0) { s_write_failures++; return; }
  s_gravadas += ok;
  s_n = 0;
}

// NO DEDICATED TASK: one sample per minute does not justify 4 KB of idle
// stack. Ride on the "card" task (tab5_sd_carona), which already wakes every
// second and writes to the card; this callback only tracks the minute.
static void registro_in_carona(void)
{
  static int64_t next_us = esp_timer_get_time() + (int64_t)SAMPLE_INTERVAL_MS * 1000;
  const int64_t now_us = esp_timer_get_time();
  if (now_us < next_us) { return; }
  next_us += (int64_t)SAMPLE_INTERVAL_MS * 1000;
  if (next_us <= now_us) { next_us = now_us + (int64_t)SAMPLE_INTERVAL_MS * 1000; }   // was too late: start again
  {
    // Read the CACHE, never I2C: the bus belongs to the interface task, which
    // updates this value every second. Reading I2C here would recreate the
    // race that already costs us an afternoon (voltage alternating 4.37/8.37V).
    tab5_measurement_t m = tab5_power_latest();

    if (s_n >= CAPACIDADE) {
      // Buffer full (card missing for a long time): discards the oldest one.
      // Losing the distant past is better than losing the present.
      memmove(&s_buf[0], &s_buf[1], sizeof(sample_t) * (CAPACIDADE - 1));
      s_n = CAPACIDADE - 1;
    }

    sample_t* a = &s_buf[s_n++];
    a->t       = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000;
    a->epoch = tab5_time_epoch();
    a->volts   = m.volts;
    a->mA      = m.mA;
    a->mW      = m.mW;
    a->pct     = (int16_t)m.pct;
    a->mhz     = (int16_t)tab5_power_cpu_mhz();
    a->sleep = tab5_power_is_sleeping() ? 1 : 0;
    { float c; a->temp = tab5_sensors_temp(&c) ? c : -273.0f; }
    snprintf(a->app, sizeof(a->app), "%s", s_app);

    // Unload OS in batch. There is no advance recording of property: during
    // development, the board restarts with each upload, and a recording is immediately
    // first minute would fill the file of loose samples of sessions that
    // lasted seconds. To find out if the record is alive without waiting ten
    // minutes, look at the Energy app - it shows how many samples are in RAM.
    //
    // WITH THE DRUMS AT THE END, THE LOT BECOMES ONE - and not every sample is worth the same.
    //
    // At ten minutes per discharge, a battery shutdown takes away up to
    // ten samples. And then THOSE ten: the end of discharge and where the voltage
    // plummets, the knee of curve, the only part that says how much team still
    // remains when the answer matters. The rest of the file has hundreds of
    // samples of full battery, which all look alike.
    //
    // Below 15% the device can be disabled at any minute, and then the cost of
    // burn every time (one card assembly per minute) stays cheap close to
    // lose the only section that cannot be reproduced without downloading the
    // entire battery of new.
    const bool no_end = (m.pct >= 0 && m.pct < 15);
    if (s_n >= (no_end ? 1 : FLUSH_BATCH_SIZE)) { descarregar(); }
  }
}

void registro_start(void)
{
  // Once per minute reads the energy cache and, batch by batch, delivers the
  // lines on recorder - everything within task "cartao".
  tab5_sd_piggyback(registro_in_carona);
  ESP_LOGI(TAG, "running: sample every %lus, flush every %d samples",
           (unsigned long)(SAMPLE_INTERVAL_MS / 1000), FLUSH_BATCH_SIZE);
}
