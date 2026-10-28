#include "tab5_emoji.hpp"

#include <M5Unified.h>
#include <string.h>
#include <stdlib.h>
#include "tab5_unicode.hpp"
#include "fonts/soft_fonts.hpp"

// BUSCA BINARIA, nao varredura - ver o aviso em tab5_emoji_dados.cpp. Com
// 1357 entradas, varredura custaria ate 1357 comparacoes por emoji
// desconhecido; busca binaria custa no maximo 11. A tabela PRECISA estar
// ordenada por codigo para isto funcionar - o gerador ja garante isso.
static const tab5_emoji_t* achar_emoji(uint32_t code)
{
  int ini = 0, fim = TAB5_EMOJI_N - 1;
  while (ini <= fim) {
    const int meio = ini + (fim - ini) / 2;
    const uint32_t c = TAB5_EMOJI_TABELA[meio].codepoint;
    if (c == code) { return &TAB5_EMOJI_TABELA[meio]; }
    if (c < code)  { ini = meio + 1; } else { fim = meio - 1; }
  }
  return nullptr;
}

// O CALLBACK QUE O LOVYANGFX JA SABE CHAMAR.
//
// Assinatura fixada pela biblioteca (LGFXBase.hpp: emoji_draw_cb_t) - nao da
// para mudar um parametro sem quebrar o registro. 'altura' vem em pixels JA
// escalados pelo tamanho de texto corrente (ver o calculo em
// LGFXBase.cpp: "metrics.height * sy >> 16") - e por isso este callback nao
// precisa saber que tamanho de fonte estava em uso.
//
// Devolver <=0 diz "nao conheco este codigo": a biblioteca cai sozinha para
// o comportamento de sempre (o codepoint 0, que nao desenha nada). Por isso a
// tabela so precisa saber dos que reconhece - o resto continua invisivel em
// vez de virar lixo na tela, exatamente como era antes deste arquivo existir.
static int32_t emoji_cb(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint32_t code, int32_t altura)
{
  const tab5_emoji_t* e = achar_emoji(code);
  if (!e) { return 0; }

  // ESCALADO PARA A CELULA, nao desenhado no tamanho nativo.
  //
  // Isto usava pushImage() puro, no tamanho FIXO do bitmap
  // (TAB5_EMOJI_LADO, 24px) - e nunca foi testado contra o caso mais comum:
  // texto de mensagem usa tamanho 2, e a celula que a biblioteca reserva para
  // um emoji ali mede 16px de lado, nao 24. O emoji saia 8px MAIOR que a
  // celula em cada eixo, vazando por cima do texto vizinho.
  //
  // pushImageRotateZoom() resolve isso escalando o bitmap de 24x24 para
  // QUALQUER 'altura' pedida, sempre centralizado - o mesmo bitmap serve para
  // o texto pequeno de uma mensagem (16px) e para um eventual tamanho 4 em
  // outro lugar (32px), sem precisar guardar mais de uma resolucao.
  //
  // SEM SUAVIZACAO (a versao "WithAA" foi tirada). O motivo:
  // antialiasing MISTURA a cor dos pixels vizinhos na borda do desenho - e
  // perto da borda de qualquer emoji, um dos vizinhos costuma ser o fundo
  // transparente (na epoca, uma cor-sentinela magenta). A mistura produzia
  // uma cor que NAO ERA mais a sentinela, e virava um pixel de verdade, na
  // cor errada - foi visto ao vivo: um ponto magenta solto no fundo preto.
  // Hoje o fundo e o INDICE 0 da paleta, e com paleta a biblioteca nem tem
  // como misturar - mas sem AA continua sendo o certo: cada pixel de origem
  // ou E fundo (pula) ou nao e (desenha), sem meio-termo.
  //
  // GEOMETRIA: (src_x,src_y) no bitmap mapeia para (dst_x,dst_y) na tela.
  //
  // A CONTA QUE SE CANCELA PARA LETRA E NAO PARA EMOJI - achado em
  // rastreando o fonte da biblioteca (LGFXBase::write() +
  // GFXfont::drawChar(), ambos em lgfx_fonts.cpp/LGFXBase.cpp), depois que
  // um teste real ("📡 Teste Ok.") mostrou a antena claramente mais baixa
  // que o texto do MESMO balao, na MESMA linha.
  //
  // write() desloca o 'y' da linha INTEIRA por "-(metrics.y_offset)*sy" ANTES
  // do loop de caracteres (y_offset e o unico valor DA FONTE, nao do glifo -
  // GFXfont::updateFontMetric() nunca escreve nele; so getDefaultMetric()
  // escreve, uma vez, quando setFont() roda). Cada caractere NORMAL passa por
  // GFXfont::drawChar(), que aplica "y += metrics.y_offset*sy" de novo -
  // CANCELANDO o deslocamento da linha antes de desenhar o glifo. O EMOJI,
  // por nao passar por drawChar(), recebe o 'y' da linha SEM o cancelamento -
  // e fica deslocado para baixo em exatamente "-y_offset * sy" pixels.
  //
  // PARA A FREESANS12PT7B (a fonte de corpo - ver
  // TAB5_FONTE_CORPO em tab5_ui.hpp): varri a tabela de glifos (grep+awk sobre
  // FreeSans12pt7b.h buscando o menor yOffset, MESMO METODO validado contra a
  // DejaVu24 antes de trocar - deu -18, batendo com o "18" ja documentado
  // aqui antes) e cheguei em glyph_ab=17 (o ascendente MAIS ALTO de toda a
  // fonte) - y_offset = -17, height (yAdvance) = 29 = TAB5_FONTE_CORPO_ALT_PX.
  //
  // Corrigir e desfazer esse deslocamento (subtrair 17/29 de 'altura' do 'y'
  // recebido) e so ENTAO centralizar o emoji no proprio corpo da fonte (a
  // metade de cima da altura, onde as letras realmente desenham) - as duas
  // contas viram UMA fracao so, aplicada ao centro.
  //
  // ESTE NUMERO E ESPECIFICO DA FREESANS12PT7B - se TAB5_FONTE_CORPO trocar de
  // novo, recalcule glyph_ab/height da fonte nova (mesmo metodo: menor yOffset
  // da tabela de glifos) e troque a conta abaixo junto.
  // a fracao e LIDA da fonte em uso (a de corpo virou a
  // Roboto suavizada, em varias alturas) - o 17/29 fixo era so da FreeSans12.
  lgfx::FontMetrics fm;
  gfx->getFont()->getDefaultMetric(&fm);
  const float Y_OFFSET_FRACAO = (fm.height > 0) ? (float)(-fm.y_offset) / (float)fm.height : 17.0f / 29.0f;
  const float FRACAO_VERTICAL = 0.5f - Y_OFFSET_FRACAO; // = -0.22: centro do emoji, corrigido e centralizado
  // ESCALA INTEIRA: o bitmap tem 24 px, e ampliar por fracao (24 -> 29 px)
  // duplica umas linhas de pixel e outras nao - o mesmo serrilhado que as
  // fontes tinham. Ate 2x a altura do texto ele sai no tamanho nativo,
  // centrado no espaco do caractere; acima disso, no maior multiplo inteiro.
  //
  // ABAIXO DE 24 px ele ENCOLHE para a altura da linha: as fontes suaves vao
  // ate 12 px (generate_fonts.py), e ali um emoji de 24 px invadia a linha de
  // cima e a de baixo. Encolher pula linhas de pixel em vez de duplicar - a
  // perda de detalhe num desenho deste tamanho nao se nota; a invasao, sim.
  const float lado = (float)TAB5_EMOJI_LADO;
  const int   mult = (altura >= 2 * TAB5_EMOJI_LADO) ? altura / TAB5_EMOJI_LADO : 1;
  const float zoom = (altura < TAB5_EMOJI_LADO) ? (float)altura / lado : (float)mult;
  gfx->pushImageRotateZoom(
      (float)x + (float)altura / 2.0f, (float)y + (float)altura * FRACAO_VERTICAL,
      lado / 2.0f, lado / 2.0f,
      0.0f,                 // sem rotacao
      zoom, zoom,
      TAB5_EMOJI_LADO, TAB5_EMOJI_LADO,
      e->indices, TAB5_EMOJI_TRANSPARENTE, lgfx::color_depth_t::palette_8bit,
      // swap565_t: a mesma leitura (bytes trocados) que a biblioteca fazia do
      // uint16_t cru antes - a paleta foi gravada assim pelo gerador.
      reinterpret_cast<const lgfx::swap565_t*>(e->paleta));

  // Avanca o cursor pela CELULA inteira (altura), nao so pelo bitmap - e o
  // mesmo espacamento que um caractere normal deixaria, e o que faz o
  // proximo caractere nao colar no emoji.
  return altura;
}

