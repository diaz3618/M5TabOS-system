#include "tab5_unicode.hpp"

#include <string.h>

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------
// Devolve quantos bytes o caractere ocupa; 0 = sequencia cortada no end of
// string (para ali); -1 = byte que nao comeca caractere (pula one).
static int decodificar(const unsigned char* p, uint32_t* code)
{
  int n;
  uint32_t c;
  if      (*p < 0x80)           { *code = *p; return 1; }
  else if ((*p & 0xE0) == 0xC0) { c = *p & 0x1F; n = 2; }
  else if ((*p & 0xF0) == 0xE0) { c = *p & 0x0F; n = 3; }
  else if ((*p & 0xF8) == 0xF0) { c = *p & 0x07; n = 4; }
  else { return -1; }
  for (int i = 1; i < n; ++i) {
    if ((p[i] & 0xC0) != 0x80) { return p[i] ? -1 : 0; }
    c = (c << 6) | (p[i] & 0x3F);
  }
  *code = c;
  return n;
}

static int codificar(uint32_t c, char* b)
{
  if (c < 0x80)    { b[0] = (char)c; return 1; }
  if (c < 0x800)   { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); return 2; }
  if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
                     b[2] = (char)(0x80 | (c & 0x3F)); return 3; }
  b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
  b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); return 4;
}

// ---------------------------------------------------------------------------
// TABELAS DE TROCA
// ---------------------------------------------------------------------------
// U+00C0..U+00FF sem o acento - so para o que a FONTE nao tiver (Ì, Ù, Å, Ø,
// ý...); o que ela tem passa reto antes of chegar aqui. '#' = caso especial.
static const char LATIN1_BASE[] = "AAAAAA#CEEEEIIIIDNOOOOOxOUUUUY##aaaaaa#ceeeeiiiidnooooo/ouuuuy#y";

// U+0100..U+017F (Latin Extended-A: polones, tcheco, turco, romeno...).
static const char LATIN_A_BASE[] =
  "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOo##RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

// Cirilico U+0410..U+042F (minusculas U+0430..U+044F: o mesmo, em minuscula).
static const char* const CIRILICO[32] = {
  "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "I", "K", "L", "M", "N", "O", "P",
  "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "", "Y", "", "E", "Yu", "Ya",
};

// Grego U+0391..U+03A9 (minusculas U+03B1..U+03C9; U+03A2 nao existe e o
// U+03C2, sigma final, cai in mesma vaga).
static const char* const GREGO[25] = {
  "A", "V", "G", "D", "E", "Z", "I", "Th", "I", "K", "L", "M", "N", "X", "O", "P",
  "R", "S", "S", "T", "Y", "F", "Ch", "Ps", "O",
};

typedef struct { uint32_t c; const char* s; } troca_t;

