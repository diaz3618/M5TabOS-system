#pragma once

#include <stddef.h>
#include <stdint.h>

// true = este code tem drawing proprio (fonte ou emoji).
typedef bool (*tab5_unicode_desenha_t)(uint32_t code);

// 'out' sempre termina em NUL; o corte cai sempre numa fronteira of caractere.
// Tambem colapsa as cadeias ZWJ no first emoji (ver tab5_emoji_preparar) e
// troca quebras of line e tabulacao por espaco.
void tab5_unicode_prepare(const char* in, char* out, size_t out_size, tab5_unicode_desenha_t desenha);
