#include "scroll.hpp"
#include "tab5_screen.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void scroll_set(scroll_t* r, uint8_t eixo, uint8_t anchoring)
{
  r->current = r->no_visor = r->max = 0;
  r->anchor = r->press_start = -1;
  r->distance = 0;
  r->eixo = eixo;
  r->anchoring = anchoring;
  r->passo_encaixe = 0;
}

void scroll_reset(scroll_t* r)
{
  r->current = 0;
  r->max = 0;   // consumidor que guarda 'max' entre passes (o Mesh) nao
                // stays com one limit velho of uma screen/conversa anterior no
                // instante entre isto e o next drawing recalcular; quem
                // recalcula 'max' TODO ciclo (o kernel) nem chega a notar.
  r->anchor = -1;
  r->press_start = -1;
  r->distance = 0;
}

bool scroll_drag(scroll_t* r, int coord, bool dentro)
{
  // SOLTA A ANCORA AO SAIR DA ZONA, nao so when o finger levanta of vez -
  // default unificado in consolidacao (as tres listas of Mesh
  // ja faziam assim; o kernel antes so soltava em '!tocando', tolerando o
  // finger passear fora of area util sem largar a anchor - one finger que saisse
  // e voltasse pulava a distancia toda of uma vez, porque a anchor ficava
  // parada no ultimo ponto DENTRO. Soltar on exit evita esse salto, e e o
  // comportamento que ja valia pra maioria das listas deste sistema.
  if (!dentro) { r->anchor = -1; return false; }

  // Primeira sample dentro of zona: so anchor, nao ha 'ciclo anterior'
  // para tirar delta nenhum - evitar isso e o que impede one salto on
  // ENTRAR in zona (o finger pode ter vindo of longe fora dela).
  if (r->anchor < 0) { r->anchor = coord; r->press_start = coord; r->distance = 0; return false; }

  const int dy = coord - r->anchor;
  if (dy == 0) { return false; }
  r->anchor = coord;

  // AFASTAMENTO DA DESCIDA, e nao soma of caminho percorrido - one finger que
  // treme (ou vai e iteration) parado nao pode ser lido como arrasto grande so
  // por ter "andado" muito em vaivem. Mesma logica historica of kernel.
  const int longe = (coord > r->press_start) ? (coord - r->press_start) : (r->press_start - coord);
  if (longe > r->distance) { r->distance = longe; }

  // SINAL: lista comum sobe/desce com o finger (finger pra top = content
  // sobe, por isso subtrai); anchored no end faz o oposto (finger pra bottom
  // revela content mais antigo, por isso soma).
  int nova = (r->anchoring == ROLAGEM_INICIO) ? (r->current - dy) : (r->current + dy);
  if (nova < 0)      { nova = 0; }
  if (nova > r->max) { nova = r->max; }
  if (nova == r->current) { return false; }

  r->current = nova;
  return true;
}

bool scroll_foi_arraste(scroll_t* r)
{
  const bool foi = r->distance > ROLAGEM_ARRASTE_MIN;
  r->distance = 0;
  return foi;
}

bool scroll_limit(scroll_t* r, int total, int window)
{
  int novo_max = total - window;
  if (novo_max < 0) { novo_max = 0; }
  r->max = novo_max;
  if (r->current > r->max) {
    // A lista encolheu (one item foi removido, uma conversa trocou) e a
    // position current became alem of end - puxa of iteration.
    r->current = r->max;
    return true;
  }
  return false;
}

scroll_pass_t scroll_scroll(scroll_t* r, const area_t& window, uint16_t color)
{
  scroll_pass_t p = {};
  p.delta = (r->anchoring == ROLAGEM_INICIO) ? (r->current - r->no_visor)
                                             : (r->no_visor - r->current);
  p.moved = tab5_screen_scroll(window.x, window.y, window.width, window.height, p.delta, color);
  if (p.moved) {
    const int ad = (p.delta > 0) ? p.delta : -p.delta;
    p.exposta = area_t{
      window.x,
      (p.delta > 0) ? (window.y + window.height - ad) : window.y,
      window.width, ad
    };
  }
  // SEMPRE sincroniza, tenha deslocado ou nao: se nao moved, o chamador
  // vai limpar/redesenhar a window inteira por conta propria (cada one of
  // seu jeito - ver o aviso grande no .hpp), e depois disso a pagina
  // visivel TAMBEM passa a refletir 'current', so que por outro caminho.
  r->no_visor = r->current;
  return p;
}

void scroll_barra(lgfx::LovyanGFX& d, const scroll_t* r, const area_t& window,
                   uint16_t color_track, int margem)
{
  if (r->max <= 0) { return; }
  // Gate extra so para ancorada-no-fim: com current==0 o "end" ja esta a
  // vista, a barrinha ali seria ruido (mesma regra que mensagens of Mesh
  // ja tinha antes desta consolidacao).
  if (r->anchoring == ROLAGEM_FIM && r->current <= 0) { return; }

  const int bx  = window.x + window.width - margem;
  const int alt = (window.height * window.height) / (window.height + r->max);
  const int pos = (r->anchoring == ROLAGEM_INICIO) ? r->current : (r->max - r->current);
  const int by  = window.y + ((window.height - alt) * pos) / r->max;

  d.fillRect(bx, window.y, 4, window.height, color_track);
  d.fillRoundRect(bx, by, 4, alt, 2, TFT_DARKGREY);
}
