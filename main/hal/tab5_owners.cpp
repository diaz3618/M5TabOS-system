#include "tab5_owners.hpp"

// Tabela pequena e fixa: sao poucas dezenas de tarefas no sistema inteiro, e
// so as registradas entram (as do IDF ficam de fora - sao SISTEMA por padrao).
#define DONOS_MAX 32

typedef struct {
  TaskHandle_t h;
  dono_tipo_t  tipo;
  const char*  dono;
} dono_t;

static dono_t       s_d[DONOS_MAX];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void tab5_dono_registrar(TaskHandle_t h, dono_tipo_t tipo, const char* dono)
{
  if (!h) { return; }
  taskENTER_CRITICAL(&s_mux);
  int livre = -1;
  for (int i = 0; i < DONOS_MAX; ++i) {
    if (s_d[i].h == h) { livre = i; break; }
    if (!s_d[i].h && livre < 0) { livre = i; }
  }
  if (livre >= 0) { s_d[livre] = { h, tipo, dono }; }
  taskEXIT_CRITICAL(&s_mux);
}

void tab5_dono_esquecer(TaskHandle_t h)
{
  if (!h) { return; }
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < DONOS_MAX; ++i) {
    if (s_d[i].h == h) { s_d[i] = { nullptr, DONO_SISTEMA, nullptr }; }
  }
  taskEXIT_CRITICAL(&s_mux);
}

dono_tipo_t tab5_dono_de(TaskHandle_t h, const char** dono)
{
  dono_tipo_t tipo = DONO_SISTEMA;
  const char* d = nullptr;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < DONOS_MAX; ++i) {
    if (s_d[i].h && s_d[i].h == h) { tipo = s_d[i].tipo; d = s_d[i].dono; break; }
  }
  taskEXIT_CRITICAL(&s_mux);
  if (dono) { *dono = d; }
  return tipo;
}
