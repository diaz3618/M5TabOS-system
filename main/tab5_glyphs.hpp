// CACHE DE GLIFOS - texto suave sem refazer a letra a cada desenho.
//
// O M5GFX desenha uma letra VLW assim (lgfx_fonts.cpp, VLWfont::drawChar):
// le o bitmap de cinza da letra e, para CADA pixel de borda suavizada, calcula
// a mistura com o fundo e faz um writeFillRect de 1x1 - mais um por linha de
// fundo. Uma letra de 30 px vira ~200 chamadas, cada uma passando pelo
// rastreador do compositor. A medida por app mostrou que isso e quase todo o
// custo de abrir um app com texto (Tarefas, Energia, Ajustes, home).
//
// Aqui a letra e desenhada UMA vez, pelo proprio M5GFX, num rascunho - o
// resultado e o mesmo pixel a pixel - e guardada como imagem RGB565 para
// aquela combinacao de fonte, cor e fundo. Da segunda vez em diante, a letra
// inteira e UM pushImage.
//
// So entra no cache o que da para reproduzir exatamente:
//   - texto com FUNDO (cor de fundo diferente da de frente): o bloco da letra
//     e todo escrito, fundo incluso, e nao depende do que havia embaixo;
//   - tamanho 1 (fontes no tamanho nativo, que e o padrao do sistema);
//   - letra que nao invade a anterior (o M5GFX nao repinta o fundo onde a
//     letra de antes ja pintou - ali ele cai no caminho original).
// Todo o resto (texto transparente, espaco, letra que falta na fonte) segue
// pelo caminho original, sem diferenca nenhuma.

#pragma once

#include <M5Unified.h>

// Uma fonte VLW do M5GFX com o cache na frente do drawChar. Mesmo tipo
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
  uint32_t rec_sem_fundo;  // ...texto transparente (le o que ha embaixo)
  uint32_t rec_invade;     // ...a letra de antes invade esta
  uint32_t rec_destino;    // ...canvas que nao e de 16 bits, ou com paleta
  uint32_t rec_escala;     // ...tamanho escalado (setTextSize != 1)
  uint32_t rec_outro;      // ...letra fora da fonte, grande demais, sem memoria
  uint32_t limpezas;   // vezes que o cache encheu e recomecou
  uint32_t entradas;   // letras guardadas agora
  uint32_t bytes;      // PSRAM usada pelas imagens agora
} glifos_estat_t;

void tab5_glifos_estat(glifos_estat_t* out);
