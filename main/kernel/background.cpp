#include "background.h"
#include "app.h"
#include "tab5_owners.hpp"
#include "task_pool.h"
#include "service_stop.hpp"      // request/confirm the output - the same as GPS, Mesh and Mesh BLE

#include <string.h>
#include <stdio.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "fundo";

#define FUNDO_PRIORIDADE  1      // below interface (4), radios (3) and worker (2)
#define FUNDO_PARAR_MS    3000
#define FUNDO_CEDER_US    20000  // Fundo_ceder() only actually cedes every 20 ms

// THE STOPPING IS THE KERNEL'S (kernel/service_stop.hpp), and not a state machine
// own. Before there was one (FREE, RUNNING, LEFT, ABANDONED, TRASH) and who
// stopped WAITING ASKING the status every 10 ms, up to 3 s, in task of
// interface - the same default as service_stop.hpp retired in services. Now:
// - the DA task uses the 'exited' semaphore as the last thing (stop_confirm) and
// DON'T TOUCH THE SPOT AFTER THAT;
// - whoever stops WAITS FOR HIM: wakes up at the moment of leaving;
// - expired, the vacancy remains 'abandoned' with the request of pe, and
// fund_maintain() clears when the semaphore is finally given;
// - the loop that ends alone and seen in the same way (semaphore given without
// no one has a request).
// The 'exited' and 'wake' lights appear once per wave and are never
// deleted: the springboard can exit after the interface has given up.
struct background_s {
  const background_def_t*  def;
  volatile bool       em_uso;         // from start to cleaning (finish)
  volatile bool       stop_request;   // someone outside the interface asked (console)
  bool                stopping;        // the interface is inside ofsistema_fundo_parar()
  bool                abandoned;     // not exited within the deadline: clean when exit
  stop_t            parada;         // request of stop and confirmation of departure
  SemaphoreHandle_t   wake;        // remove the loop of Fundo_esperar(); created 1x, never deleted
  int                 notif;
  int64_t             since_us;
  int64_t             cedeu_us;
  char                summary[NOTIF_RESUMO];
};

static background_s          s_f[FUNDO_MAX];
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> s_profile_dirty{false};

static void trampolim(void* arg)
{
  background_s* f = (background_s*)arg;
  f->def->body(f);
  // The LAST time this task touches the vacancy: from now on the interface
  // can clean it (the traffic light is not turned off, only the parking space is reset).
  stop_confirm(&f->parada);
  system_wake_interface();   // fund_manter() clears who exited alone
  // Returns: the vacancy of pool returns the owner and iteration to sleep. Never vTaskDelete.
}

// So in interface (the only one that changes 'def').
static background_s* achar(const background_def_t* d)
{
  for (int i = 0; i < FUNDO_MAX; ++i) {
    if (s_f[i].em_uso && s_f[i].def == d) { return &s_f[i]; }
  }
  return nullptr;
}

// Returns to free space, preserving both traffic lights, and empties the 'wake':
// one warning that is left of stop cannot make the next loop skip the
// first wait. (The 'exited' is emptied by the para_preparar of next.)
static void reset_vaga(background_s* f)
{
  SemaphoreHandle_t wake = f->wake;
  SemaphoreHandle_t exited    = f->parada.exited;
  taskENTER_CRITICAL(&s_mux);
  memset(f, 0, sizeof(*f));   // em_uso = false
  f->wake = wake;
  f->parada.exited = exited;
  taskEXIT_CRITICAL(&s_mux);
  if (wake) { xSemaphoreTake(wake, 0); }
}

// Cleanup common to every end: stop requested, loop that ended alone, or
// abandoned que por end exited. Always in interface.
static void finalizar(background_s* f)
{
  if (f->def->on_stop) { f->def->on_stop(); }
  notification_limpar(f->notif);
  ESP_LOGI(TAG, "'%s' parou (rodou %lld s)", f->def->name,
           (esp_timer_get_time() - f->since_us) / 1000000);
  reset_vaga(f);
  s_profile_dirty = true;
}

// Has the vacancy task already exited? Does not consume the traffic light.
static bool exited(const background_s* f) { return !stop_running(&f->parada); }

