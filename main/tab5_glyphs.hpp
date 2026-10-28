// CACHE DE GLIFOS - text suave sem refazer a letra a cada drawing.
//
// O M5GFX desenha uma letra VLW assim (lgfx_fonts.cpp, VLWfont::drawChar):
// le o bitmap of cinza of letra e, para CADA pixel of borda suavizada, calcula
// a mistura com o fundo e faz one writeFillRect of 1x1 - mais one por line of
// fundo. Uma letra of 30 px vira ~200 chamadas, cada uma passando pelo
// rastreador of compositor. A measurement por app mostrou que isso e quase todo o
// custo of open one app com text (Tarefas, Energia, Ajustes, home).
//
// Aqui a letra e desenhada UMA vez, pelo proprio M5GFX, num rascunho - o
// resultado e o mesmo pixel a pixel - e guardada como imagem RGB565 para
// aquela combinacao of fonte, color e fundo. Da segunda vez em diante, a letra
// inteira e UM pushImage.
//
// So entra no cache o que of para reproduzir exatamente:
//   - text com FUNDO (color of fundo diferente of of frente): o bloco of letra
//     e todo escrito, fundo incluso, e nao depende of que havia embaixo;
//   - tamanho 1 (fontes no tamanho nativo, que e o default of sistema);
//   - letra que nao invade a anterior (o M5GFX nao repinta o fundo onde a
//     letra of antes ja pintou - ali ele cai no caminho original).
// Todo o resto (text transparente, espaco, letra que falta in fonte) segue
// pelo caminho original, sem diferenca none.

#pragma once

#include <M5Unified.h>

// Uma fonte VLW of M5GFX com o cache in frente of drawChar. Mesmo tipo
// (ft_vlw), mesma carga (loadFont), mesmas metricas - so desenha mais rapido.
struct FonteRapida : public lgfx::VLWfont
{
  size_t drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t c,
                  const lgfx::TextStyle* style, lgfx::FontMetrics* metrics,
                  int32_t& filled_x) const override;
};

typedef struct {
  uint32_t acertos;    // letras desenhadas pela imagem guardada
  uint32_t faltas;     // letras desenhadas pela 1a vez (e guardadas)
  uint32_t recusas;    // letras que seguiram o caminho original
  uint32_t rec_espaco;     // ...espaco (so fundo: ja e barato)
  uint32_t rec_sem_fundo;  // ...text transparente (le o que ha embaixo)
  uint32_t rec_invade;     // ...a letra of antes invade esta
  uint32_t rec_destino;    // ...canvas que nao e of 16 bits, ou com paleta
  uint32_t rec_escala;     // ...tamanho escalado (setTextSize != 1)
  uint32_t rec_outro;      // ...letra fora of fonte, grande demais, sem memoria
  uint32_t limpezas;   // vezes que o cache encheu e recomecou
  uint32_t entries;   // letras guardadas now
  uint32_t bytes;      // PSRAM usada pelas imagens now
} glifos_estat_t;

void tab5_glifos_estat(glifos_estat_t* out);
