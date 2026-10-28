#include "tab5_bus.hpp"

#include "hal/axi_icm_ll.h"
#include "esp_log.h"

static const char* TAG = "barramento";

// 0..15 nos dois campos, com 0 de fabrica para todo mundo.
//
// 8 e nao 15 de proposito. O que importa e a ORDEM entre os mestres, e nao a
// magnitude: basta a tela estar acima da CPU para vencer a disputa. Deixando
// folga por cima, sobra faixa para um dia colocar algo entre os dois (o DMA do
// audio, por exemplo) sem ter que rebaixar a tela.
#define PRIO_TELA   8
#define PRIO_PADRAO 0

static bool s_ligado = false;

void tab5_barramento_priorizar_tela(bool ligar)
{
  const uint32_t p = ligar ? PRIO_TELA : PRIO_PADRAO;

  // Prioridade de ARBITRAGEM: quem ganha o barramento quando dois pedem junto.
  // E este o campo que decide a disputa.
  AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst1_priority = p;
  AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst2_priority = p;

  // ARQOS do canal de LEITURA. Complementar: a arbitragem escolhe quem entra,
  // o QoS diz o quanto o pedido dele vale para o controlador de memoria.
  //
  // So leitura (arqos), nao escrita: o painel apenas LE o framebuffer. Mexer no
  // canal de escrita nao ajudaria a tela e tiraria banda de quem escreve.
  axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 0, p);
  axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 0, p);

  s_ligado = ligar;
  ESP_LOGI(TAG, "prioridade da tela no acesso a memoria: %s (gdma=%u, cache=%u)",
           ligar ? "ALTA (contra a piscada azul)" : "padrao",
           (unsigned)AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst1_priority,
           (unsigned)AXI_ICM.mst_arb_priority_reg0.reg_cache_priority);
}

bool tab5_barramento_priorizado(void) { return s_ligado; }