// ORDENADA por code (busca binaria).
static const troca_t TROCAS[] = {
  { 0x00A2, "c" },   { 0x00A3, "GBP" }, { 0x00A5, "JPY" }, { 0x00A6, "|" },  { 0x00A7, "S" },
  { 0x00A8, "\"" },  { 0x00A9, "(c)" }, { 0x00AC, "-" },   { 0x00AE, "(R)" }, { 0x00AF, "-" },
  { 0x00B4, "'" },   { 0x00B6, "P" },   { 0x00B8, "," },   { 0x00BC, "1/4" }, { 0x00BD, "1/2" },
  { 0x00BE, "3/4" },
  { 0x0386, "A" },   { 0x0388, "E" },   { 0x0389, "I" },   { 0x038A, "I" },   { 0x038C, "O" },
  { 0x038E, "Y" },   { 0x038F, "O" },   { 0x03AC, "a" },   { 0x03AD, "e" },   { 0x03AE, "i" },
  { 0x03AF, "i" },   { 0x03CC, "o" },   { 0x03CD, "y" },   { 0x03CE, "o" },
  { 0x0401, "E" },   { 0x0404, "Ye" },  { 0x0406, "I" },   { 0x0407, "Yi" },  { 0x040E, "U" },
  { 0x0451, "e" },   { 0x0454, "ye" },  { 0x0456, "i" },   { 0x0457, "yi" },  { 0x045E, "u" },
  { 0x0490, "G" },   { 0x0491, "g" },
  { 0x2010, "-" },   { 0x2011, "-" },   { 0x2012, "-" },   { 0x2013, "-" },   { 0x2014, "-" },
  { 0x2015, "-" },   { 0x2016, "||" },  { 0x2018, "'" },   { 0x2019, "'" },   { 0x201A, "," },
  { 0x201B, "'" },   { 0x201C, "\"" },  { 0x201D, "\"" },  { 0x201E, "\"" },  { 0x201F, "\"" },
  { 0x2020, "+" },   { 0x2021, "+" },   { 0x2022, "*" },   { 0x2023, ">" },   { 0x2024, "." },
  { 0x2025, ".." },  { 0x2026, "..." }, { 0x2027, "-" },   { 0x2030, "%o" },  { 0x2032, "'" },
  { 0x2033, "\"" },  { 0x2035, "'" },   { 0x2039, "<" },   { 0x203A, ">" },   { 0x203C, "!!" },
  { 0x2043, "-" },   { 0x2044, "/" },   { 0x2047, "??" },  { 0x2048, "?!" },  { 0x2049, "!?" },
  { 0x2052, "%" },
  { 0x20AC, "EUR" }, { 0x20B9, "Rs" },  { 0x20BD, "RUB" },
  { 0x2102, "C" },   { 0x2103, "C" },   { 0x210A, "g" },   { 0x210B, "H" },   { 0x210C, "H" },
  { 0x210D, "H" },   { 0x210E, "h" },   { 0x2110, "I" },   { 0x2111, "I" },   { 0x2112, "L" },
  { 0x2115, "N" },   { 0x2116, "No" },  { 0x2119, "P" },   { 0x211A, "Q" },   { 0x211B, "R" },
  { 0x211C, "R" },   { 0x211D, "R" },   { 0x2122, "TM" },  { 0x2124, "Z" },   { 0x2128, "Z" },
  { 0x212C, "B" },   { 0x212D, "C" },   { 0x212F, "e" },   { 0x2130, "E" },   { 0x2131, "F" },
  { 0x2133, "M" },   { 0x2134, "o" },
  { 0x2190, "<-" },  { 0x2191, "^" },   { 0x2192, "->" },  { 0x2193, "v" },   { 0x2194, "<->" },
  { 0x21D0, "<=" },  { 0x21D2, "=>" },  { 0x21D4, "<=>" },
  { 0x2212, "-" },   { 0x2215, "/" },   { 0x2217, "*" },   { 0x2219, "." },   { 0x2248, "~" },
  { 0x2260, "!=" },  { 0x2264, "<=" },  { 0x2265, ">=" },
  { 0x25CF, "*" },   { 0x25E6, "o" },
};

static const char* buscar_troca(uint32_t c)
{
  int ini = 0, end = (int)(sizeof(TROCAS) / sizeof(TROCAS[0])) - 1;
  while (ini <= end) {
    const int m = ini + (end - ini) / 2;
    if (TROCAS[m].c == c) { return TROCAS[m].s; }
    if (TROCAS[m].c < c) { ini = m + 1; } else { end = m - 1; }
  }
  return nullptr;
}

// Sem drawing e sem width: some.
static bool invisivel(uint32_t c)
{
  return c == 0x00AD || c == 0x034F || c == 0x061C || c == 0x180E
      || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E)
      || (c >= 0x2060 && c <= 0x206F) || (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF
      || (c >= 0x20D0 && c <= 0x20FF)       // combinantes of simbolo (o keycap 1️⃣ e U+20E3)
      || (c >= 0x1F3FB && c <= 0x1F3FF)     // tom of pele: o emoji base stays, in color default
      || (c >= 0xE0000 && c <= 0xE01EF);    // tags (bandeiras of subdivisao) e seletores
}

static bool espaco(uint32_t c)
{
  return c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F
      || c == 0x205F || c == 0x3000;
}

