// EMOJI - um conjunto pequeno, desenhado como IMAGEM, no lugar de fonte.
//
// -------------------------------------------------------------------------
// POR QUE IMAGEM, E POR QUE ISTO E DO KERNEL E NAO DE UM APP SO
// -------------------------------------------------------------------------
// A fonte deste sistema e um bitmap ASCII 6x8 - nao tem emoji, e nunca vai
// ter por fonte: a arte de um emoji de verdade nao cabe num desenho de
// caractere. A saida nao e desenhar de novo, e MOSTRAR o desenho que ja
// existe - o LovyanGFX ja tem o hook certo para isso.
//
// setEmojiCallback() (ver LGFXBase.hpp) e chamado automaticamente pela
// biblioteca toda vez que write()/drawString() encontra um codigo Unicode que
// a fonte atual nao conhece - e isso cobre TODO emoji, porque a fonte deste
// sistema e puro ASCII. Registrando UMA vez no boot, qualquer app que chame
// d.drawString() com uma mensagem recebida passa a mostrar os emoji deste
// conjunto sem saber que este arquivo existe - e o mesmo principio do resto
// do kernel: quem desenha texto nao deveria ter que se preocupar com isso.
//
// -------------------------------------------------------------------------
// O CONJUNTO, O LIMITE E A LICENCA
// -------------------------------------------------------------------------
// TODO emoji de UM SO ponto de codigo Unicode - 1357 deles (dos 1391 que
// existem na versao 17.0; 34 sao adicoes recentes que o Twemoji ainda nao
// desenhou). Arte do Twemoji (twemoji.twitter.com), licenca CC-BY 4.0 -
// atribuicao obrigatoria, e por isso ela mora aqui.
//
// "UM SO PONTO DE CODIGO" e o limite de verdade, e vale entender por que.
// O seletor de estilo (U+FE0F) NAO conta contra isso - a propria biblioteca
// ja o descarta antes de qualquer emoji chegar ao callback (LGFXBase.cpp:
// "if (uniCode >= 0xFE00 && uniCode < 0xFE10) continue"). O que fica de FORA:
//
//   - bandeiras: dois "indicadores regionais" colados (🇧🇷 = U+1F1E7 U+1F1F7)
//   - tom de pele: o emoji base + um modificador (👍🏽 = U+1F44D U+1F3FD)
//   - sequencias ZWJ: varios emoji unidos por U+200D (👨‍👩‍👧 = pessoa+ZWJ+pessoa+ZWJ+crianca)
//
// Cada um desses e VARIOS codigos Unicode formando UMA imagem composta, e o
// hook do LovyanGFX (setEmojiCallback) dispara uma vez POR CODIGO - nao ha
// como esta arquitetura, como esta, juntar N chamadas numa imagem so sem
// trabalho extra de verdade (juntar os codigos pendentes, decidir quando a
// sequencia fechou). Fica para quando fizer falta.
//
// -------------------------------------------------------------------------
// FORMATO
// -------------------------------------------------------------------------
// 24x24 (TAB5_EMOJI_LADO) com PALETA: cada emoji tem a sua lista de cores
// RGB565 (no maximo 256 - nenhum Twemoji passa de 237, a media e 62) e um
// indice de 1 byte por pixel. O indice 0 (TAB5_EMOJI_TRANSPARENTE) e sempre
// o fundo: "nao desenhe". A transparencia e decidida pelo INDICE, nunca pela
// cor - nenhuma mistura de pixels consegue fabricar um falso "transparente".
//
// Por que paleta e nao RGB565 cru: 2 bytes por pixel viram 1, mais a paleta.
// 1357 emoji passaram de ~1,49MB para ~0,91MB de flash, com a MESMA imagem
// pixel a pixel (o gerador so troca a forma de guardar, nao a arte).
//
// TAMANHO NA TELA - ver tab5_emoji.cpp. Ate 2x a altura da linha o bitmap
// sai no tamanho nativo (24px); acima disso, no maior multiplo inteiro; em
// linha MENOR que 24px ele encolhe para a altura dela, para nao invadir as
// linhas de cima e de baixo.
//
// -------------------------------------------------------------------------
// BUSCA
// -------------------------------------------------------------------------
// TAB5_EMOJI_TABELA e ORDENADA por codigo, e tab5_emoji.cpp busca nela por
// BUSCA BINARIA - com mais de mil entradas, varredura linear seria
// desperdicio (ate 1357 comparacoes por emoji desconhecido, contra no maximo
// 11 na busca binaria). O gerador (generate_emoji.py) ja escreve a tabela
// ordenada; se um dia alguem editar isto a mao, a ordem PRECISA ser mantida.
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint32_t        codepoint;   // ponto de codigo Unicode, SEM o seletor de estilo (FE0F)
  const uint8_t*  indices;     // TAB5_EMOJI_LADO x TAB5_EMOJI_LADO, 1 byte por pixel
  const uint16_t* paleta;      // RGB565 com os bytes trocados; [0] e o fundo
} tab5_emoji_t;

static const uint32_t TAB5_EMOJI_TRANSPARENTE = 0;   // indice do fundo

extern const int             TAB5_EMOJI_LADO;
extern const tab5_emoji_t    TAB5_EMOJI_TABELA[];   // ORDENADA por codigo - ver acima
extern const int             TAB5_EMOJI_N;

// Registra o callback no M5.Display. Chamar UMA vez, no boot, depois do
// M5.begin() (o callback e um metodo do painel, que so existe depois disso).
void tab5_emoji_iniciar(void);

// -------------------------------------------------------------------------
// PREPARAR TEXTO PARA DESENHO - a limpeza que evita "lixo na tela"
// -------------------------------------------------------------------------
// Quem monta uma mensagem ou um nome para d.drawString() deveria passar por
// aqui antes, sempre que o texto vem de fora (outro no, outra pessoa) e pode
// conter emoji. Resolve DOIS problemas de uma vez:
//
// 1) CORTE SEGURO. Um snprintf("%.60s", txt) corta por BYTE, nao por
//    caractere - e um emoji ocupa 4 bytes UTF-8. Cortar no meio de um deles
//    deixa bytes orfaos no fim da string, e o decodificador UTF-8 nao tem
//    como saber o que fazer com eles: o resultado e imprevisivel, e foi essa
//    a causa mais provavel do "lixo" observado nas conversas.
//    Aqui o corte SEMPRE cai numa fronteira de caractere.
//
// 2) CADEIAS ZWJ COLAPSADAS PARA UM SO ICONE. Um emoji composto (familia,
//    casal, profissao...) e VARIOS codigos Unicode colados por U+200D (o
//    "zero width joiner"). Este sistema so tem UM desenho por codigo (ver o
//    limite documentado no topo deste arquivo) - sem tratamento, "familia"
//    vira 3 ou 4 icones soltos e sem nenhuma relacao visivel entre si, lado
//    a lado - uma linha cheia de simbolos sem sentido. Aqui mantemos so o PRIMEIRO codigo da cadeia e descartamos o
//    ZWJ e o resto - um icone so, nao perfeito (nao e a imagem composta de
//    verdade), mas CONSISTENTE: sempre um por emoji digitado, nunca uma
//    fileira aleatoria.
//
// 'out' sempre termina em NUL. Se 'in' for maior que 'out_tam' permitir, o
// corte cai na ultima fronteira de caractere que couber - nunca no meio.
void tab5_emoji_preparar(const char* in, char* out, size_t out_tam);
