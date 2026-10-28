#include "tab5_owners.hpp"

// Small fixed table: the whole system has only a few dozen tasks, and only
// registered tasks are stored (IDF tasks stay out and default to SYSTEM).
#define OWNERS_MAX 32

typedef struct {
  TaskHandle_t h;
  owner_type_t  type;
  const char*  owner;
} owner_entry_t;

static owner_entry_t s_entries[OWNERS_MAX];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void tab5_owner_register(TaskHandle_t h, owner_type_t type, const char* owner)
{
  if (!h) { return; }
  taskENTER_CRITICAL(&s_mux);
  int free_space = -1;
  for (int i = 0; i < OWNERS_MAX; ++i) {
    if (s_entries[i].h == h) { free_space = i; break; }
    if (!s_entries[i].h && free_space < 0) { free_space = i; }
  }
  if (free_space >= 0) { s_entries[free_space] = { h, type, owner }; }
  taskEXIT_CRITICAL(&s_mux);
}

void tab5_owner_forget(TaskHandle_t h)
{
  if (!h) { return; }
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < OWNERS_MAX; ++i) {
    if (s_entries[i].h == h) { s_entries[i] = { nullptr, OWNER_SYSTEM, nullptr }; }
  }
  taskEXIT_CRITICAL(&s_mux);
}

owner_type_t tab5_owner_of(TaskHandle_t h, const char** owner)
{
  owner_type_t type = OWNER_SYSTEM;
  const char* d = nullptr;
  taskENTER_CRITICAL(&s_mux);
  for (int i = 0; i < OWNERS_MAX; ++i) {
    if (s_entries[i].h && s_entries[i].h == h) {
      type = s_entries[i].type;
      d = s_entries[i].owner;
      break;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
  if (owner) { *owner = d; }
  return type;
}
