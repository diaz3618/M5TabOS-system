#include "scroll.hpp"
#include "tab5_screen.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void rolagem_definir(rolagem_t* r, uint8_t eixo, uint8_t ancoragem)
{
  r->atual = r->no_visor = r->max = 0;
  r->ancora = r->descida = -1;
  r->afastamento = 0;
  r->eixo = eixo;
  r->ancoragem = ancoragem;
  r->passo_encaixe = 0;
}

void rolagem_zerar(rolagem_t* r)
{
  r->atual = 0;
  r->max = 0;   // consumidor que guarda 'max' entre passadas (o Mesh) nao
                // fica com um limite velho de uma tela/conversa anterior no
                // instante entre isto e o proximo desenho recalcular; quem
                // recalcula 'max' TODO ciclo (o kernel) nem chega a notar.
  r->ancora = -1;
  r->descida = -1;
  r->afastamento = 0;
}

bool rolagem_arrastar(rolagem_t* r, int coord, bool dentro)
{
  // SOLTA A ANCORA AO SAIR DA ZONA, nao so quando o dedo levanta de vez -
  // padrao unificado na consolidacao (as tres listas do Mesh
  // ja faziam assim; o kernel antes so soltava em '!tocando', tolerando o
  // dedo passear fora da area util sem largar a ancora - um dedo que saisse
  // e voltasse pulava a distancia toda de uma vez, porque a ancora ficava
  // parada no ultimo ponto DENTRO. Soltar ao sair evita esse salto, e e o
  // comportamento que ja valia pra maioria das listas deste sistema.
  if (!dentro) { r->ancora = -1; return false; }

  // Primeira amostra dentro da zona: so ancora, nao ha 'ciclo anterior'
  // para tirar delta nenhum - evitar isso e o que impede um salto ao
  // ENTRAR na zona (o dedo pode ter vindo de longe fora dela).
  if (r->ancora < 0) { r->ancora = coord; r->descida = coord; r->afastamento = 0; return false; }

  const int dy = coord - r->ancora;
  if (dy == 0) { return false; }
  r->ancora = coord;

  // AFASTAMENTO DA DESCIDA, e nao soma do caminho percorrido - um dedo que
  // treme (ou vai e volta) parado nao pode ser lido como arrasto grande so
  // por ter "andado" muito em vaivem. Mesma logica historica do kernel.
  const int longe = (coord > r->descida) ? (coord - r->descida) : (r->descida - coord);
  if (longe > r->afastamento) { r->afastamento = longe; }

  // SINAL: lista comum sobe/desce com o dedo (dedo pra cima = conteudo
  // sobe, por isso subtrai); ancorada no fim faz o oposto (dedo pra baixo
  // revela conteudo mais antigo, por isso soma).
  int nova = (r->ancoragem == ROLAGEM_INICIO) ? (r->atual - dy) : (r->atual + dy);
  if (nova < 0)      { nova = 0; }
  if (nova > r->max) { nova = r->max; }
  if (nova == r->atual) { return false; }

  r->atual = nova;
  return true;
}

bool rolagem_foi_arraste(rolagem_t* r)
{
  const bool foi = r->afastamento > ROLAGEM_ARRASTE_MIN;
  r->afastamento = 0;
  return foi;
}

bool rolagem_limite(rolagem_t* r, int total, int janela)
{
  int novo_max = total - janela;
  if (novo_max < 0) { novo_max = 0; }
  r->max = novo_max;
  if (r->atual > r->max) {
    // A lista encolheu (um item foi removido, uma conversa trocou) e a
    // posicao atual ficou alem do fim - puxa de volta.
    r->atual = r->max;
    return true;
  }
  return false;
}

rolagem_passada_t rolagem_deslocar(rolagem_t* r, const area_t& janela, uint16_t cor)
{
  rolagem_passada_t p = {};
  p.delta = (r->ancoragem == ROLAGEM_INICIO) ? (r->atual - r->no_visor)
                                             : (r->no_visor - r->atual);
  p.deslocou = tab5_tela_rolar(janela.x, janela.y, janela.largura, janela.altura, p.delta, cor);
  if (p.deslocou) {
    const int ad = (p.delta > 0) ? p.delta : -p.delta;
    p.exposta = area_t{
      janela.x,
      (p.delta > 0) ? (janela.y + janela.altura - ad) : janela.y,
      janela.largura, ad
    };
  }
  // SEMPRE sincroniza, tenha deslocado ou nao: se nao deslocou, o chamador
  // vai limpar/redesenhar a janela inteira por conta propria (cada um do
  // seu jeito - ver o aviso grande no .hpp), e depois disso a pagina
  // visivel TAMBEM passa a refletir 'atual', so que por outro caminho.
  r->no_visor = r->atual;
  return p;
}

void rolagem_barra(lgfx::LovyanGFX& d, const rolagem_t* r, const area_t& janela,
                   uint16_t cor_trilha, int margem)
{
  if (r->max <= 0) { return; }
  // Gate extra so para ancorada-no-fim: com atual==0 o "fim" ja esta a
  // vista, a barrinha ali seria ruido (mesma regra que mensagens do Mesh
  // ja tinha antes desta consolidacao).
  if (r->ancoragem == ROLAGEM_FIM && r->atual <= 0) { return; }

  const int bx  = janela.x + janela.largura - margem;
  const int alt = (janela.altura * janela.altura) / (janela.altura + r->max);
  const int pos = (r->ancoragem == ROLAGEM_INICIO) ? r->atual : (r->max - r->atual);
  const int by  = janela.y + ((janela.altura - alt) * pos) / r->max;

  d.fillRect(bx, janela.y, 4, janela.altura, cor_trilha);
  d.fillRoundRect(bx, by, 4, alt, 2, TFT_DARKGREY);
}
