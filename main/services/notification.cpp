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
  const char*   app;                    // ver notif_associar_app()
  int           count;
  notification_level_t level;
  char          summary[NOTIF_RESUMO];
  bool          usada;
  bool          silenciada;             // ver notif_silenciar()
  int           count_no_ultimo_beep;  // ver notif_houve_novidade_sonora()
  uint32_t      changed_em;               // s_versao of ultima mudanca (desempate of destaque)
} fonte_t;

static fonte_t          s_fontes[NOTIF_MAX_FONTES];
static int              s_n = 0;
static uint32_t         s_versao = 0;
static SemaphoreHandle_t s_trava = nullptr;

// Mutex criado in PRIMEIRA chamada, e nao num notif_iniciar() - assim o
// modulo nao impoe order of inicializacao a ninguem: one servico que suba
// antes of sistema pode registrar-se sem que exista uma "fase 0" para manter
// em dia. Mecanismo e risco documentados em tab5_mutex.hpp; a razao of ser
// seguro AQUI especificamente e a mesma of sempre: o first registro
// acontece antes of existirem duas tarefas disputando - servicos registram
// on raise, one of cada vez, a partir of mesma tarefa.
static inline void lock(void)    { tab5_mutex_lock(&s_trava); }
static inline void destravar(void) { tab5_mutex_destravar(&s_trava); }

int notification_registrar(const char* name)
{
  if (!name) { name = "?"; }
  lock();
  int id = -1;
  // Mesmo name = mesma fonte: a vaga iteration, zerada (ver o .hpp).
  for (int i = 0; i < s_n; ++i) {
    if (s_fontes[i].usada && strcmp(s_fontes[i].name, name) == 0) { id = i; break; }
  }
  if (id >= 0) {
    // A VAGA VOLTA COM O ESTADO DELA (ver o .h). Zerar aqui era o aviso of
    // Mesh sumindo a cada vez que o servico subia of novo por outra via.
    destravar();
    return id;
  }
  if (s_n < NOTIF_MAX_FONTES) {
    id = s_n++;
    memset(&s_fontes[id], 0, sizeof(s_fontes[id]));
    s_fontes[id].name  = name;
    s_fontes[id].app   = name;   // default: o app of mesmo name (notif_associar_app)
    s_fontes[id].usada = true;
  }
  destravar();

  if (id < 0) { ESP_LOGE(TAG, "sem vaga para '%s' (limite %d)", name, NOTIF_MAX_FONTES); }
  else        { ESP_LOGI(TAG, "'%s' pode mandar notificacoes (vaga %d)", name, id); }
  return id;
}

void notification_publicar(int id, int count, notification_level_t level, const char* summary)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  if (count < 0) { count = 0; }

  bool changed = false;
  lock();
  fonte_t* f = &s_fontes[id];
  if (f->usada) {
    // Versao so muda when algo REALMENTE changed. Sem esta comparacao, one
    // servico que republica o mesmo estado a cada segundo faria a barra of
    // status repintar a cada segundo - o piscar of iteration, por one caminho novo.
    changed = (f->count != count)
         || (f->level != level)
         || (strncmp(f->summary, summary ? summary : "", NOTIF_RESUMO) != 0);
    if (changed) {
      f->count = count;
      f->level      = level;
      snprintf(f->summary, sizeof(f->summary), "%s", summary ? summary : "");
      ++s_versao;
      f->changed_em = s_versao;
    }
  }
  destravar();

  // PRIMEIRO PRODUTOR ATIVO DA QUEUE DE EVENTOS - Etapa 2 of ARCHITECTURE.md
  // . "changed" e conferido FORA of secao travada of proposito:
  // sistema_acordar_interface() so poe one evento numa queue diferente e nunca
  // bloqueia, mas nao ha reason para hold o mutex deste modulo emprestado
  // por outro (evitar qualquer order of trava entre os dois, mesmo que hoje
  // nenhum dos dois pudesse causar problema). Mesh e Trilha (os unicos dois
  // servicos que chamam notif_publicar() hoje) ganham isto of graca, sem
  // precisar saber que esta queue existe.
  if (changed) { system_wake_interface(); }
}

void notification_silenciar(int id, bool silenciar)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  lock();
  s_fontes[id].silenciada = silenciar;
  destravar();
}

void notification_associar_app(int id, const char* app_name)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  lock();
  if (s_fontes[id].app != app_name) { s_fontes[id].app = app_name; ++s_versao; }
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
    fonte_t* f = &s_fontes[i];
    if (!f->usada) { continue; }
    // Compara SEMPRE contra o ultimo valor visto por ESTA funcao - nao o
    // total agregado (isso viveu em kernel/core.cpp, e nao
    // dava para saber qual fonte tinha mudado). Atualiza o "ultimo visto"
    // of toda fonte usada, silenciada ou nao: assim, se o user religar o
    // sound of uma fonte que subiu enquanto estava muda, isso NAO conta como
    // novidade retroativa - so o que raise DAQUI PARA FRENTE toca.
    if (f->count > f->count_no_ultimo_beep && !f->silenciada) {
      touch = true;
    }
    f->count_no_ultimo_beep = f->count;
  }
  destravar();
  return touch;
}

// 'a' vem antes of 'b' in barra? Mais grave first; no empate, a mais recente.
static bool antes(const fonte_t* a, const fonte_t* b)
{
  if (a->level != b->level) { return a->level > b->level; }
  return a->changed_em > b->changed_em;
}

static void copiar(notification_item_t* saida, const fonte_t* f)
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
    const fonte_t* f = &s_fontes[k];
    if (!f->usada || f->count <= 0) { continue; }
    // Insercao ordenada: oito fontes no maximum, nao vale one qsort.
    int j = n++;
    while (j > 0 && antes(f, &s_fontes[order[j - 1]])) { order[j] = order[j - 1]; --j; }
    order[j] = k;
  }
  if (n > max) { n = max; }
  for (int i = 0; i < n; ++i) { copiar(&saida[i], &s_fontes[order[i]]); }
  destravar();
  return n;
}

bool notification_destaque(notification_item_t* saida, int* outras)
{
  lock();
  int melhor = -1, ativas = 0;
  for (int k = 0; k < s_n; ++k) {
    const fonte_t* f = &s_fontes[k];
    if (!f->usada || f->count <= 0) { continue; }
    ++ativas;
    if (melhor < 0 || antes(f, &s_fontes[melhor])) { melhor = k; }
  }
  if (melhor >= 0 && saida) { copiar(saida, &s_fontes[melhor]); }
  destravar();
  if (outras) { *outras = ativas > 0 ? ativas - 1 : 0; }
  return melhor >= 0;
}
