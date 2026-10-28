// PRIORIDADE NO BARRAMENTO DE MEMORIA (AXI-ICM do ESP32-P4).
//
// O PROBLEMA, na raiz. A tela e um painel MIPI-DSI em modo DPI: o controlador
// le o framebuffer da PSRAM CONTINUAMENTE, linha a linha, no ritmo do relogio
// de pixel. A 80MHz num painel 720x1280 com os porticos, sao ~119 MB/s que NAO
// podem atrasar - cada linha tem que estar no FIFO quando o painel a pede.
//
// Quando o processador tambem martela a PSRAM (limpar a tela, desenhar baloes,
// descarregar 256KB de cache), os dois disputam o mesmo barramento. Se o DMA da
// tela perde a disputa por tempo demais, o FIFO esvazia - e o proprio driver do
// IDF descreve o que acontece:
//
//   "when an underrun happens, the LCD display may already becomes blue,
//    it's too late to recover the display, so we just print an error message
//    as a hint to the user that he should optimize the memory bandwidth
//    (with AXI-ICM)"                        (esp_lcd_panel_dpi.c, linha 113)
//
// E A PISCADA AZUL. Nao e bug de nenhum app: e arbitragem de barramento.
//
// O QUE VINHAMOS FAZENDO ERA REMENDO. Limpar em faixas com pausas, evitar
// limpar duas vezes, espacar repinturas - tudo isso REDUZ a demanda do lado da
// CPU, e ajuda. Mas nao resolve, porque a cada app novo alguem tem que lembrar
// de todas as regras, e a disputa continua existindo.
//
// A SOLUCAO QUE O PROPRIO DRIVER APONTA e o AXI-ICM: a matriz de interconexao
// tem prioridade de arbitragem POR MESTRE, e por padrao TODOS valem 0 - o DMA
// da tela e tratado igual ao cache do processador. Dando prioridade maior ao
// mestre que alimenta o painel, o arbitro serve a tela primeiro e o FIFO deixa
// de secar, independentemente do que a CPU esteja fazendo.
//
// O DPI le por DW-GDMA (esp_lcd_panel_dpi.c usa esp_private/dw_gdma.h), que
// aparece na ICM como gdma_mst1 e gdma_mst2.
//
// HONESTIDADE SOBRE O RISCO: nenhum componente do IDF chama esta API - ela e
// exposta e nao usada. Nao consegui testar em hardware. Por isso existe o
// comando de console para ligar e desligar em execucao: da para comparar os
// dois estados sem regravar, e o log do driver ("underrun happens") diz na hora
// qual deles esta certo.
#pragma once

#include <stdbool.h>

// Aplica a prioridade. Uma vez, no boot.
void tab5_barramento_priorizar_tela(bool ligar);

// Estado atual, para o console mostrar.
bool tab5_barramento_priorizado(void);