bool system_fundo_start(const background_def_t* d)
{
  if (!d || !d->body || !d->name) { return false; }
  if (achar(d)) { return false; }   // already running (or still leaving)

  background_s* f = nullptr;
  for (int i = 0; i < FUNDO_MAX && !f; ++i) {
    if (!s_f[i].em_uso) { f = &s_f[i]; }
  }
  if (!f) {
    ESP_LOGW(TAG, "sem vaga para '%s' (limite %d)", d->name, FUNDO_MAX);
    return false;
  }

  if (d->on_start && !d->on_start()) {
    ESP_LOGW(TAG, "'%s' recusou iniciar", d->name);
    return false;
  }

  if (!f->wake) { f->wake = xSemaphoreCreateBinary(); }   // once per slot, forever
  reset_vaga(f);
  f->def      = d;
  f->since_us = esp_timer_get_time();
  f->cedeu_us = f->since_us;
  snprintf(f->summary, sizeof(f->summary), "rodando");

  // VISIBLE FROM THE FIRST INSTANT - the gray tablet and the promise of
  // "nothing runs hidden". The registry is idempotent by name.
  f->notif = notification_registrar(d->name);
  notification_silenciar(f->notif, d->com_sound ? !d->com_sound() : false);
  notification_publicar(f->notif, 1, NOTIF_ESTADO, f->summary);

  const bool pronta = f->wake && stop_prepare(&f->parada);
  taskENTER_CRITICAL(&s_mux);
  f->em_uso = true;   // lastly and under the lock: whoever lists sees the vacancy already complete
  taskEXIT_CRITICAL(&s_mux);
  if (!pronta
   || !pool_create(nullptr, trampolim, f, d->name, d->stack ? d->stack : 4096,
                  FUNDO_PRIORIDADE, OWNER_BACKGROUND, d->name)) {   // owner: see hal/tab5_owners.hpp
    ESP_LOGE(TAG, "sem memoria para a tarefa de '%s'", d->name);
    stop_abandon(&f->parada);
    finalizar(f);
    return false;
  }
  s_profile_dirty = true;
  system_wake_interface();
  ESP_LOGI(TAG, "'%s' rodando em fundo", d->name);
  return true;
}

bool system_fundo_stop(const background_def_t* d)
{
  background_s* f = achar(d);
  if (!f || f->stopping || f->abandoned) { return false; }   // abandoned: we've already given up on her

  f->stopping = true;
  // The request BEFORE wake: the woken loop must already see "stop", otherwise
  // iteration to sleep its entire wait (fundo_wait can be of minutes).
  f->parada.request = true;
  xSemaphoreGive(f->wake);
  if (stop_request(&f->parada, FUNDO_PARAR_MS)) {   // waits at the traffic lights, without asking
    finalizar(f);
    return true;
  }
  // We don't kill the task (see why in kernel/app_task.h): the request stays
  // of pe, it exits when it can, and Fundo_manter() clears at that time. A
  // gum tells the truth in the meantime.
  ESP_LOGE(TAG, "'%s' nao saiu em %d ms - fica parando em fundo", d->name, FUNDO_PARAR_MS);
  notification_publicar(f->notif, 1, NOTIF_ESTADO, "parando...");
  f->abandoned = true;
  f->stopping = false;
  return false;
}

bool system_fundo_active(const background_def_t* d)
{
  bool active = false;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    const background_s* f = &s_f[i];
    if (f->em_uso && f->def == d && !f->abandoned && !stop_requested(&f->parada) && !exited(f)) { active = true; }
  }
  taskEXIT_CRITICAL(&s_mux);
  return active;
}

int system_fundo_listar(background_info_t* out, int max)
{
  int n = 0;
  const int64_t now = esp_timer_get_time();
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX && n < max; ++i) {
    const background_s* f = &s_f[i];
    if (!f->em_uso) { continue; }
    out[n].name = f->def->name;
    out[n].ha_s = (uint32_t)((now - f->since_us) / 1000000);
    memcpy(out[n].summary, f->summary, sizeof(out[n].summary));
    ++n;
  }
  taskEXIT_CRITICAL(&s_mux);
  return n;
}

