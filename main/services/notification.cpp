#include "notification.h"
#include "tab5_mutex.hpp"
#include "app.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "notif";

typedef struct {
  const char*   name;
  const char*   app;                    // see notif_associar_app()
  int           count;
  notification_level_t level;
  char          summary[NOTIF_RESUMO];
  bool          usada;
  bool          silenciada;             // see notif_silenciar()
  int           count_at_last_beep;    // see notif_houve_novidade_sonora()
  uint32_t      changed_version;          // s_version of last change (tiebreaker of highlight)
} notification_source_t;

static notification_source_t s_sources[NOTIF_MAX_FONTES];
static int              s_n = 0;
static uint32_t         s_versao = 0;
static SemaphoreHandle_t s_trava = nullptr;

// The mutex is created on the FIRST call, not in notif_iniciar(), so the
// module imposes no initialization order: a service starting before the
// system may register without requiring a maintained "phase 0".
// up to date. Mechanism and risk documented in tab5_mutex.hpp; the reason to be
// safe HERE specifically and the same as always: the first registration
// happens before there are two competing tasks - services register
// on raise, one at a time, from the same task.
static inline void lock(void)    { tab5_mutex_lock(&s_trava); }
static inline void destravar(void) { tab5_mutex_destravar(&s_trava); }

int notification_registrar(const char* name)
{
  if (!name) { name = "?"; }
  lock();
  int id = -1;
  // Same name = same source: vague iteration, zeroed (see the .hpp).
  for (int i = 0; i < s_n; ++i) {
    if (s_sources[i].usada && strcmp(s_sources[i].name, name) == 0) { id = i; break; }
  }
  if (id >= 0) {
    // THE SLOT RETURNS WITH ITS STATE (see the header). Clearing it made here
    // the Mesh notice disappears whenever the service restarts through another path.
    destravar();
    return id;
  }
  if (s_n < NOTIF_MAX_FONTES) {
    id = s_n++;
    memset(&s_sources[id], 0, sizeof(s_sources[id]));
    s_sources[id].name  = name;
    s_sources[id].app   = name;   // default: the app of the same name (notif_associar_app)
    s_sources[id].usada = true;
  }
  destravar();

  if (id < 0) { ESP_LOGE(TAG, "no slot for '%s' (limit %d)", name, NOTIF_MAX_FONTES); }
  else        { ESP_LOGI(TAG, "'%s' can publish notifications (slot %d)", name, id); }
  return id;
}

void notification_publicar(int id, int count, notification_level_t level, const char* summary)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  if (count < 0) { count = 0; }

  bool changed = false;
  lock();
  notification_source_t* f = &s_sources[id];
  if (f->usada) {
    // The version changes only when something REALLY changes. Without this
    // comparison, a service republishing the same state every second would
    // repaint the status bar every second and make it flicker.
    changed = (f->count != count)
         || (f->level != level)
         || (strncmp(f->summary, summary ? summary : "", NOTIF_RESUMO) != 0);
    if (changed) {
      f->count = count;
      f->level      = level;
      snprintf(f->summary, sizeof(f->summary), "%s", summary ? summary : "");
      ++s_versao;
      f->changed_version = s_versao;
    }
  }
  destravar();

  // FIRST ACTIVE PRODUCER OF THE EVENTS QUEUE - Stage 2 of docs/ARCHITECTURE.md
  // . "changed" is checked OUTSIDE of section locked of purpose:
  //sistema_acordar_interface() only puts one event in a different queue and never
  // blocks, but there is no reason to hold this module's mutex while borrowing
  // on the other (avoid any order of lock between the two, even if today
  // neither could cause a problem). Mesh and Track (the only two
  // Services that call notif_publicar() today get this for free without
  // needing to know this queue exists.
  if (changed) { system_wake_interface(); }
}

void notification_silenciar(int id, bool silenciar)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  lock();
  s_sources[id].silenciada = silenciar;
  destravar();
}

void notification_associar_app(int id, const char* app_name)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  lock();
  if (s_sources[id].app != app_name) { s_sources[id].app = app_name; ++s_versao; }
  destravar();
}

uint32_t notification_versao(void)
{
  lock();
  const uint32_t v = s_versao;
  destravar();
  return v;
}

bool notification_houve_novidade_sonora(void)
{
  lock();
  bool touch = false;
  for (int i = 0; i < s_n; ++i) {
    notification_source_t* f = &s_sources[i];
    if (!f->usada) { continue; }
    // ALWAYS compare against the last value seen by THIS function, not an
    // aggregate total (that used to live in kernel/core.cpp, which could not
    // identify the changed source). Update the "last seen" value for every
    // source, silenced or not. If the user unmutes a source that increased
    // while muted, that must NOT count as retroactive news; only future
    // increases should sound.
    if (f->count > f->count_at_last_beep && !f->silenciada) {
      touch = true;
    }
    f->count_at_last_beep = f->count;
  }
  destravar();
  return touch;
}

// 'a' comes before 'b' in slash? Lowest first; in the draw, the most recent.
static bool comes_before(const notification_source_t* a, const notification_source_t* b)
{
  if (a->level != b->level) { return a->level > b->level; }
  return a->changed_version > b->changed_version;
}

static void copiar(notification_item_t* saida, const notification_source_t* f)
{
  saida->name       = f->name;
  saida->app        = f->app;
  saida->count = f->count;
  saida->level      = f->level;
  snprintf(saida->summary, sizeof(saida->summary), "%s", f->summary);
}

int notification_listar(notification_item_t* saida, int max)
{
  if (!saida || max <= 0) { return 0; }
  int order[NOTIF_MAX_FONTES];
  int n = 0;
  lock();
  for (int k = 0; k < s_n; ++k) {
    const notification_source_t* f = &s_sources[k];
    if (!f->usada || f->count <= 0) { continue; }
    // Ordered insertion: with only eight sources, qsort is not worthwhile.
    int j = n++;
    while (j > 0 && comes_before(f, &s_sources[order[j - 1]])) { order[j] = order[j - 1]; --j; }
    order[j] = k;
  }
  if (n > max) { n = max; }
  for (int i = 0; i < n; ++i) { copiar(&saida[i], &s_sources[order[i]]); }
  destravar();
  return n;
}

bool notification_destaque(notification_item_t* saida, int* outras)
{
  lock();
  int melhor = -1, ativas = 0;
  for (int k = 0; k < s_n; ++k) {
    const notification_source_t* f = &s_sources[k];
    if (!f->usada || f->count <= 0) { continue; }
    ++ativas;
    if (melhor < 0 || comes_before(f, &s_sources[melhor])) { melhor = k; }
  }
  if (melhor >= 0 && saida) { copiar(saida, &s_sources[melhor]); }
  destravar();
  if (outras) { *outras = ativas > 0 ? ativas - 1 : 0; }
  return melhor >= 0;
}