// Uma letra/numero of equivalente direto (estilizados, width cheia,
// circulados...). 0 = nao e desse tipo.
static uint32_t equivalente(uint32_t c)
{
  // Simbolos Alfanumericos Matematicos: 13 estilos of A-Z a-z, em blocos of 52.
  if (c >= 0x1D400 && c <= 0x1D6A3) {
    const uint32_t l = (c - 0x1D400) % 52;
    return l < 26 ? 'A' + l : 'a' + (l - 26);
  }
  if (c == 0x1D6A4) { return 'i'; }
  if (c == 0x1D6A5) { return 'j'; }
  if (c >= 0x1D7CE && c <= 0x1D7FF) { return '0' + (c - 0x1D7CE) % 10; }
  if (c >= 0xFF01 && c <= 0xFF5E)   { return c - 0xFEE0; }       // width cheia
  if (c >= 0x24B6 && c <= 0x24CF)   { return 'A' + (c - 0x24B6); }   // Ⓐ
  if (c >= 0x24D0 && c <= 0x24E9)   { return 'a' + (c - 0x24D0); }   // ⓐ
  if (c >= 0x249C && c <= 0x24B5)   { return 'a' + (c - 0x249C); }   // ⒜
  if (c == 0x24EA)                  { return '0'; }
  if (c >= 0x2460 && c <= 0x2468)   { return '1' + (c - 0x2460); }   // ①..⑨
  if (c >= 0x1F130 && c <= 0x1F149) { return 'A' + (c - 0x1F130); }  // 🄰
  if (c >= 0x1F150 && c <= 0x1F169) { return 'A' + (c - 0x1F150); }  // 🅐
  if (c >= 0x1F170 && c <= 0x1F189) { return 'A' + (c - 0x1F170); }  // 🅰 (os que nao sao emoji)
  if (c == 0x2070)                  { return '0'; }
  if (c == 0x2071)                  { return 'i'; }
  if (c >= 0x2074 && c <= 0x2079)   { return '4' + (c - 0x2074); }
  if (c == 0x207F)                  { return 'n'; }
  if (c >= 0x2080 && c <= 0x2089)   { return '0' + (c - 0x2080); }
  return 0;
}

// Acento combinado (a + U+0301) -> o caractere pronto (á). 0 = nao ha.
static uint32_t compor(char base, uint32_t marca)
{
  static const char VOG[] = "AEIOUaeiou";
  static const uint8_t GRAVE[] = { 0xC0, 0xC8, 0xCC, 0xD2, 0xD9, 0xE0, 0xE8, 0xEC, 0xF2, 0xF9 };
  static const uint8_t AGUDO[] = { 0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xE1, 0xE9, 0xED, 0xF3, 0xFA };
  static const uint8_t CIRC[]  = { 0xC2, 0xCA, 0xCE, 0xD4, 0xDB, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB };
  static const uint8_t TREMA[] = { 0xC4, 0xCB, 0xCF, 0xD6, 0xDC, 0xE4, 0xEB, 0xEF, 0xF6, 0xFC };
  const char* v = strchr(VOG, base);
  const int i = (v && base) ? (int)(v - VOG) : -1;
  switch (marca) {
    case 0x0300: return i >= 0 ? GRAVE[i] : 0;
    case 0x0301: return i >= 0 ? AGUDO[i] : base == 'Y' ? 0xDD : base == 'y' ? 0xFD : 0;
    case 0x0302: return i >= 0 ? CIRC[i] : 0;
    case 0x0303: return base == 'A' ? 0xC3 : base == 'O' ? 0xD5 : base == 'N' ? 0xD1
                      : base == 'a' ? 0xE3 : base == 'o' ? 0xF5 : base == 'n' ? 0xF1 : 0;
    case 0x0308: return i >= 0 ? TREMA[i] : base == 'y' ? 0xFF : 0;
    case 0x0327: return base == 'C' ? 0xC7 : base == 'c' ? 0xE7 : 0;
    default: return 0;
  }
}

