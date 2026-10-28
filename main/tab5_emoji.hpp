// EMOJI - one conjunto pequeno, drawn como IMAGEM, no lugar of fonte.
//
// -------------------------------------------------------------------------
// POR QUE IMAGEM, E POR QUE ISTO E DO KERNEL E NAO DE UM APP SO
// -------------------------------------------------------------------------
// A fonte deste sistema e one bitmap ASCII 6x8 - nao tem emoji, e nunca vai
// ter por fonte: a arte of one emoji of verdade nao cabe num drawing of
// caractere. A saida nao e draw of novo, e MOSTRAR o drawing que ja
// existe - o LovyanGFX ja tem o hook certo para isso.
//
// setEmojiCallback() (ver LGFXBase.hpp) e chamado automaticamente pela
// biblioteca toda vez que write()/drawString() encontra one code Unicode que
// a fonte current nao conhece - e isso cobre TODO emoji, porque a fonte deste
// sistema e puro ASCII. Registrando UMA vez no boot, qualquer app que chame
// d.drawString() com uma message recebida passa a mostrar os emoji deste
// conjunto sem saber que este arquivo existe - e o mesmo principio of resto
// of kernel: quem desenha text nao deveria ter que se preocupar com isso.
//
// -------------------------------------------------------------------------
// O CONJUNTO, O LIMITE E A LICENCA
// -------------------------------------------------------------------------
// TODO emoji of UM SO ponto of code Unicode - 1357 deles (dos 1391 que
// existem in versao 17.0; 34 sao adicoes recentes que o Twemoji ainda nao
// desenhou). Arte of Twemoji (twemoji.twitter.com), licenca CC-BY 4.0 -
// atribuicao obrigatoria, e por isso ela mora aqui.
//
// "UM SO PONTO DE CODIGO" e o limit of verdade, e vale entender por que.
// O seletor of estilo (U+FE0F) NAO conta contra isso - a propria biblioteca
// ja o descarta antes of qualquer emoji chegar on callback (LGFXBase.cpp:
// "if (uniCode >= 0xFE00 && uniCode < 0xFE10) continue"). O que stays of FORA:
//
//   - bandeiras: dois "indicadores regionais" colados (🇧🇷 = U+1F1E7 U+1F1F7)
//   - tom of pele: o emoji base + one modificador (👍🏽 = U+1F44D U+1F3FD)
//   - sequencias ZWJ: varios emoji unidos por U+200D (👨‍👩‍👧 = pessoa+ZWJ+pessoa+ZWJ+crianca)
//
// Cada one desses e VARIOS codigos Unicode formando UMA imagem composta, e o
// hook of LovyanGFX (setEmojiCallback) dispara uma vez POR CODIGO - nao ha
// como esta arquitetura, como esta, juntar N chamadas numa imagem so sem
// work extra of verdade (juntar os codigos pendentes, decidir when a
// sequencia fechou). Fica para when fizer falta.
//
// -------------------------------------------------------------------------
// FORMATO
// -------------------------------------------------------------------------
// 24x24 (TAB5_EMOJI_LADO) com PALETA: cada emoji tem a sua lista of cores
// RGB565 (no maximum 256 - nenhum Twemoji passa of 237, a media e 62) e one
// indice of 1 byte por pixel. O indice 0 (TAB5_EMOJI_TRANSPARENTE) e sempre
// o fundo: "nao desenhe". A transparencia e decidida pelo INDICE, nunca pela
// color - none mistura of pixels consegue fabricar one falso "transparente".
//
// Por que paleta e nao RGB565 cru: 2 bytes por pixel viram 1, mais a paleta.
// 1357 emoji passaram of ~1,49MB para ~0,91MB of flash, com a MESMA imagem
// pixel a pixel (o gerador so troca a forma of guardar, nao a arte).
//
// TAMANHO NA TELA - ver tab5_emoji.cpp. Ate 2x a height of line o bitmap
// exit no tamanho nativo (24px); acima disso, no maior multiplo inteiro; em
// line MENOR que 24px ele encolhe para a height dela, para nao invadir as
// lines of top e of bottom.
//
// -------------------------------------------------------------------------
// BUSCA
// -------------------------------------------------------------------------
// TAB5_EMOJI_TABELA e ORDENADA por code, e tab5_emoji.cpp busca nela por
// BUSCA BINARIA - com mais of mil entries, varredura linear seria
// desperdicio (ate 1357 comparacoes por emoji desconhecido, contra no maximum
// 11 in busca binaria). O gerador (generate_emoji.py) ja escreve a tabela
// ordenada; se one dia alguem edit isto a mao, a order PRECISA ser mantida.
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint32_t        codepoint;   // ponto of code Unicode, SEM o seletor of estilo (FE0F)
  const uint8_t*  indices;     // TAB5_EMOJI_LADO x TAB5_EMOJI_LADO, 1 byte por pixel
  const uint16_t* paleta;      // RGB565 com os bytes trocados; [0] e o fundo
} tab5_emoji_t;

static const uint32_t TAB5_EMOJI_TRANSPARENTE = 0;   // indice of fundo

extern const int             TAB5_EMOJI_LADO;
extern const tab5_emoji_t    TAB5_EMOJI_TABELA[];   // ORDENADA por code - ver acima
extern const int             TAB5_EMOJI_N;

// Registra o callback no M5.Display. Chamar UMA vez, no boot, depois of
// M5.begin() (o callback e one metodo of panel, que so existe depois disso).
void tab5_emoji_start(void);

// -------------------------------------------------------------------------
// PREPARAR TEXTO PARA DESENHO - a limpeza que evita "lixo in screen"
// -------------------------------------------------------------------------
// Quem monta uma message ou one name para d.drawString() deveria passar por
// aqui antes, sempre que o text vem of fora (outro no, outra pessoa) e pode
// conter emoji. Resolve DOIS problemas of uma vez:
//
// 1) CORTE SEGURO. Um snprintf("%.60s", txt) corta por BYTE, nao por
//    caractere - e one emoji ocupa 4 bytes UTF-8. Cortar no middle of one deles
//    deixa bytes orfaos no end of string, e o decodificador UTF-8 nao tem
//    como saber o que fazer com eles: o resultado e imprevisivel, e foi essa
//    a causa mais provavel of "lixo" observado nas conversas.
//    Aqui o corte SEMPRE cai numa fronteira of caractere.
//
// 2) CADEIAS ZWJ COLAPSADAS PARA UM SO ICONE. Um emoji composto (familia,
//    casal, profissao...) e VARIOS codigos Unicode colados por U+200D (o
//    "zero width joiner"). Este sistema so tem UM drawing por code (ver o
//    limit documentado no topo deste arquivo) - sem tratamento, "familia"
//    vira 3 ou 4 icones soltos e sem none relacao visivel entre si, lado
//    a lado - uma line cheia of simbolos sem sentido. Aqui mantemos so o PRIMEIRO code of cadeia e descartamos o
//    ZWJ e o resto - one icon so, nao perfeito (nao e a imagem composta of
//    verdade), mas CONSISTENTE: sempre one por emoji digitado, nunca uma
//    fileira aleatoria.
//
// 'out' sempre termina em NUL. Se 'in' for maior que 'out_tam' permitir, o
// corte cai in ultima fronteira of caractere que couber - nunca no middle.
void tab5_emoji_prepare(const char* in, char* out, size_t out_size);
