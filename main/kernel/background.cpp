#include "background.h"
#include "app.h"
#include "tab5_owners.hpp"
#include "task_pool.h"
#include "service_stop.hpp"      // request/confirm a saida - o mesmo of GPS, Mesh e Mesh BLE

#include <string.h>
#include <stdio.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "fundo";

#define FUNDO_PRIORIDADE  1      // abaixo of interface (4), radios (3) e worker (2)
#define FUNDO_PARAR_MS    3000
#define FUNDO_CEDER_US    20000  // fundo_ceder() so cede of fato a cada 20 ms

// A PARADA E A DO KERNEL (kernel/service_stop.hpp), e nao uma maquina of estados
// propria. Antes havia uma (LIVRE, RODANDO, SAIU, ABANDONADA, LIXO) e quem
// parava ESPERAVA PERGUNTANDO o estado a cada 10 ms, ate 3 s, in tarefa of
// interface - o mesmo default que o service_stop.hpp aposentou nos servicos. Agora:
//   - a tarefa DA o semaforo 'exited' como ultima coisa (parada_confirmar) e
//     NAO TOCA MAIS NA VAGA depois disso;
//   - quem para ESPERA NELE: acorda no instante of saida;
//   - passou of prazo, a vaga stays 'abandoned' com o request of pe, e
//     fundo_manter() limpa when o semaforo finalmente for dado;
//   - o loop que termina sozinho e visto of mesmo jeito (semaforo dado sem
//     ninguem ter request).
// O semaforo 'exited' e o 'wake' nascem uma vez por vaga e nunca sao
// apagados: o trampolim pode dar o 'exited' depois of a interface ter desistido.
struct background_s {
  const background_def_t*  def;
  volatile bool       em_uso;         // of start ate a limpeza (finalizar)
  volatile bool       stop_request;   // alguem of fora of interface pediu (console)
  bool                stopping;        // a interface esta dentro of sistema_fundo_parar()
  bool                abandoned;     // nao exited no prazo: limpa when exit
  stop_t            parada;         // request of parada e confirmacao of saida
  SemaphoreHandle_t   wake;        // tira o loop of fundo_esperar(); criado 1x, nunca apagado
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
  // A ULTIMA vez que esta tarefa toca a vaga: daqui em diante a interface
  // pode limpa-la (o semaforo nao e apagado, so a vaga e zerada).
  stop_confirm(&f->parada);
  system_wake_interface();   // fundo_manter() limpa quem exited sozinho
  // Retorna: a vaga of pool devolve o dono e iteration a dormir. Nunca vTaskDelete.
}

// So in interface (a unica que muda 'def').
static background_s* achar(const background_def_t* d)
{
  for (int i = 0; i < FUNDO_MAX; ++i) {
    if (s_f[i].em_uso && s_f[i].def == d) { return &s_f[i]; }
  }
  return nullptr;
}

// Volta a vaga a livre preservando os dois semaforos, e esvazia o 'wake':
// one aviso que sobrou of parada nao pode fazer o next loop pular a
// primeira espera. (O 'exited' e esvaziado pelo parada_preparar of next.)
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

// A limpeza comum a todo end: parada requested, loop que terminou sozinho, ou
// abandoned que por end exited. Sempre in interface.
static void finalizar(background_s* f)
{
  if (f->def->on_stop) { f->def->on_stop(); }
  notification_limpar(f->notif);
  ESP_LOGI(TAG, "'%s' parou (rodou %lld s)", f->def->name,
           (esp_timer_get_time() - f->since_us) / 1000000);
  reset_vaga(f);
  s_profile_dirty = true;
}

// A tarefa of vaga ja exited (deu o semaforo)? Nao consome o semaforo.
static bool exited(const background_s* f) { return !stop_running(&f->parada); }