void tab5_emoji_iniciar(void)
{
  M5.Display.setEmojiCallback(emoji_cb);
}

// O QUE TEM DESENHO: a fonte (lida do proprio VLW - a lista de glifos e a
// que generate_fonts.py gravou, sem copia a manter em sincronia) ou o emoji.
// A de corpo e a negrito saem do mesmo CARACTERES; basta uma.
static bool fonte_tem(uint32_t code)
{
  static uint32_t* s_cods = nullptr;
  static int       s_n = -1;
  if (s_n < 0) {
    s_n = 0;
    if (FONTES_CORPO_N > 0 && FONTES_CORPO[0].tamanho >= 24) {
      const uint8_t* v = FONTES_CORPO[0].dados;
      auto be32 = [](const uint8_t* b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; };
      const uint32_t n = be32(v);
      if (24 + n * 28 <= FONTES_CORPO[0].tamanho) {
        s_cods = (uint32_t*)malloc(n * sizeof(uint32_t));
        if (s_cods) { for (uint32_t i = 0; i < n; ++i) { s_cods[i] = be32(v + 24 + i * 28); } s_n = (int)n; }
      }
    }
  }
  for (int i = 0; i < s_n; ++i) { if (s_cods[i] == code) { return true; } }
  return false;
}

static bool desenhavel(uint32_t code)
{
  return fonte_tem(code) || achar_emoji(code) != nullptr;
}

void tab5_emoji_preparar(const char* in, char* out, size_t out_tam)
{
  // A limpeza toda (ZWJ, corte, e o que nao tem desenho) mora em
  // tab5_unicode - pura, testada no Mac (testes/rodar_unicode.sh).
  tab5_unicode_preparar(in, out, out_tam, desenhavel);
}
