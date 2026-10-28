// ANIMACOES DA TELA DE BLOQUEIO.
//
// Cada vez que a tela de bloqueio aparece, uma animacao diferente do conjunto
// e sorteada e fica rodando POR TRAS dos textos ate o desbloqueio.
//
// Regras que tornam isto leve:
//   - o estado de todas as animacoes divide uma unica uniao estatica
//     (~2 KB); so a sorteada esta viva;
//   - nada de repintar a tela: cada objeto apaga so a propria caixa (a antiga
//     e a nova numa so) e se desenha de novo. Todas as formas sao sem
//     antialiasing, entao desenhar duas vezes no mesmo lugar e inofensivo;
//   - caixas que se sobrepoem viram uma so (nada e apagado duas vezes);
//   - o quadro sai no ritmo de quadro do sistema (kernel/frame_pacing.hpp), como o
//     de qualquer app periodico, e anda o sistema_quadro_dt() dele;
//   - o fundo (linhas abstratas) e uma imagem pintada uma vez por bloqueio
//     (PSRAM, so enquanto bloqueado): "apagar" uma caixa e copiar o pedaco
//     dela, o mesmo custo de pintar preto;
//   - o texto (hora, data, aviso, dica) fica na frente: o shell o redesenha,
//     recortado, so onde a animacao tocou.
//
// Chamar so da tarefa de interface, de dentro de desenhar_conteudo().

#pragma once

#include <M5Unified.h>
#include "app.h"

typedef struct {
  int16_t x, y, w, h;
} bq_ret_t;

// Escolhe a animacao da proxima exibicao (nunca a mesma duas vezes seguidas).
void bq_anim_sortear(void);

// O periodo pedido enquanto bloqueado (~30 q/s). O sistema o arredonda para
// varreduras inteiras do painel (2, em qualquer clock).
static const uint32_t BQ_ANIM_PERIODO_MS = 33;

// Devolve a memoria do fundo (desbloqueio, repouso). A proxima exibicao
// pinta outro.
void bq_anim_soltar(void);

// Comeca a animacao sorteada numa area recem-limpa (preta).
void bq_anim_iniciar(const area_t& a);

// Avanca e desenha um quadro. 'extras' sao retangulos que o shell acabou de
// precisar apagar (texto que mudou): a animacao os apaga e se refaz neles.
// 'caixas' sao as caixas dos textos; para cada uma, 'clips' devolve a area (ja
// apagada, w == 0 se a animacao nao tocou) onde o texto deve ser repintado -
// UMA vez, recortado nela.
void bq_anim_quadro(lgfx::LovyanGFX& d, const bq_ret_t* extras, int n_extras,
                    const bq_ret_t* caixas, int n_caixas, bq_ret_t* clips);

// PAPEL DE PAREDE DA HOME: um dos desenhos abstratos do fundo (estilo 0..4:
// fios, ondas, teia, raios, circulos - a ordem de SETTINGS_WALLPAPER_*), pintado
// na area 'a'. A mesma semente da sempre o mesmo desenho. false = sem memoria
// ou estilo invalido (a area fica como estava - preta).
bool bq_fundo_parede(lgfx::LovyanGFX& d, const area_t& a, int estilo, uint32_t semente);
// O papel de parede pronto fica guardado (PSRAM, o tamanho da area) enquanto
// estilo, semente e area forem os mesmos - a home volta sem gera-lo de novo.
// Soltar: ao abrir um app e no repouso (o kernel chama).
void bq_parede_soltar(void);
