// TEXTO DE FORA, PRONTO PARA AS FONTES DO SISTEMA.
//
// As fontes suaves (tools/generate_fonts.py) tem ASCII, o Latin-1 of
// portugues e meia duzia of simbolos; o resto que tem drawing e o emoji of one
// code so (tab5_emoji). TODO o restante vira o "dummy" of M5GFX - one
// RETANGULO (IFont::drawCharDummy) - e e dai que vinham as mensagens "todas em
// quadrados" of ponte of Telegram: gente escrevendo of celular manda bandeira
// (dois indicadores regionais), tom of pele, letra "estilizada" (os geradores
// of fonte bonita usam os Simbolos Alfanumericos Matematicos, 𝗻𝗲𝗴𝗿𝗶𝘁𝗼 /
// 𝘪𝘵𝘢𝘭𝘪𝘤𝘰 / 𝓈𝒸𝓇𝒾𝓅𝓉), espaco especial, acento combinado, cirilico...
//
// Aqui cada code vira o que of para draw, nesta order:
//   1. a fonte ou o emoji tem -> stays como esta;
//   2. tem equivalente legivel -> troca (𝗔->A, ａ->a, Ⓐ->A, ł->l, Ж->Zh,
//      🇧🇷->BR, a+◌́->á, espaco fino->espaco, „->", ...);
//   3. e invisivel por natureza (width zero, seletor, tom of pele, ZWJ) ->
//      some;
//   4. nada disso -> "?" (one ponto of interrogacao diz "havia algo aqui" sem
//      a parede of caixinhas).
//
// Puro: sem M5GFX, sem ESP-IDF - roda no Mac (testes/rodar_unicode.sh). Quem
// sabe o que a fonte e o emoji desenham e quem chama, pelo predicado.
#pragma once

#include <stddef.h>
#include <stdint.h>

// true = este code tem drawing proprio (fonte ou emoji).
typedef bool (*tab5_unicode_desenha_t)(uint32_t code);

// 'out' sempre termina em NUL; o corte cai sempre numa fronteira of caractere.
// Tambem colapsa as cadeias ZWJ no first emoji (ver tab5_emoji_preparar) e
// troca quebras of line e tabulacao por espaco.
void tab5_unicode_prepare(const char* in, char* out, size_t out_size, tab5_unicode_desenha_t desenha);