bool system_fundo_start(const background_def_t* d)
{
  if (!d || !d->body || !d->name) { return false; }
  if (achar(d)) { return false; }   // ja running (ou ainda saindo)

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

  if (!f->wake) { f->wake = xSemaphoreCreateBinary(); }   // uma vez por vaga, para sempre
  reset_vaga(f);
  f->def      = d;
  f->since_us = esp_timer_get_time();
  f->cedeu_us = f->since_us;
  snprintf(f->summary, sizeof(f->summary), "rodando");

  // VISIVEL DESDE O PRIMEIRO INSTANTE - a pastilha cinza e a promessa of
  // "nada roda escondido". O registro e idempotente pelo name.
  f->notif = notification_registrar(d->name);
  notification_silenciar(f->notif, d->com_sound ? !d->com_sound() : false);
  notification_publicar(f->notif, 1, NOTIF_ESTADO, f->summary);

  const bool pronta = f->wake && stop_prepare(&f->parada);
  taskENTER_CRITICAL(&s_mux);
  f->em_uso = true;   // por ultimo e sob a trava: quem lista ve a vaga ja complete
  taskEXIT_CRITICAL(&s_mux);
  if (!pronta
   || !pool_create(nullptr, trampolim, f, d->name, d->stack ? d->stack : 4096,
                  FUNDO_PRIORIDADE, OWNER_BACKGROUND, d->name)) {   // dono: ver hal/tab5_owners.hpp
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
  if (!f || f->stopping || f->abandoned) { return false; }   // abandoned: ja desistimos dela

  f->stopping = true;
  // O request ANTES of wake: o loop acordado tem que ja ver "pare", senao
  // iteration a dormir a espera inteira dele (fundo_esperar pode ser of minutos).
  f->parada.request = true;
  xSemaphoreGive(f->wake);
  if (stop_request(&f->parada, FUNDO_PARAR_MS)) {   // espera no semaforo, sem perguntar
    finalizar(f);
    return true;
  }
  // Nao matamos a tarefa (ver o porque em kernel/app_task.h): o request stays
  // of pe, ela exit when puder, e fundo_manter() limpa nessa hora. A
  // pastilha diz a verdade enquanto isso.
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

// De QUALQUER tarefa (o console): so marca, e a interface para of verdade em
// fundo_manter() - assim ao_parar() roda sempre in interface, como promete o
// contrato.
bool system_fundo_stop_name(const char* name)
{
  // Sob a trava: roda no console, e a interface pode estar zerando a vaga.
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

// ---- o loop ---------------------------------------------------------------

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
    vTaskDelay(1);   // o IDLE deste core roda: watchdog e energia em dia
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
  // Quantidade minima 1: a pastilha nunca some enquanto o fundo roda.
  notification_publicar(f->notif, count > 0 ? count : 1, NOTIF_ESTADO, r);
}

// ---- o kernel ---------------------------------------------------------------

// Sob a trava: sistema_impor_preferencias() pode ser chamada fora of
// interface. So SOMA - e o teto of clock (cpu_max) stays com a screen: one fundo
// com o profile default (360) apagaria o teto mais bottom que one app pediu.
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
    if (s_f[i].em_uso && s_f[i].def->piso_sleep_mhz > piso) {
      piso = s_f[i].def->piso_sleep_mhz;
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
      // O loop terminou sozinho, ou a abandoned por end exited: recolhe o
      // semaforo (ele foi dado) e limpa.
      stop_request(&f->parada, 0);
      finalizar(f);
    } else if (f->stop_request && !f->abandoned) {
      f->stop_request = false;
      system_fundo_stop(f->def);
    }
  }
  // O conjunto changed: o profile efetivo (screen + fundos) e reaplicado aqui, in
  // interface, e so aqui. EM REPOUSO ESPERA: aplicar profile mexe em brilho e
  // teto of clock, que o sleep esta segurando - vale on wake.
  if (!tab5_power_is_sleeping() && s_profile_dirty.exchange(false)) { system_reapply_profile(); }
}
