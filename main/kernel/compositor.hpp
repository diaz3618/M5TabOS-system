// COMPOSITOR - a base of screen of sistema (COMPOSITOR.md).
//
// MODO DIRETO (o "direct mode" of LVGL com duas paginas). Todo drawing cai
// DIRETO in pagina of tras of panel - nao existe mais uma terceira imagem
// no middle. O objeto of drawing (kernel/virtual_screen) anota cada rectangle
// touched (comp_invalidar), e o frame tem tres tempos:
//
//   1. ABRIR (preguicoso, no first drawing of iteration - comp_abrir):
//      espera a troca of frame anterior terminar (so entao a pagina of tras
//      esta livre). As areas que o frame anterior desenhou ficam PENDENTES:
//      cada uma vem of frente so when algo for write ou read nela
//      (comp_preparar), e o resto on apresentar. A que uma copia of DMA
//      sobrescreve inteira (rolagem) nunca e copiada.
//   2. DESENHAR in pagina of tras, como sempre.
//   3. APRESENTAR (comp_apresentar, uma vez por iteration): devolve on cache as
//      lines desenhadas e pede a troca. NAO espera - quem espera e o
//      next ABRIR.
//
// Cada pixel mudado passa 3 vezes pela PSRAM (a CPU escreve; a sincronia le e
// escreve uma vez), e nao 5 como com a screen virtual. E sem a screen virtual
// sobram 1,84 MB of PSRAM.
//
// Coordenadas fisicas (retrato). Chamar so of tarefa of interface.

#pragma once

#include <M5Unified.h>
#include "tab5_frame.hpp"

// Marca one rectangle como drawn neste frame.
void comp_invalidar(const tab5_rect_t& r);

// Pede a screen inteira no next frame. Pode ser chamada of QUALQUER tarefa
// (o console usa): so arma one request que a interface atende on apresentar.
void comp_invalidar_tudo(void);

// Abre o frame, se ainda nao estiver open, e devolve a pagina of tras
// (onde draw). nullptr = sem panel (sleep, troca of clock).
void* comp_abrir(void);

// Antes of write OU LER 'r' in pagina of tras: traz of frente o que o
// frame anterior changed ali e a of tras ainda nao tem (sincronia preguicosa).
// O rastreador chama isto sozinho; ninguem mais precisa.
void comp_preparar(const tab5_rect_t& r);

// O mesmo, para quem vai write 'r' INTEIRO e opaco (fillRect, copia of
// DMA): o que estava pending dentro of 'r' e descartado sem copiar - ia ser
// sobrescrito. So o que stays fora of 'r' continua pending.
void comp_cobrir(const tab5_rect_t& r);

// O rectangle 'src' ja foi drawn NESTE frame? (A rolagem so copia of
// pagina of frente o que a frente tem igual a of tras.)
bool comp_desenhado_neste_quadro(const tab5_rect_t& src);

// Leva o rectangle 'r' of pagina of FRENTE, a partir of (ox, oy), para o
// mesmo tamanho em 'r' in of tras, por DMA. Abre o frame. false = nao deu
// (quem chamou faz pela CPU).
bool comp_copiar_da_frente(const tab5_rect_t& r, int ox, int oy);

// Leva o rectangle 'r' of uma imagem 720x1280 RGB565 (a surface of DMA
// of one app - camera) para a pagina of tras, por DMA, e o marca.
bool comp_copiar_imagem(const void* imagem, const tab5_rect_t& r);

// Apresenta o frame, se houver algo drawn (false = nada a fazer, ou sem
// panel - nesse caso o frame e descartado).
bool comp_apresentar(void);

// Ha algo drawn esperando apresentar?
bool comp_tem_sujeira(void);

// Espera a troca requested pelo ultimo frame acontecer of fato. Para quem
// precisa of frame NA TELA antes of follow (acender a luz on wake).
void comp_esperar_troca(void);

// O PAINEL E NOVO (boot, sleep, troca of clock): as duas paginas
// nasceram sem nada nosso, e nao ha of onde synchronize. Devolve true UMA vez
// por panel novo - o core responde com one redraw complete. Chamar so com
// o panel no ar (fora of sleep).
bool comp_painel_novo(void);

// Conta quantas vezes a lista of areas foi consumida. Quem guarda "ja
// invalidei isto" usa para saber que a lista e nova.
uint32_t comp_rodada(void);

// Diagnostico: uptime (us) of ultimo frame montado e quantos pixels ele levou.
void comp_ultimo_quadro(int64_t* when_us, uint32_t* px);

// ---------------------------------------------------------------------------
// MEDIDA - o custo of cada frame, para comparar antes e depois of mexer.
// ---------------------------------------------------------------------------
// Tudo ACUMULADO since o boot e nunca zerado: o console e o monitor leem on
// mesmo time, e cada one guarda a sua foto e faz a diferenca. Assim nenhum
// leitor apaga a window of outro.
//
// Histograma of custo of VOLTA que gerou frame (of wake of interface ate
// a troca requested), em PERIODOS DO PAINEL: coube em 1 varredura, em 2, em 3,
// ou mais. E o numero que decide a fluidez - a iteration que passa of 1 periodo
// deixa o frame anterior in screen mais uma varredura.
#define COMP_FAIXAS 4

typedef struct {
  uint32_t frames;                 // quadros apresentados
  uint32_t cheios;                // quadros que changed >= 90% of screen
  uint32_t estouros;              // vezes que a lista of areas encheu
  uint64_t px_frame;             // pixels desenhados (areas apresentadas)
  uint64_t px_sinc;               // pixels of sincronia (frente -> tras)
  uint64_t px_rolar_dma;          // pixels of rolagem copiados por DMA
  uint64_t us_copia;              // time locked em copia por DMA (todas)
  uint64_t us_espera;             // time esperando a troca of frame anterior
  uint64_t us_drawing;            // CPU desenhando (informado pelo core, sem as copias)
  uint32_t n_drawing;             // passes of drawing medidas
  uint64_t us_drawing_complete;   // ...so as passes COMPLETAS (open app, home)
  uint32_t n_drawing_complete;
  uint64_t us_iteration;              // voltas inteiras que geraram frame
  uint32_t voltas[COMP_FAIXAS];   // histograma of custo dessas voltas, em periodos
  uint64_t px_rolados;            // rolagem total (hal/tab5_screen: DMA ou CPU)
  uint64_t us_rolar;
} comp_estat_t;

void comp_estat(comp_estat_t* out);

// O core mede a pass of drawing entre comp_marcar_desenho() e
// comp_contar_desenho(us); o time que o proprio compositor gastou nesse middle
// (open, synchronize, copiar) e descontado - "drawing" e so a CPU pintando.
void comp_marcar_desenho(void);
// 'complete' = a pass redesenhou a screen inteira (open/close app).
// Devolve o time of CPU of pass (us, sem o time of compositor).
uint32_t comp_contar_desenho(uint32_t us, bool complete);
void comp_contar_volta(uint32_t us);
