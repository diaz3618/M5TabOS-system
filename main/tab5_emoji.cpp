#include "tab5_emoji.hpp"

#include <M5Unified.h>
#include <string.h>
#include <stdlib.h>
#include "tab5_unicode.hpp"
#include "fonts/soft_fonts.hpp"

// BUSCA BINARIA, nao varredura - ver o aviso em tab5_emoji_dados.cpp. Com
// 1357 entries, varredura custaria ate 1357 comparacoes por emoji
// desconhecido; busca binaria custa no maximum 11. A tabela PRECISA estar
// ordenada por code para isto funcionar - o gerador ja garante isso.
static const tab5_emoji_t* achar_emoji(uint32_t code)
{
  int ini = 0, end = TAB5_EMOJI_N - 1;
  while (ini <= end) {
    const int middle = ini + (end - ini) / 2;
    const uint32_t c = TAB5_EMOJI_TABELA[middle].codepoint;
    if (c == code) { return &TAB5_EMOJI_TABELA[middle]; }
    if (c < code)  { ini = middle + 1; } else { end = middle - 1; }
  }
  return nullptr;
}

// O CALLBACK QUE O LOVYANGFX JA SABE CHAMAR.
//
// Assinatura fixada pela biblioteca (LGFXBase.hpp: emoji_draw_cb_t) - nao of
// para mudar one parametro sem break o registro. 'height' vem em pixels JA
// escalados pelo tamanho of text corrente (ver o calculo em
// LGFXBase.cpp: "metrics.height * sy >> 16") - e por isso este callback nao
// precisa saber que tamanho of fonte estava em uso.
//
// Devolver <=0 diz "nao conheco este code": a biblioteca cai sozinha para
// o comportamento of sempre (o codepoint 0, que nao desenha nada). Por isso a
// tabela so precisa saber dos que reconhece - o resto continua invisivel em
// vez of virar lixo in screen, exatamente como era antes deste arquivo existir.
static int32_t emoji_cb(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint32_t code, int32_t height)
{
  const tab5_emoji_t* e = achar_emoji(code);
  if (!e) { return 0; }

  // ESCALADO PARA A CELULA, nao drawn no tamanho nativo.
  //
  // Isto usava pushImage() puro, no tamanho FIXO of bitmap
  // (TAB5_EMOJI_LADO, 24px) - e nunca foi testado contra o caso mais comum:
  // text of message usa tamanho 2, e a celula que a biblioteca reserva para
  // one emoji ali mede 16px of lado, nao 24. O emoji saia 8px MAIOR que a
  // celula em cada eixo, vazando por top of text vizinho.
  //
  // pushImageRotateZoom() resolve isso escalando o bitmap of 24x24 para
  // QUALQUER 'height' requested, sempre centralizado - o mesmo bitmap serve para
  // o text pequeno of uma message (16px) e para one eventual tamanho 4 em
  // outro lugar (32px), sem precisar guardar mais of uma resolucao.
  //
  // SEM SUAVIZACAO (a versao "WithAA" foi tirada). O reason:
  // antialiasing MISTURA a color dos pixels vizinhos in borda of drawing - e
  // perto of borda of qualquer emoji, one dos vizinhos costuma ser o fundo
  // transparente (in epoca, uma cor-sentinela magenta). A mistura produzia
  // uma color que NAO ERA mais a sentinela, e virava one pixel of verdade, in
  // color errada - foi visto on vivo: one ponto magenta released no fundo preto.
  // Hoje o fundo e o INDICE 0 of paleta, e com paleta a biblioteca nem tem
  // como misturar - mas sem AA continua sendo o certo: cada pixel of origem
  // ou E fundo (pula) ou nao e (desenha), sem meio-termo.
  //
  // GEOMETRIA: (src_x,src_y) no bitmap mapeia para (dst_x,dst_y) in screen.
  //
  // A CONTA QUE SE CANCELA PARA LETRA E NAO PARA EMOJI - achado em
  // rastreando o fonte of biblioteca (LGFXBase::write() +
  // GFXfont::drawChar(), ambos em lgfx_fonts.cpp/LGFXBase.cpp), depois que
  // one teste real ("📡 Teste Ok.") mostrou a antena claramente mais baixa
  // que o text of MESMO bubble, in MESMA line.
  //
  // write() desloca o 'y' of line INTEIRA por "-(metrics.y_offset)*sy" ANTES
  // of loop of caracteres (y_offset e o unico valor DA FONTE, nao of glifo -
  // GFXfont::updateFontMetric() nunca escreve nele; so getDefaultMetric()
  // escreve, uma vez, when setFont() roda). Cada caractere NORMAL passa por
  // GFXfont::drawChar(), que aplica "y += metrics.y_offset*sy" of novo -
  // CANCELANDO o offset of line antes of draw o glifo. O EMOJI,
  // por nao passar por drawChar(), recebe o 'y' of line SEM o cancelamento -
  // e stays deslocado para bottom em exatamente "-y_offset * sy" pixels.
  //
  // PARA A FREESANS12PT7B (a fonte of body - ver
  // TAB5_FONTE_CORPO em tab5_ui.hpp): varri a tabela of glifos (grep+awk sobre
  // FreeSans12pt7b.h buscando o menor yOffset, MESMO METODO validado contra a
  // DejaVu24 antes of switch - deu -18, batendo com o "18" ja documentado
  // aqui antes) e cheguei em glyph_ab=17 (o ascendente MAIS ALTO of toda a
  // fonte) - y_offset = -17, height (yAdvance) = 29 = TAB5_FONTE_CORPO_ALT_PX.
  //
  // Corrigir e desfazer esse offset (subtrair 17/29 of 'height' of 'y'
  // recebido) e so ENTAO centralizar o emoji no proprio body of fonte (a
  // metade of top of height, onde as letras realmente desenham) - as duas
  // contas viram UMA fracao so, aplicada on centro.
  //
  // ESTE NUMERO E ESPECIFICO DA FREESANS12PT7B - se TAB5_FONTE_CORPO switch of
  // novo, recalcule glyph_ab/height of fonte nova (mesmo metodo: menor yOffset
  // of tabela of glifos) e troque a conta abaixo junto.
  // a fracao e LIDA of fonte em uso (a of body virou a
  // Roboto suavizada, em varias alturas) - o 17/29 fixo era so of FreeSans12.
  lgfx::FontMetrics fm;
  gfx->getFont()->getDefaultMetric(&fm);
  const float Y_OFFSET_FRACAO = (fm.height > 0) ? (float)(-fm.y_offset) / (float)fm.height : 17.0f / 29.0f;
  const float FRACAO_VERTICAL = 0.5f - Y_OFFSET_FRACAO; // = -0.22: centro of emoji, corrigido e centralizado
  // ESCALA INTEIRA: o bitmap tem 24 px, e ampliar por fracao (24 -> 29 px)
  // duplica umas lines of pixel e outras nao - o mesmo serrilhado que as
  // fontes tinham. Ate 2x a height of text ele exit no tamanho nativo,
  // centrado no espaco of caractere; acima disso, no maior multiplo inteiro.
  //
  // ABAIXO DE 24 px ele ENCOLHE para a height of line: as fontes suaves vao
  // ate 12 px (generate_fonts.py), e ali one emoji of 24 px invadia a line of
  // top e a of bottom. Encolher pula lines of pixel em vez of duplicar - a
  // perda of detalhe num drawing deste tamanho nao se nota; a invasao, sim.
  const float lado = (float)TAB5_EMOJI_LADO;
  const int   mult = (height >= 2 * TAB5_EMOJI_LADO) ? height / TAB5_EMOJI_LADO : 1;
  const float zoom = (height < TAB5_EMOJI_LADO) ? (float)height / lado : (float)mult;
  gfx->pushImageRotateZoom(
      (float)x + (float)height / 2.0f, (float)y + (float)height * FRACAO_VERTICAL,
      lado / 2.0f, lado / 2.0f,
      0.0f,                 // sem rotation
      zoom, zoom,
      TAB5_EMOJI_LADO, TAB5_EMOJI_LADO,
      e->indices, TAB5_EMOJI_TRANSPARENTE, lgfx::color_depth_t::palette_8bit,
      // swap565_t: a mesma leitura (bytes trocados) que a biblioteca fazia of
      // uint16_t cru antes - a paleta foi gravada assim pelo gerador.
      reinterpret_cast<const lgfx::swap565_t*>(e->paleta));

  // Avanca o cursor pela CELULA inteira (height), nao so pelo bitmap - e o
  // mesmo espacamento que one caractere normal deixaria, e o que faz o
  // next caractere nao colar no emoji.
  return height;
}

void tab5_emoji_start(void)
{
  M5.Display.setEmojiCallback(emoji_cb);
}

// O QUE TEM DESENHO: a fonte (lida of proprio VLW - a lista of glifos e a
// que generate_fonts.py gravou, sem copia a manter em sincronia) ou o emoji.
// A of body e a negrito saem of mesmo CARACTERES; basta uma.
static bool fonte_tem(uint32_t code)
{
  static uint32_t* s_cods = nullptr;
  static int       s_n = -1;
  if (s_n < 0) {
    s_n = 0;
    if (FONTES_CORPO_N > 0 && FONTES_CORPO[0].tamanho >= 24) {
      const uint8_t* v = FONTES_CORPO[0].data;
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

void tab5_emoji_prepare(const char* in, char* out, size_t out_size)
{
  // A limpeza toda (ZWJ, corte, e o que nao tem drawing) mora em
  // tab5_unicode - pura, testada no Mac (testes/rodar_unicode.sh).
  tab5_unicode_prepare(in, out, out_size, desenhavel);
}
