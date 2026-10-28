// ANIMACOES DA TELA DE BLOQUEIO.
//
// Cada vez que a screen of bloqueio aparece, uma animacao diferente of conjunto
// e sorteada e stays running POR TRAS dos textos ate o desbloqueio.
//
// Regras que tornam isto leve:
//   - o estado of todas as animacoes divide uma unica uniao estatica
//     (~2 KB); so a sorteada esta alive;
//   - nada of repintar a screen: cada objeto apaga so a propria caixa (a antiga
//     e a nova numa so) e se desenha of novo. Todas as formas sao sem
//     antialiasing, entao draw duas vezes no mesmo lugar e inofensivo;
//   - caixas que se sobrepoem viram uma so (nada e apagado duas vezes);
//   - o frame exit no ritmo of frame of sistema (kernel/frame_pacing.hpp), como o
//     of qualquer app periodico, e anda o sistema_quadro_dt() dele;
//   - o fundo (lines abstratas) e uma imagem pintada uma vez por bloqueio
//     (PSRAM, so enquanto locked): "erase" uma caixa e copiar o pedaco
//     dela, o mesmo custo of paint preto;
//   - o text (hora, data, aviso, dica) stays in frente: o shell o redesenha,
//     recortado, so onde a animacao tocou.
//
// Chamar so of tarefa of interface, of dentro of desenhar_conteudo().

#pragma once

#include <M5Unified.h>
#include "app.h"

typedef struct {
  int16_t x, y, w, h;
} bq_ret_t;

// Escolhe a animacao of next exibicao (nunca a mesma duas vezes seguidas).
void bq_anim_sortear(void);

// O periodo request enquanto locked (~30 q/s). O sistema o arredonda para
// varreduras inteiras of panel (2, em qualquer clock).
static const uint32_t BQ_ANIM_PERIODO_MS = 33;

// Devolve a memoria of fundo (desbloqueio, sleep). A next exibicao
// pinta outro.
void bq_anim_release(void);

// Comeca a animacao sorteada numa area recem-limpa (preta).
void bq_anim_start(const area_t& a);

// Avanca e desenha one frame. 'extras' sao retangulos que o shell acabou of
// precisar erase (text que changed): a animacao os apaga e se refaz neles.
// 'caixas' sao as caixas dos textos; para cada uma, 'clips' devolve a area (ja
// apagada, w == 0 se a animacao nao tocou) onde o text deve ser repintado -
// UMA vez, recortado nela.
void bq_anim_frame(lgfx::LovyanGFX& d, const bq_ret_t* extras, int n_extras,
                    const bq_ret_t* caixas, int n_caixas, bq_ret_t* clips);

// PAPEL DE PAREDE DA HOME: one dos desenhos abstratos of fundo (estilo 0..4:
// fios, ondas, teia, raios, circulos - a order of SETTINGS_WALLPAPER_*), pintado
// in area 'a'. A mesma semente of sempre o mesmo drawing. false = sem memoria
// ou estilo invalido (a area stays como estava - preta).
bool bq_fundo_parede(lgfx::LovyanGFX& d, const area_t& a, int estilo, uint32_t semente);
// O papel of parede pronto stays guardado (PSRAM, o tamanho of area) enquanto
// estilo, semente e area forem os mesmos - a home iteration sem gera-lo of novo.
// Soltar: on open one app e no sleep (o kernel chama).
void bq_parede_release(void);
