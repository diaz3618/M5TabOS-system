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
  const char*   nome;
  const char*   app;                    // ver notif_associar_app()
  int           quantidade;
  notif_nivel_t nivel;
  char          resumo[NOTIF_RESUMO];
  bool          usada;
  bool          silenciada;             // ver notif_silenciar()
  int           quantidade_no_ultimo_beep;  // ver notif_houve_novidade_sonora()
  uint32_t      mudou_em;               // s_versao da ultima mudanca (desempate do destaque)
} fonte_t;

static fonte_t          s_fontes[NOTIF_MAX_FONTES];
static int              s_n = 0;
static uint32_t         s_versao = 0;
static SemaphoreHandle_t s_trava = nullptr;

// Mutex criado na PRIMEIRA chamada, e nao num notif_iniciar() - assim o
// modulo nao impoe ordem de inicializacao a ninguem: um servico que suba
// antes do sistema pode registrar-se sem que exista uma "fase 0" para manter
// em dia. Mecanismo e risco documentados em tab5_mutex.hpp; a razao de ser
// seguro AQUI especificamente e a mesma de sempre: o primeiro registro
// acontece antes de existirem duas tarefas disputando - servicos registram
// ao subir, um de cada vez, a partir da mesma tarefa.
static inline void travar(void)    { tab5_mutex_travar(&s_trava); }
static inline void destravar(void) { tab5_mutex_destravar(&s_trava); }

int notif_registrar(const char* nome)
{
  if (!nome) { nome = "?"; }
  travar();
  int id = -1;
  // Mesmo nome = mesma fonte: a vaga volta, zerada (ver o .hpp).
  for (int i = 0; i < s_n; ++i) {
    if (s_fontes[i].usada && strcmp(s_fontes[i].nome, nome) == 0) { id = i; break; }
  }
  if (id >= 0) {
    // A VAGA VOLTA COM O ESTADO DELA (ver o .h). Zerar aqui era o aviso do
    // Mesh sumindo a cada vez que o servico subia de novo por outra via.
    destravar();
    return id;
  }
  if (s_n < NOTIF_MAX_FONTES) {
    id = s_n++;
    memset(&s_fontes[id], 0, sizeof(s_fontes[id]));
    s_fontes[id].nome  = nome;
    s_fontes[id].app   = nome;   // padrao: o app de mesmo nome (notif_associar_app)
    s_fontes[id].usada = true;
  }
  destravar();

  if (id < 0) { ESP_LOGE(TAG, "sem vaga para '%s' (limite %d)", nome, NOTIF_MAX_FONTES); }
  else        { ESP_LOGI(TAG, "'%s' pode mandar notificacoes (vaga %d)", nome, id); }
  return id;
}

void notif_publicar(int id, int quantidade, notif_nivel_t nivel, const char* resumo)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  if (quantidade < 0) { quantidade = 0; }

  bool mudou = false;
  travar();
  fonte_t* f = &s_fontes[id];
  if (f->usada) {
    // Versao so muda quando algo REALMENTE mudou. Sem esta comparacao, um
    // servico que republica o mesmo estado a cada segundo faria a barra de
    // status repintar a cada segundo - o piscar de volta, por um caminho novo.
    mudou = (f->quantidade != quantidade)
         || (f->nivel != nivel)
         || (strncmp(f->resumo, resumo ? resumo : "", NOTIF_RESUMO) != 0);
    if (mudou) {
      f->quantidade = quantidade;
      f->nivel      = nivel;
      snprintf(f->resumo, sizeof(f->resumo), "%s", resumo ? resumo : "");
      ++s_versao;
      f->mudou_em = s_versao;
    }
  }
  destravar();

  // PRIMEIRO PRODUTOR ATIVO DA QUEUE DE EVENTOS - Etapa 2 do ARCHITECTURE.md
  // . "mudou" e conferido FORA da secao travada de proposito:
  // sistema_acordar_interface() so poe um evento numa queue diferente e nunca
  // bloqueia, mas nao ha motivo para segurar o mutex deste modulo emprestado
  // por outro (evitar qualquer ordem de trava entre os dois, mesmo que hoje
  // nenhum dos dois pudesse causar problema). Mesh e Trilha (os unicos dois
  // servicos que chamam notif_publicar() hoje) ganham isto de graca, sem
  // precisar saber que esta queue existe.
  if (mudou) { sistema_acordar_interface(); }
}

