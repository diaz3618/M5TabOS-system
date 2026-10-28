#include "tab5_bmp.hpp"
#include "tab5_sd.hpp"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "bmp";

static const size_t BLOCO = 32 * 1024;

static void u32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void u16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

bool tab5_bmp_salvar(const char* pasta, const char* name, const uint16_t* data, int width, int height,
                     tab5_bmp_filtro_t filtro)
{
  if (!pasta || !name || !data || width <= 0 || height <= 0) { return false; }
  const int64_t t0 = esp_timer_get_time();

  const uint32_t w = (uint32_t)width, h = (uint32_t)height;
  const uint32_t line_bytes = ((w * 2 + 3) / 4) * 4;   // lines of bmp alinhadas em 4 bytes
  const uint32_t data_size   = line_bytes * h;
  const uint32_t cab_size     = 14 + 40 + 12;             // arquivo + info + 3 mascaras

  uint8_t cab[cab_size];
  memset(cab, 0, sizeof(cab));
  cab[0] = 'B'; cab[1] = 'M';
  u32(cab + 2,  cab_size + data_size);
  u32(cab + 10, cab_size);
  u32(cab + 14, 40);
  u32(cab + 18, w);
  u32(cab + 22, h);          // height POSITIVA = lines of bottom para top
  u16(cab + 26, 1);
  u16(cab + 28, 16);
  u32(cab + 30, 3);          // BI_BITFIELDS
  u32(cab + 34, data_size);
  u32(cab + 54, 0xF800);     // R
  u32(cab + 58, 0x07E0);     // G
  u32(cab + 62, 0x001F);     // B

  uint8_t* bloco = (uint8_t*)heap_caps_malloc(BLOCO, MALLOC_CAP_SPIRAM);
  if (!bloco) { ESP_LOGE(TAG, "sem PSRAM para o bloco de escrita"); return false; }
  if (!tab5_sd_mount()) { heap_caps_free(bloco); ESP_LOGE(TAG, "cartao indisponivel"); return false; }

  char arquivo[48];
  snprintf(arquivo, sizeof(arquivo), "%s.bmp", name);
  FILE* f = tab5_sd_open(pasta, arquivo, "wb");
  bool ok = (f != nullptr);
  if (ok) { ok = fwrite(cab, 1, cab_size, f) == cab_size; }

  // Linhas of bottom para top, juntadas no bloco ate ele encher.
  const uint32_t por_bloco = BLOCO / line_bytes;
  for (uint32_t y = 0; ok && y < h; ) {
    uint32_t n = 0;
    for (; n < por_bloco && y < h; ++n, ++y) {
      uint8_t* dst = bloco + n * line_bytes;
      memcpy(dst, data + (size_t)(h - 1 - y) * w, w * 2);
      if (filtro) { filtro((uint16_t*)dst, (int)w); }   // linha_bytes e multiplo of 4: alinhado
      if (line_bytes > w * 2) { memset(dst + w * 2, 0, line_bytes - w * 2); }
    }
    ok = fwrite(bloco, 1, n * line_bytes, f) == n * line_bytes;
  }
  if (f) { ok = (fclose(f) == 0) && ok; }
  tab5_sd_unmount();
  heap_caps_free(bloco);

  if (ok) {
    ESP_LOGI(TAG, "%s/%s gravado (%ux%u, %lu KB em %lld ms)", pasta, arquivo, (unsigned)w, (unsigned)h,
             (unsigned long)((cab_size + data_size) / 1024), (long long)((esp_timer_get_time() - t0) / 1000));
  } else {
    ESP_LOGE(TAG, "falha ao gravar %s/%s", pasta, arquivo);
  }
  return ok;
}
