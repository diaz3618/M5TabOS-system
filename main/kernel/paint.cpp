#include "paint.hpp"
#include "app.h"

#include <string.h>

void pintura_iniciar(pintura_t* p)
{
  // A ordem importa: consultamos sistema_area_foi_limpa() SEMPRE, mesmo que ja
  // saibamos que a passada e completa, porque ela e valida apenas durante esta
  // chamada de desenhar() - nao da para adiar a leitura.
  const bool limpou = sistema_area_foi_limpa();
  p->complete = limpou || p->pedido || !p->ja_desenhou;
  p->pedido      = false;
  p->ja_desenhou = true;
}

void pintura_pedir(pintura_t* p) { p->pedido = true; }

bool pintura_mudou_i(const pintura_t* p, int* mem, int agora)
{
  if (p->complete || *mem != agora) { *mem = agora; return true; }
  return false;
}

bool pintura_mudou_s(const pintura_t* p, char* mem, size_t tam, const char* agora)
{
  if (p->complete || strncmp(mem, agora, tam - 1) != 0) {
    snprintf(mem, tam, "%s", agora);
    return true;
  }
  return false;
}