// O text que substitui 'c' (ASCII), em 'buf'. nullptr = nao ha.
static const char* transliterar(uint32_t c, char buf[8])
{
  const uint32_t eq = equivalente(c);
  if (eq) { buf[0] = (char)eq; buf[1] = 0; return buf; }
  if (c >= 0xC0 && c <= 0xFF) {
    const char b = LATIN1_BASE[c - 0xC0];
    if (b != '#') { buf[0] = b; buf[1] = 0; return buf; }
    switch (c) {
      case 0xC6: return "AE";
      case 0xDE: return "Th";
      case 0xDF: return "ss";
      case 0xE6: return "ae";
      case 0xFE: return "th";
    }
  }
  if (c >= 0x100 && c <= 0x17F) {
    const char b = LATIN_A_BASE[c - 0x100];
    if (b != '#') { buf[0] = b; buf[1] = 0; return buf; }
    return c == 0x152 ? "OE" : "oe";
  }
  if (c >= 0x0410 && c <= 0x044F) {
    const char* s = CIRILICO[(c - 0x0410) % 32];
    if (c < 0x0430) { return s; }
    size_t k = 0;
    for (; s[k] && k < 7; ++k) { buf[k] = (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]); }
    buf[k] = 0;
    return buf;
  }
  if ((c >= 0x0391 && c <= 0x03A9) || (c >= 0x03B1 && c <= 0x03C9)) {
    const char* s = GREGO[c >= 0x03B1 ? c - 0x03B1 : c - 0x0391];
    if (c < 0x03B1) { return s; }
    buf[0] = (char)(s[0] + 32);
    if (s[1]) { buf[1] = s[1]; buf[2] = 0; } else { buf[1] = 0; }
    return buf;
  }
  return buscar_troca(c);
}

// ---------------------------------------------------------------------------
// A PASSADA
// ---------------------------------------------------------------------------
void tab5_unicode_prepare(const char* in, char* out, size_t out_size, tab5_unicode_desenha_t desenha)
{
  if (out_size == 0) { return; }
  if (!in) { out[0] = 0; return; }
  size_t o = 0;
  size_t ult = (size_t)-1;   // onde comeca o ultimo caractere escrito (para compor acento)
  const unsigned char* p = (const unsigned char*)in;
  uint32_t anterior = 0;     // o ultimo code LIDO (ver ZWJ em tab5_emoji_preparar)

  // Poe 'n' bytes; false = nao cabe (para everything, in fronteira).
  auto por = [&](const char* s, size_t n) -> bool {
    if (n == 0) { return true; }
    if (o + n >= out_size) { return false; }
    memcpy(out + o, s, n);
    ult = o;
    o += n;
    return true;
  };

  while (*p) {
    uint32_t c = 0;
    const int n = decodificar(p, &c);
    if (n == 0) { break; }
    if (n < 0) { ++p; continue; }
    const unsigned char* prox = p + n;
    const bool colado = (anterior == 0x200D);
    anterior = c;

    // Cadeia ZWJ (familia, profissao...): stays so o first emoji.
    if (c == 0x200D || colado) { p = prox; continue; }

    bool ok = true;
    if (c == '\n' || c == '\r' || c == '\t') {
      if (o > 0 && out[o - 1] != ' ') { ok = por(" ", 1); }
    } else if (c < 0x20 || c == 0x7F) {
      // controle: some
    } else if (c < 0x80) {
      ok = por((const char*)p, 1);
    } else if (c >= 0x0300 && c <= 0x036F) {
      // Acento combinado: junta no anterior, se der; senao some (a letra stays).
      if (ult != (size_t)-1 && o - ult == 1) {
        const uint32_t pronto = compor(out[ult], c);
        if (pronto && desenha && desenha(pronto)) {
          char b[4];
          const int k = codificar(pronto, b);
          if (ult + (size_t)k < out_size) { o = ult; ok = por(b, (size_t)k); }
        }
      }
    } else if (desenha && desenha(c)) {
      ok = por((const char*)p, (size_t)n);
    } else if (invisivel(c)) {
      // some
    } else if (espaco(c)) {
      ok = por(" ", 1);
    } else if (c >= 0x1F1E6 && c <= 0x1F1FF) {
      // Bandeira = dois indicadores regionais (🇧🇷 = B R). Vira "BR".
      uint32_t c2 = 0;
      const int n2 = decodificar(prox, &c2);
      if (n2 > 0 && c2 >= 0x1F1E6 && c2 <= 0x1F1FF) {
        const char par[2] = { (char)('A' + (c - 0x1F1E6)), (char)('A' + (c2 - 0x1F1E6)) };
        ok = por(par, 2);
        prox += n2;
        anterior = c2;
      }
    } else {
      char buf[8];
      const char* t = transliterar(c, buf);
      if (!t) { t = "?"; }
      ok = por(t, strlen(t));
    }
    if (!ok) { break; }
    p = prox;
  }
  out[o] = 0;
}
