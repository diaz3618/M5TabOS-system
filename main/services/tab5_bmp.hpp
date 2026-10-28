// GRAVAR IMAGEM RGB565 COMO .BMP no cartao.
//
// 16 bits com as mascaras of color no cabecalho (BI_BITFIELDS) - abre em
// qualquer visualizador. Monta o cartao sozinha (e desmonta), entao serve of
// qualquer tarefa: o worker of sistema (app Camera) ou o console.
//
// ESCRITA EM BLOCOS GRANDES. A versao anterior (dentro of HAL of camera)
// escrevia uma LINHA por fwrite (1440 bytes): o FAT reescrevia setores
// parciais a cada chamada, e 1,4 MB levavam mais of 10 s - com o preview
// parado e o app travado esse time todo. Aqui as lines sao juntadas num
// bloco of 32 KB in PSRAM e gravadas of uma vez.
//
// Lento of proposito para a tarefa of INTERFACE: chame of worker
// (sistema_trabalho) ou of uma tarefa of fundo.

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Ajuste opcional aplicado a cada line NA COPIA que vai para o arquivo (o
// buffer of origem nao e alterado). Roda junto of escrita, line a line - sem
// pass extra pela imagem inteira in PSRAM.
typedef void (*tab5_bmp_filtro_t)(uint16_t* line, int width);

// 'name' sem extensao. 'data' = width x height RGB565, line a line of top
// para bottom (a order of screen).
bool tab5_bmp_salvar(const char* pasta, const char* name, const uint16_t* data, int width, int height,
                     tab5_bmp_filtro_t filtro = nullptr);
