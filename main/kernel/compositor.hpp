// COMPOSITOR - a base de tela do sistema (COMPOSITOR.md).
//
// MODO DIRETO (o "direct mode" da LVGL com duas paginas). Todo desenho cai
// DIRETO na pagina de tras do painel - nao existe mais uma terceira imagem
// no meio. O objeto de desenho (kernel/virtual_screen) anota cada retangulo
// tocado (comp_invalidar), e o quadro tem tres tempos:
//
//   1. ABRIR (preguicoso, no primeiro desenho da volta - comp_abrir):
//      espera a troca do quadro anterior terminar (so entao a pagina de tras
//      esta livre). As areas que o quadro anterior desenhou ficam PENDENTES:
//      cada uma vem da frente so quando algo for escrever ou ler nela
//      (comp_preparar), e o resto ao apresentar. A que uma copia de DMA
//      sobrescreve inteira (rolagem) nunca e copiada.
//   2. DESENHAR na pagina de tras, como sempre.
//   3. APRESENTAR (comp_apresentar, uma vez por volta): devolve ao cache as
//      linhas desenhadas e pede a troca. NAO espera - quem espera e o
//      proximo ABRIR.
//
// Cada pixel mudado passa 3 vezes pela PSRAM (a CPU escreve; a sincronia le e
// escreve uma vez), e nao 5 como com a tela virtual. E sem a tela virtual
// sobram 1,84 MB de PSRAM.
//
// Coordenadas fisicas (retrato). Chamar so da tarefa de interface.

#pragma once

#include <M5Unified.h>
#include "tab5_frame.hpp"

// Marca um retangulo como desenhado neste quadro.
void comp_invalidar(const tab5_ret_t& r);

// Pede a tela inteira no proximo quadro. Pode ser chamada de QUALQUER tarefa
// (o console usa): so arma um pedido que a interface atende ao apresentar.
void comp_invalidar_tudo(void);

// Abre o quadro, se ainda nao estiver aberto, e devolve a pagina de tras
// (onde desenhar). nullptr = sem painel (repouso, troca de relogio).
void* comp_abrir(void);

// Antes de escrever OU LER 'r' na pagina de tras: traz da frente o que o
// quadro anterior mudou ali e a de tras ainda nao tem (sincronia preguicosa).
// O rastreador chama isto sozinho; ninguem mais precisa.
void comp_preparar(const tab5_ret_t& r);

// O mesmo, para quem vai escrever 'r' INTEIRO e opaco (fillRect, copia de
// DMA): o que estava pendente dentro de 'r' e descartado sem copiar - ia ser
// sobrescrito. So o que fica fora de 'r' continua pendente.
void comp_cobrir(const tab5_ret_t& r);

// O retangulo 'src' ja foi desenhado NESTE quadro? (A rolagem so copia da
// pagina da frente o que a frente tem igual a de tras.)
bool comp_desenhado_neste_quadro(const tab5_ret_t& src);

// Leva o retangulo 'r' da pagina da FRENTE, a partir de (ox, oy), para o
// mesmo tamanho em 'r' na de tras, por DMA. Abre o quadro. false = nao deu
// (quem chamou faz pela CPU).
bool comp_copiar_da_frente(const tab5_ret_t& r, int ox, int oy);

// Leva o retangulo 'r' de uma imagem 720x1280 RGB565 (a superficie de DMA
// de um app - camera) para a pagina de tras, por DMA, e o marca.
bool comp_copiar_imagem(const void* imagem, const tab5_ret_t& r);

// Apresenta o quadro, se houver algo desenhado (false = nada a fazer, ou sem
// painel - nesse caso o quadro e descartado).
bool comp_apresentar(void);

// Ha algo desenhado esperando apresentar?
bool comp_tem_sujeira(void);

// Espera a troca pedida pelo ultimo quadro acontecer de fato. Para quem
// precisa do quadro NA TELA antes de seguir (acender a luz ao acordar).
void comp_esperar_troca(void);

// O PAINEL E NOVO (boot, repouso, troca de relogio): as duas paginas
// nasceram sem nada nosso, e nao ha de onde sincronizar. Devolve true UMA vez
// por painel novo - o core responde com um redesenho completo. Chamar so com
// o painel no ar (fora do repouso).
bool comp_painel_novo(void);

// Conta quantas vezes a lista de areas foi consumida. Quem guarda "ja
// invalidei isto" usa para saber que a lista e nova.
uint32_t comp_rodada(void);

// Diagnostico: uptime (us) do ultimo frame montado e quantos pixels ele levou.
void comp_ultimo_quadro(int64_t* quando_us, uint32_t* px);

// ---------------------------------------------------------------------------
// MEDIDA - o custo de cada quadro, para comparar antes e depois de mexer.
// ---------------------------------------------------------------------------
// Tudo ACUMULADO desde o boot e nunca zerado: o console e o monitor leem ao
// mesmo tempo, e cada um guarda a sua foto e faz a diferenca. Assim nenhum
// leitor apaga a janela do outro.
//
// Histograma do custo da VOLTA que gerou quadro (do acordar da interface ate
// a troca pedida), em PERIODOS DO PAINEL: coube em 1 varredura, em 2, em 3,
// ou mais. E o numero que decide a fluidez - a volta que passa de 1 periodo
// deixa o quadro anterior na tela mais uma varredura.
#define COMP_FAIXAS 4

typedef struct {
  uint32_t quadros;               // quadros apresentados
  uint32_t cheios;                // quadros que mudaram >= 90% da tela
  uint32_t estouros;              // vezes que a lista de areas encheu
  uint64_t px_quadro;             // pixels desenhados (areas apresentadas)
  uint64_t px_sinc;               // pixels da sincronia (frente -> tras)
  uint64_t px_rolar_dma;          // pixels da rolagem copiados por DMA
  uint64_t us_copia;              // tempo bloqueado em copia por DMA (todas)
  uint64_t us_espera;             // tempo esperando a troca do quadro anterior
  uint64_t us_desenho;            // CPU desenhando (informado pelo core, sem as copias)
  uint32_t n_desenho;             // passadas de desenho medidas
  uint64_t us_desenho_completo;   // ...so as passadas COMPLETAS (abrir app, home)
  uint32_t n_desenho_completo;
  uint64_t us_volta;              // voltas inteiras que geraram quadro
  uint32_t voltas[COMP_FAIXAS];   // histograma do custo dessas voltas, em periodos
  uint64_t px_rolados;            // rolagem total (hal/tab5_screen: DMA ou CPU)
  uint64_t us_rolar;
} comp_estat_t;

void comp_estat(comp_estat_t* out);

// O core mede a passada de desenho entre comp_marcar_desenho() e
// comp_contar_desenho(us); o tempo que o proprio compositor gastou nesse meio
// (abrir, sincronizar, copiar) e descontado - "desenho" e so a CPU pintando.
void comp_marcar_desenho(void);
// 'completo' = a passada redesenhou a tela inteira (abrir/fechar app).
// Devolve o tempo de CPU da passada (us, sem o tempo do compositor).
uint32_t comp_contar_desenho(uint32_t us, bool completo);
void comp_contar_volta(uint32_t us);
