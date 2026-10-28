// NOTIFICATIONS - a system service, not an application patch.
//
// -------------------------------------------------------------------------
// WHY THIS EXISTS
// -------------------------------------------------------------------------
// The status bar used to call tab5_mesh_nao_lidas() directly. It worked, but
// made the SYSTEM know one SERVICE by name. A second app needing to announce
// something—a low-battery alarm, a completed download, or a GPS that lost its
// fix—would have to be wired into the same place with another special case.
// Eventually changing the status bar would mean changing five modules.
//
// Here the relationship is inverted: whoever has news ANNOUNCES it, and the
// system only asks "is there anything?" without knowing who produced it.
// tab5_mesh disappears from sistema.cpp.
//
// -------------------------------------------------------------------------
// THE MODEL: ONE SOURCE, ONE STATE
// -------------------------------------------------------------------------
// Each service (or app) has ONE registered source and rewrites its state when
// needed. Usage guidance for publishers is in WRITING_APPS.md, section 8.2.
// This is not an event queue; it is CURRENT STATE—"I have three messages,
// one of them direct." That is intentional:
//
//   - a queue grows without limit when nobody consumes it; state does not;
//   - "three messages" is more useful than three "one message" notices;
//   - the producer knows how to aggregate better than the display layer.
//
// -------------------------------------------------------------------------
// CONCURRENCY
// -------------------------------------------------------------------------
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
// IDEMPOTENTE PELO NOME: registrar of novo one name que ja existe
// devolve a MESMA vaga, COM O ESTADO QUE ELA TINHA. (Antes a vaga voltava
// zerada - e o Mesh, que se registra of novo a cada vez que o servico sobe
// through another path erased its own notice before anyone read it. A caller
// that wants to clear it publishes 0; registration does not decide that.) Mesh
// registers whenever the service starts, including after each Grove probe.
// Previously each start consumed a new slot; by the eighth there was no slot,
// s_notif became -1, and every later message arrived without a notice until reboot.
//
// 'name' precisa sobreviver on registro (literal ou estatico): guardamos o
// pointer, not a copy. This is the same contract as app_t.name.
int notification_registrar(const char* name);

// Publish the source's CURRENT state. A count of 0 clears the notice.
// 'summary' may be nullptr when there is no useful text.
void notification_publicar(int id, int count, notification_level_t level, const char* summary);

// QUAL APP ABRE when o aviso e touched in central of notices (kernel/shell).
// Without an association, the app with the SAME NAME as the source is used—the
// fundo (background.h), cujo name ja e o of app. 'app_nome' segue o contrato of
// 'name': literal or static. nullptr = the notice opens nothing.
void notification_associar_app(int id, const char* app_name);

// Shortcut for "I have nothing more to say."
static inline void notification_limpar(int id) { notification_publicar(id, 0, NOTIF_INFO, nullptr); }

// Liga/desliga o AVISO SONORO desta fonte, e SO desta
// It does not affect
// count/level/summary: a fonte continua publicando e aparecendo in barra
// e in lista of ativas exatamente igual, so para of count para o beep (ver
// notif_houve_novidade_sonora() abaixo). Quem grava a preferencia entre
// reinicios e o CHAMADOR (ver app_trilha/trilha_background.cpp e settings.h) - este modulo so
// guarda o estado EM RAM, of mesmo jeito que guarda count/level.
void notification_silenciar(int id, bool silenciar);

// ---- leitura, para quem desenha -------------------------------------------

// Changes whenever a publication changes anything. The bar compares it with
// the last version it drew, avoiding needless repainting and ensuring news
// appears immediately instead of at the next 1% battery change.
uint32_t notification_versao(void);

typedef struct {
  const char*   name;
  const char*   app;          // name of app que o aviso abre (nullptr = nenhum)
  int           count;
  notification_level_t level;
  char          summary[NOTIF_RESUMO];
} notification_item_t;

// A FONTE EM DESTAQUE - a que a barra mostra. E a of maior
// level entre as ATIVAS (count > 0); no empate, a que changed por ultimo.
// 'outras' recebe quantas fontes ativas ficaram of fora. false = nada active.
//
// Substitui o antigo "N notices" com varias fontes ativas: ele SOMAVA coisas
// values that should not be added (Trilha points + Mesh messages): with a track
// gravando, uma message nova virava "348 notices" em vez of "1 em LongFast",
// e sumia no numero. Copia em vez of return ponteiro: a tabela muda a
// qualquer momento, noutra tarefa.
bool notification_destaque(notification_item_t* saida, int* outras);

// TODAS AS ATIVAS, in order em que a barra as mostra: o destaque first
// (mais grave; no empate, a mais recente) e o resto in mesma regra. Devolve
// quantas copiou (ate 'max'). E o que a central of notices e a screen of
// bloqueio listam - com mais of one app avisando, one so destaque e one "+N"
// escondiam justamente o que o user queria saber: DE QUEM.
int notification_listar(notification_item_t* saida, int max);

// ---- leitura, para quem toca sound -------------------------------------------

// True if any NON-SILENCED source increased its count since the LAST call to
// this specific function. This is the ONLY question the beep player
// (kernel/core.cpp) needs to ask; it need not know how many sources exist or
// their names. Call it at the SAME time interval (currently every BARRA_MS),
// which defines "since the last time."
bool notification_houve_novidade_sonora(void);