void notif_silenciar(int id, bool silenciar)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  travar();
  s_fontes[id].silenciada = silenciar;
  destravar();
}

void notif_associar_app(int id, const char* app_nome)
{
  if (id < 0 || id >= NOTIF_MAX_FONTES) { return; }
  travar();
  if (s_fontes[id].app != app_nome) { s_fontes[id].app = app_nome; ++s_versao; }
  destravar();
}

uint32_t notif_versao(void)
{
  travar();
  const uint32_t v = s_versao;
  destravar();
  return v;
}

bool notif_houve_novidade_sonora(void)
{
  travar();
  bool tocar = false;
  for (int i = 0; i < s_n; ++i) {
    fonte_t* f = &s_fontes[i];
    if (!f->usada) { continue; }
    // Compara SEMPRE contra o ultimo valor visto por ESTA funcao - nao o
    // total agregado (isso viveu em kernel/core.cpp, e nao
    // dava para saber qual fonte tinha mudado). Atualiza o "ultimo visto"
    // de toda fonte usada, silenciada ou nao: assim, se o usuario religar o
    // som de uma fonte que subiu enquanto estava muda, isso NAO conta como
    // novidade retroativa - so o que subir DAQUI PARA FRENTE toca.
    if (f->quantidade > f->quantidade_no_ultimo_beep && !f->silenciada) {
      tocar = true;
    }
    f->quantidade_no_ultimo_beep = f->quantidade;
  }
  destravar();
  return tocar;
}

// 'a' vem antes de 'b' na barra? Mais grave primeiro; no empate, a mais recente.
static bool antes(const fonte_t* a, const fonte_t* b)
{
  if (a->nivel != b->nivel) { return a->nivel > b->nivel; }
  return a->mudou_em > b->mudou_em;
}

static void copiar(notif_item_t* saida, const fonte_t* f)
{
  saida->nome       = f->nome;
  saida->app        = f->app;
  saida->quantidade = f->quantidade;
  saida->nivel      = f->nivel;
  snprintf(saida->resumo, sizeof(saida->resumo), "%s", f->resumo);
}

int notif_listar(notif_item_t* saida, int max)
{
  if (!saida || max <= 0) { return 0; }
  int ordem[NOTIF_MAX_FONTES];
  int n = 0;
  travar();
  for (int k = 0; k < s_n; ++k) {
    const fonte_t* f = &s_fontes[k];
    if (!f->usada || f->quantidade <= 0) { continue; }
    // Insercao ordenada: oito fontes no maximo, nao vale um qsort.
    int j = n++;
    while (j > 0 && antes(f, &s_fontes[ordem[j - 1]])) { ordem[j] = ordem[j - 1]; --j; }
    ordem[j] = k;
  }
  if (n > max) { n = max; }
  for (int i = 0; i < n; ++i) { copiar(&saida[i], &s_fontes[ordem[i]]); }
  destravar();
  return n;
}

bool notif_destaque(notif_item_t* saida, int* outras)
{
  travar();
  int melhor = -1, ativas = 0;
  for (int k = 0; k < s_n; ++k) {
    const fonte_t* f = &s_fontes[k];
    if (!f->usada || f->quantidade <= 0) { continue; }
    ++ativas;
    if (melhor < 0 || antes(f, &s_fontes[melhor])) { melhor = k; }
  }
  if (melhor >= 0 && saida) { copiar(saida, &s_fontes[melhor]); }
  destravar();
  if (outras) { *outras = ativas > 0 ? ativas - 1 : 0; }
  return melhor >= 0;
}
