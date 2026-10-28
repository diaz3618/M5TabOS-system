#include "tab5_marks.hpp"

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "soc/rtc.h"
#include "esp_cache.h"
#include "sdmmc_cmd.h"

#define MARCOS_N 64

static DRAM_ATTR tab5_mark_t s_anel[MARCOS_N];
static DRAM_ATTR uint32_t     s_prox = 0;
static DRAM_ATTR portMUX_TYPE s_mux  = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR tab5_mark(const char* what, int32_t value)
{
  const int64_t agora = esp_timer_get_time();
  portENTER_CRITICAL_SAFE(&s_mux);
  tab5_mark_t& m = s_anel[s_prox % MARCOS_N];
  m.us    = agora;
  m.what = what;
  m.value = value;
  s_prox  = s_prox + 1;
  portEXIT_CRITICAL_SAFE(&s_mux);
}

int tab5_marks_between(int64_t from_us, int64_t to_us, tab5_mark_t* out, int max)
{
  tab5_mark_t copia[MARCOS_N];
  uint32_t prox;
  portENTER_CRITICAL(&s_mux);
  prox = s_prox;
  for (int i = 0; i < MARCOS_N; ++i) { copia[i] = s_anel[i]; }
  portEXIT_CRITICAL(&s_mux);

  // Quando ha mais marks na janela que cabem em 'out', ficam os MAIS NOVOS -
  // os que cairam mais perto do atraso. A primeira version guardava os mais
  // antigos e cortava justamente os ultimos milissegundos antes dele.
  const uint32_t total = prox < MARCOS_N ? prox : MARCOS_N;
  int na_janela = 0;
  for (uint32_t k = prox - total; k != prox; ++k) {
    const tab5_mark_t& m = copia[k % MARCOS_N];
    if (m.what && m.us >= from_us && m.us <= to_us) { ++na_janela; }
  }
  int pular = na_janela > max ? na_janela - max : 0;
  int n = 0;
  for (uint32_t k = prox - total; k != prox && n < max; ++k) {
    const tab5_mark_t& m = copia[k % MARCOS_N];
    if (!(m.what && m.us >= from_us && m.us <= to_us)) { continue; }
    if (pular > 0) { --pular; continue; }
    out[n++] = m;
  }
  return n;
}

// O DESVIO DA TROCA DE CLOCK (ver o .hpp). Roda dentro do do_switch() do
// esp_pm, em secao critica e possivelmente com a cache desligada: IRAM, data
// em DRAM, nada alem de anotar e seguir.
extern "C" void __real_rtc_clk_cpu_freq_set_config_fast(const rtc_cpu_freq_config_t* config);

extern "C" void IRAM_ATTR __wrap_rtc_clk_cpu_freq_set_config_fast(const rtc_cpu_freq_config_t* config)
{
  tab5_mark("clock da CPU ->", config ? (int32_t)config->freq_mhz : -1);
  __real_rtc_clk_cpu_freq_set_config_fast(config);
}

// O CARTAO: leitura e escrita de setores, que e DMA do SDMMC. O
// atraso de 9,7 s apareceu sem troca de clock nenhuma, com o Mesh carregando
// nos e conversas do cartao - o proximo suspeito e esse trafego.
extern "C" esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t bloco, size_t n);
extern "C" esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t bloco, size_t n);

extern "C" esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t bloco, size_t n)
{
  tab5_mark("sd: le setores", (int32_t)n);
  return __real_sdmmc_read_sectors(card, dst, bloco, n);
}

extern "C" esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t bloco, size_t n)
{
  tab5_mark("sd: escreve setores", (int32_t)n);
  return __real_sdmmc_write_sectors(card, src, bloco, n);
}

// SINCRONIA DE CACHE GRANDE: escrever de volta ou invalidar dezenas de KB da
// PSRAM ocupa o barramento que o DMA do panel usa. So as grandes (>= 16 KB)
// - as pequenas sao constantes e so encheriam o anel.
extern "C" esp_err_t __real_esp_cache_msync(void* addr, size_t size, int flags);

extern "C" esp_err_t IRAM_ATTR __wrap_esp_cache_msync(void* addr, size_t size, int flags)
{
  if (size >= 16384) { tab5_mark("cache: msync KB", (int32_t)(size / 1024)); }
  return __real_esp_cache_msync(addr, size, flags);
}
