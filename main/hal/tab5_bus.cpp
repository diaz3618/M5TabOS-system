#include "tab5_bus.hpp"

#include "hal/axi_icm_ll.h"
#include "esp_log.h"

static const char* TAG = "bus";

// 0..15 in both fields; the factory default is 0 for every master.
//
// Deliberately 8, not 15. The ORDER among masters matters, not the magnitude;
// the screen only needs to rank above the CPU. Headroom leaves space to put
// something between them later (audio DMA, for example) without demoting it.
#define SCREEN_PRIORITY  8
#define DEFAULT_PRIORITY 0

static bool s_enabled = false;

void tab5_bus_prioritize_screen(bool enable)
{
  const uint32_t p = enable ? SCREEN_PRIORITY : DEFAULT_PRIORITY;

  // ARBITRATION priority: who wins when two masters request the bus together.
  // This is the field that decides the contest.
  AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst1_priority = p;
  AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst2_priority = p;

  // ARQOS for the READ channel. Arbitration chooses who enters; QoS tells the
  // memory controller how much that request is worth.
  //
  // Read only (files), not write: the panel only READS the framebuffer. Changing
  // the write channel would not help the screen and would steal writer bandwidth.
  axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 0, p);
  axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 0, p);

  s_enabled = enable;
  ESP_LOGI(TAG, "screen memory-access priority: %s (gdma=%u, cache=%u)",
           enable ? "HIGH (prevents blue flicker)" : "default",
           (unsigned)AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst1_priority,
           (unsigned)AXI_ICM.mst_arb_priority_reg0.reg_cache_priority);
}

bool tab5_bus_is_prioritized(void) { return s_enabled; }
