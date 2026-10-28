// SAVE AN RGB565 IMAGE AS A .BMP FILE on the card.
//
// The 16-bit color masks are stored in the header (BI_BITFIELDS), so any
// viewer can open the file. The function mounts and unmounts the card itself,
// so it can be called by any task: the system worker (Camera app) or console.
//
// WRITE IN LARGE BLOCKS. The previous version (inside the camera HAL) wrote
// one ROW per fwrite (1440 bytes). FAT rewrote partial sectors on every call,
// making a 1.4 MB image take more than 10 seconds while the preview and app
// were frozen. Rows are now collected in a 32 KB PSRAM block and written once.
//
// Deliberately slow for the INTERFACE task: call it from a worker
// (sistema_trabalho) or a background task.

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Optional adjustment applied to each row in the COPY written to the file
// (the source buffer is not changed). It runs during the row-by-row write,
// without an extra pass over the entire image in PSRAM.
typedef void (*tab5_bmp_filtro_t)(uint16_t* line, int width);

// 'name' without an extension. 'data' is width x height RGB565, row by row
// from top to bottom (the screen's order).
bool tab5_bmp_salvar(const char* pasta, const char* name, const uint16_t* data, int width, int height,
                     tab5_bmp_filtro_t filtro = nullptr);
