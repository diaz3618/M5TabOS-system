#include "tab5_journal.hpp"

#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "reserva";

// Type "data", own subtype (0x40..0xFE and applications) - see partitions.csv.
#define RESERVA_SUBTIPO ((esp_partition_subtype_t)0x99)

static const esp_partition_t* s_part = nullptr;
static journal_t              s_r = { };
static SemaphoreHandle_t      s_trava = nullptr;

static bool p_read(void* ctx, uint32_t pos, void* buf, uint32_t n)
{
  return esp_partition_read((const esp_partition_t*)ctx, pos, buf, n) == ESP_OK;
}
static bool partition_write(void* ctx, uint32_t pos, const void* buf, uint32_t n)
{
  return esp_partition_write((const esp_partition_t*)ctx, pos, buf, n) == ESP_OK;
}
static bool p_erase(void* ctx, uint32_t pos, uint32_t n)
{
  return esp_partition_erase_range((const esp_partition_t*)ctx, pos, n) == ESP_OK;
}
// The entire record (up to tens of KB - a NodeDB of Mesh) goes to PSRAM.
static void* p_alocar(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void  p_liberar(void* p) { heap_caps_free(p); }

bool tab5_journal_init(void)
{
  if (s_r.ready) { return true; }
  if (!s_trava) { s_trava = xSemaphoreCreateMutex(); }
  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, RESERVA_SUBTIPO, "reserva");
  if (!s_part || !s_trava) {
    ESP_LOGE(TAG, "sem a particao \"reserva\" - grave a tabela de particoes nova (./flash.sh)");
    return false;
  }
  const journal_storage_t m = { p_read, partition_write, p_erase, p_alocar, p_liberar,
                             (void*)s_part, (uint32_t)(s_part->size / JOURNAL_SECTOR * JOURNAL_SECTOR) };
  xSemaphoreTake(s_trava, portMAX_DELAY);
  const bool ok = journal_open(&s_r, &m);
  xSemaphoreGive(s_trava);
  if (!ok) { ESP_LOGE(TAG, "particao ilegivel"); return false; }
  if (s_r.pending) {
    ESP_LOGW(TAG, "%u gravacao(oes) de um pendrive anterior esperando o cartao%s",
             (unsigned)s_r.pending, s_r.dirty ? " (uma ficou pela half e foi descartada)" : "");
  }
  return true;
}

bool tab5_journal_write(const journal_entry_t* reg)
{
  if (!s_r.ready) { return false; }
  xSemaphoreTake(s_trava, portMAX_DELAY);
  const bool ok = journal_write(&s_r, reg);
  xSemaphoreGive(s_trava);
  return ok;
}

bool tab5_journal_pending(void) { return s_r.ready && s_r.pending > 0; }

int tab5_journal_apply(journal_apply_fn fn, void* ctx)
{
  if (!s_r.ready) { return 0; }
  xSemaphoreTake(s_trava, portMAX_DELAY);
  const int n = journal_apply(&s_r, fn, ctx);
  xSemaphoreGive(s_trava);
  return n;
}

void tab5_journal_usage(uint32_t* usado, uint32_t* total, uint32_t* pending)
{
  if (usado)     { *usado = s_r.end; }
  if (total)     { *total = s_r.storage.size; }
  if (pending) { *pending = s_r.pending; }
}
