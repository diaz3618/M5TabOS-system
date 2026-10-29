// NOTIFICATIONS - a system service, not an application patch.
//
// ------------------------------------------------------------------------
// WHY THIS EXISTS
// ------------------------------------------------------------------------
// The status bar used to call tab5_mesh_nao_lidas() directly. It worked, but
// made the SYSTEM know one SERVICE by name. A second app needing to announce
// something—a low-battery alarm, a completed download, or a GPS that lost its
// fix—would have to be wired into the same place with another special case.
// Eventually changing the status bar would mean changing five modules.
//
// Here the relationship is inverted: whoever has news ANNOUNCES it, and the
// system only asks "is there anything?" without knowing who produced it.
// tab5_mesh disappears fromsistema.cpp.
//
// ------------------------------------------------------------------------
// THE MODEL: ONE SOURCE, ONE STATE
// ------------------------------------------------------------------------
// Each service (or app) has ONE registered source and rewrites its state when
// needed. Usage guidance for publishers is in docs/WRITING_APPS.md, section 8.2.
// This is not an event queue; it is CURRENT STATE—"I have three messages,
// one of them direct." That is intentional:
//
// - a queue grows without limit when nobody consumes it; state does not;
// - "three messages" is more useful than three "one message" notices;
// - the producer knows how to aggregate better than the display layer.
//
// ------------------------------------------------------------------------
// COMPETITION
// ------------------------------------------------------------------------
// notif_publicar() can be called from ANY task—that is the point, since the
// service, not the interface, owns the news. The interface task performs the
// reads. The module protects itself.
//
// What it does NOT do: draw. Notifications do not know what a screen is.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define NOTIF_MAX_FONTES  8
#define NOTIF_RESUMO      32

// Severity. The bar displays the WORST active source—red outranks blue,
// because something addressed to you is not the same as passing traffic.
typedef enum {
  NOTIF_ESTADO = 0,   // activity in progress ("recording a track") - shown only when nothing else needs saying
  NOTIF_INFO,         // something is happening; it requires no action
  NOTIF_ATENCAO,      // addressed to you, or likely to cause trouble soon
  NOTIF_URGENTE,      // requires action: battery low, card full
} notification_level_t;

// Register a source and return its identifier, or -1 if no slot is available.
// IDEMPOTENT BY NAME: register of new one name that already exists
// returns the SAME vacancy, WITH THE CONDITION IT HAD. (Before the vacancy returned
// reset - and the Mesh, which registers itself again every time the service goes up
// through another path erased its own notice before anyone read it. The caller
// that wants to clear it publishes 0; registration does not decide that.) Mesh
// registers whenever the service starts, including after each Grove probe.
// Previously each start consumed a new slot; by the eighth there was no slot,
// s_notif became -1, and every later message arrived without a notice until reboot.
//
// 'name' needs to survive in the registry (literal or static): we keep the
// pointer, not a copy. This is the same contract as app_t.name.
int notification_registrar(const char* name);

// Publish the source's CURRENT state. A count of 0 clears the notice.
// 'summary' may be nullptr when there is no useful text.
void notification_publicar(int id, int count, notification_level_t level, const char* summary);

// WHICH APP OPENS when the notice is touched in central of notices (kernel/shell).
// Without an association, the app with the SAME NAME as the source is used—the
// background (background.h), whose name is already the of app. 'app_name' follows the contract of
// 'name': literal or static. nullptr = the notice opens nothing.
void notification_associar_app(int id, const char* app_name);

// Shortcut for "I have nothing more to say."
static inline void notification_limpar(int id) { notification_publicar(id, 0, NOTIF_INFO, nullptr); }

// Turns on/off the BEEP of this source, and SO of this
// It does not affect
// count/level/summary: the source continues publishing and appearing in bar
// and in list of actives exactly the same, only for of count for the beep (see
// notif_houve_novidade_sonora() below). Who records the preference between
// restarts and the CALLER (see app_trilha/trilha_background.cpp and settings.h) - this module only
// stores the state IN RAM, in the same way as it stores count/level.
void notification_silenciar(int id, bool silenciar);

// ---- reading, for those who draw -------------------------------------------

// Changes whenever a publication changes anything. The bar compares it with
// the last version it drew, avoiding needless repainting and ensuring news
// appears immediately instead of at the next 1% battery change.
uint32_t notification_versao(void);

typedef struct {
  const char*   name;
  const char*   app;          // name of app that the warning opens (nullptr = none)
  int           count;
  notification_level_t level;
  char          summary[NOTIF_RESUMO];
} notification_item_t;

// THE FEATURED FONT - the one the bar shows. And the greatest of
// level among ACTIVE ones (count > 0); in a draw, the one that changed last.
// 'other' gets how many active sources are out. false = nothing active.
//
// Replaces the old "N notices" with several active sources: it ADDED things
// values that should not be added (Trilha points + Mesh messages): with a track
// recording, a new message became "348 notices" instead of "1 in LongFast",
// and disappeared in the number. Copy instead of return pointer: the table changes the
// any time, in another task.
bool notification_destaque(notification_item_t* saida, int* outras);

// ALL ACTIVE, in order in which the bar shows them: highlight first
// (most serious; in a tie, the most recent) and the rest according to the same rule. Return
// how many you copied (up to 'max'). And what does the central of notices and the screen of
// lock list - with more than one app warning, one only highlighted and one "+N"
// hid precisely what the user wanted to know: WHOSE.
int notification_listar(notification_item_t* saida, int max);

// ---- reading, for those who play sound -------------------------------------------

// True if any NON-SILENCED source increased its count since the LAST call to
// this specific function. This is the ONLY question the beep player
// (kernel/core.cpp) needs to ask; it need not know how many sources exist or
// their names. Call it at the SAME time interval (currently every BARRA_MS),
// which defines "since the last time."
bool notification_houve_novidade_sonora(void);