// Of ANY task (the console): just mark, and the interface for of truth in
// Fundo_manter() - so ao_parar() always runs in interface, as promised by
// contract.
bool system_fundo_stop_name(const char* name)
{
  // Under the lock: runs on the console, and the interface may be resetting the slot.
  bool achou = false;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    background_s* f = &s_f[i];
    if (f->em_uso && !f->abandoned && strcmp(f->def->name, name) == 0) {
      f->stop_request = true;
      achou = true;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
  if (achou) { system_wake_interface(); }
  return achou;
}

// ---- the loop ---------------------------------------------------------------

bool background_wait(background_t* f, uint32_t ms)
{
  if (stop_requested(&f->parada)) { return false; }
  xSemaphoreTake(f->wake, pdMS_TO_TICKS(ms));
  f->cedeu_us = esp_timer_get_time();
  return !stop_requested(&f->parada);
}

bool background_yield(background_t* f)
{
  const int64_t now = esp_timer_get_time();
  if (now - f->cedeu_us >= FUNDO_CEDER_US) {
    vTaskDelay(1);   // this core's IDLE runs: watchdog and energy up to date
    f->cedeu_us = esp_timer_get_time();
  }
  return !stop_requested(&f->parada);
}

bool background_running(const background_t* f) { return !stop_requested(&f->parada); }

void background_estado(background_t* f, int count, const char* summary)
{
  char r[NOTIF_RESUMO];
  snprintf(r, sizeof(r), "%s", summary ? summary : "");
  taskENTER_CRITICAL(&s_mux);
  memcpy(f->summary, r, sizeof(r));
  taskEXIT_CRITICAL(&s_mux);
  // Minimum quantity 1: the tablet never disappears while the bottom rotates.
  notification_publicar(f->notif, count > 0 ? count : 1, NOTIF_ESTADO, r);
}

// ---- the kernel ---------------------------------------------------------------

// Under the lock:sistema_impor_preferencias() can be called outside of
// interface. So SOMA - and the ceiling of clock (cpu_max) stays with the screen: one bottom
// with the default profile (360) would delete the bottom ceiling that one app requested.
void background_somar_profile(tab5_profile_t& p)
{
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    const background_s* f = &s_f[i];
    if (!f->em_uso) { continue; }
    const tab5_profile_t& q = f->def->profile;
    p.imu          = p.imu          || q.imu;
    p.microphone    = p.microphone    || q.microphone;
    p.speaker = p.speaker || q.speaker;
    p.grove_5v     = p.grove_5v     || q.grove_5v;
    p.usb_a_5v     = p.usb_a_5v     || q.usb_a_5v;
    p.camera       = p.camera       || q.camera;
    p.radio        = p.radio        || q.radio;
    if (q.cpu_min_mhz > p.cpu_min_mhz) { p.cpu_min_mhz = q.cpu_min_mhz; }
  }
  taskEXIT_CRITICAL(&s_mux);
}

int background_piso_sleep(void)
{
  int piso = 0;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < FUNDO_MAX; ++i) {
    if (s_f[i].em_uso && s_f[i].def->sleep_clock_floor_mhz > piso) {
      piso = s_f[i].def->sleep_clock_floor_mhz;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
  return piso;
}

void background_manter(void)
{
  for (int i = 0; i < FUNDO_MAX; ++i) {
    background_s* f = &s_f[i];
    if (!f->em_uso || f->stopping) { continue; }
    if (exited(f)) {
      // The loop ended on its own, or abandoned by end exited: collects the
      // traffic light (it was given) and clear.
      stop_request(&f->parada, 0);
      finalizar(f);
    } else if (f->stop_request && !f->abandoned) {
      f->stop_request = false;
      system_fundo_stop(f->def);
    }
  }
  // The changed set: the effective profile (screen + backgrounds) is reapplied here, in
  // interface, and only here. ON STANDBY: applying profile changes brightness and
  // ceiling of clock, which sleep is holding - worth on wake.
  if (!tab5_power_is_sleeping() && s_profile_dirty.exchange(false)) { system_reapply_profile(); }
}
