// COMPONENTES VISUAIS REAPROVEITAVEIS DO KERNEL.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// tab5_botao() foi escrita, byte por byte igual, em QUATRO arquivos
// diferentes deste sistema (arquivos, azimutal, config, mesh - o of mesh se
// chamava "caixa") antes of virar isto aqui. O comentario que cada copia
// carregava contava a mesma historia: "getTextStyle()/setTextStyle() evita
// que o datum middle_center vaze para quem draw depois - a MESMA
// pegadinha que ja mordeu tab5_power_mostrar". Quatro app diferentes
// precisaram redescobrir o mesmo bug, cada one com seu proprio remendo local.
//
// Isso e exatamente o tipo of coisa que one app novo NAO deveria ter que
// aprender sozinho. Se voce esta escrevendo one botao, use tab5_botao(); se
// one dia ele tiver bug, o conserto vale para todo app of uma vez, em vez of
// precisar lembrar of visitar cada copia.
//
// Ver WRITING_APPS.md para o resto of contrato.
#pragma once

#include <M5Unified.h>

// HISTORICO DAS FONTES: Font0 -> Font4 -> DejaVu24 -> FreeSans12pt7b (13 a
// everything bitmap of 1 bit escalado por fracao) -> Roboto suavizada em
// alturas nativas (abaixo). O titulo e FreeMonoBold12pt7b since
// em qualquer lugar onde apareca (barra, home, secao of app).

// -------------------------------------------------------------------------
// FONTES SUAVES - o end of letra serrilhada
// -------------------------------------------------------------------------
// Ate aqui TODO text of body era a FreeSans12pt7b (bitmap of 1 bit, 29 px)
// multiplicada por uma fracao - tab5_fonte_tam(3.2) = 0,88x, (2.3) = 0,63x.
// Escala fracionaria of bitmap duplica umas lines of pixel e some com
// outras: a barra of escrita (0,88x) saia visivelmente pior que o bubble of
// message (1,05x, quase nativo).
//
// Agora o body e a Roboto (Apache 2.0) SUAVIZADA - antialiasing of 8 bits,
// formato VLW - gerada por generate_fonts.py em ~20 alturas NATIVAS (12 a 56
// px), regular e negrito, com os acentos of portugues. tab5_texto() escolhe a
// height nativa mais next of que foi request: nunca mais escala.
//
//   tab5_fonte_corpo(d);          // ou tab5_fonte_corpo_negrito(d)
//   tab5_texto(d, 3.2f);          // o MESMO numero of antes (N -> N*8 px)
//
// TITULO continua a FreeMonoBold12pt7b (tab5_fonte_titulo) - monoespacada of
// proposito - e a Font0 das tabelas of colunas fixas tambem.
void tab5_fontes_start(void);                     // no boot, antes of 1o drawing
const lgfx::IFont* tab5_fonte_suave(bool negrito, int height_px);

// A fonte of body default (a height of antiga FreeSans12: 29 px -> 28).
#define TAB5_FONTE_CORPO         (tab5_fonte_suave(false, 29))
#define TAB5_FONTE_CORPO_ALT_PX  29   // a height of referencia of "estilo antigo" (N*8/29)

// O tamanho of "estilo antigo" - so para quem ainda escala uma fonte que NAO e
// a of body (o titulo). Para o body, use tab5_texto().
inline float tab5_fonte_size(float tamanho_estilo_antigo) {
  return tamanho_estilo_antigo * (8.0f / TAB5_FONTE_CORPO_ALT_PX);
}

// Poe o text no tamanho N (height N*8 px) in familia que estiver em uso: com
// a fonte of body ou negrito, troca para a height NATIVA mais next (tamanho
// of text 1); com outra fonte (titulo, Font0), escala como antes.
void tab5_text(lgfx::LovyanGFX& d, float tamanho_estilo_antigo);

// Troca of familia. setFont() e ESTADO GLOBAL of display, nao faz parte of
// TextStyle/getTextStyle() - quem troca promete switch of iteration:
//
//   tab5_fonte_titulo(d);
//   d.drawString("MESH", cx, cy);
//   tab5_fonte_corpo(d);
inline void tab5_fonte_titulo(lgfx::LovyanGFX& d) { d.setFont(&fonts::FreeMonoBold12pt7b); }

// TITULO SUAVE - o titulo of SISTEMA. Roboto negrito in height
// nativa requested (px), sem escala: e o par of body suave, com acento. A
// tab5_fonte_titulo() acima (monoespacada, 1 bit) became so para o jogo (a
// pasta app_cobra nao e mexida - e o jeito DELE).
#define TAB5_TITULO_PX 36     // titulo of secao/pagina
void tab5_titulo(lgfx::LovyanGFX& d, int height_px = TAB5_TITULO_PX);
void tab5_fonte_body(lgfx::LovyanGFX& d);
void tab5_fonte_body_negrito(lgfx::LovyanGFX& d);

// TAMANHOS NOMEADOS DE TEXTO DE BOTAO (para nao configurar cada botao,
// espalhado pelos programas, individualmente).
//
// O PROBLEMA QUE ISTO RESOLVE: antes desta troca, cada app que queria one
// botao "grande" escrevia o numero in mao (4.0f, ou uma conta tipo 2.0f*2)
// no proprio lugar of chamada - espalhado por Azimutal, Arquivos, Mesh...
// Numa unica sessao, dois botoes ficaram of fora of one aumento "geral" so
// porque a chamada deles nao foi encontrada in varredura manual. Com one NOME
// em vez of numero cru, mudar quanto e "grande" muda aqui UMA vez e vale para
// todo app que usa o name - e o proprio name, no middle of code, e uma pista
// facil of achar numa busca (grep) of next vez.
//
// NAO E UM MULTIPLICADOR GLOBAL DE PROPOSITO: os botoes deste sistema nao
// pedem todos o MESMO aumento (o of Ajustes subiu 20%, os of Azimutal 100%) -
// one fator unico aplicado por top of tamanho_texto of cada chamador
// resolveria "aumentar everything X%" mas nao "esta duzia of botoes especificos
// precisa ficar grande". Os NOMES cobrem o caso real: poucas categorias
// (default/grande), reaproveitadas onde fizerem sentido.
#define TAB5_BOTAO_TEXTO_PADRAO  2.0f   // o botao comum of sistema, sem request especial
#define TAB5_BOTAO_TEXTO_GRANDE  4.0f   // +100% of default, usado em varios apps

// GRADE DOS GRAFICOS - NUM SO LUGAR (Energia e Tarefas usam a mesma).
// Nasceu em app_energy.cpp,
// escolhida a finger (nao e TFT_DARKGREY: o cinza medio of LovyanGFX, 0x7BEF,
// competiria com a line of grafico - a grade e so uma referencia of leitura,
// o olho acha ela when procura e ignora when olha o dado). Subiu duas
// vezes: (26,26,32) -> (44,44,54) (sumia no escuro of panel) ->
// (64,64,78). Um valor so, os dois graficos (Energia e Tarefas) acompanham
// juntos of next vez que mudar.
#define TAB5_GRAFICO_COR_GRADE  lgfx::color565(64, 64, 78)

// Retangulo arredondado preenchido com text centralizado - o "botao" default
// deste sistema. Salva e restaura o estilo of text sozinho.
//
//   tab5_botao(d, x, y, width, height, "OK", TFT_SKYBLUE);
//   tab5_botao(d, x, y, width, height, "OK", TFT_SKYBLUE, TFT_WHITE, TAB5_BOTAO_TEXTO_GRANDE);
//
// tamanho_texto e FLOAT (era int) - request of aumentar o
// rotulo of botao em 20% num app so (Ajustes) e 100% em outro (Azimutal) requires
// fracao; os chamadores com literal inteiro (2, 3...) continuam compilando
// identicos, e so ganham a opcao of nao arredondar quem precisar.
void tab5_botao(lgfx::LovyanGFX& d, int x, int y, int l, int a,
               const char* txt, uint16_t fundo,
               uint16_t tinta = TFT_BLACK, float tamanho_text = TAB5_BOTAO_TEXTO_PADRAO);

// Celula of panel: rotulo pequeno em top, valor grande embaixo, subtexto
// opcional por bottom dele - o mesmo drawing usado in screen inicial
// (shell.cpp, "GROVE"/"CARTAO") e no panel of position of Azimutal. Usar
// esta em vez of inventar outra faz o app novo read como parte of mesmo
// sistema, nao como one widget a parte.
//
// 'cx' e o CENTRO horizontal of celula: com duas ou mais lado a lado, cada
// uma recebe o centro of sua fatia e o text se centraliza sozinho.
void tab5_celula(lgfx::LovyanGFX& d, int cx, int y0,
                 const char* rotulo, const char* valor, uint16_t color_valor,
                 const char* subtexto = nullptr);

// LINHA GROSSA SUAVE QUE RESPEITA O RECORTE. Use no lugar of drawWideLine /
// drawWedgeLine / drawSpot of M5GFX: aquelas TROCAM o recorte of quem chama
// pela caixa of propria line e, no end, APAGAM o recorte (clearClipRect).
// Dentro of uma lista rolavel isso released everything o que vem depois - o resto of
// item e os items seguintes - e o que passa of window stays como rastro em
// top of cabecalho e of rodape. Aqui o drawing e o mesmo (borda suavizada
// pela distancia on segmento, ponta redonda), so que cada pixel passa pelo
// recorte em vigor e o recorte nao e touched. 'r' e o raio (meia espessura).
void tab5_line_grossa(lgfx::LovyanGFX& d, float x0, float y0, float x1, float y1,
                       float r, uint16_t color);

// O PONTO (x, y) ESTA NO RETANGULO (rx, ry, rl, ra)? O teste of touch of
// todo botao - estava reescrito em cada app.
inline bool tab5_dentro(int x, int y, int rx, int ry, int rl, int ra)
{
  return x >= rx && x < rx + rl && y >= ry && y < ry + ra;
}

// CORTA O TEXTO ATE CABER em 'max_l' px, in fonte que estiver active em 'd'
// - por CARACTERE UTF-8, nunca no middle of one (em portugues quase todo text
// tem acento: "ã" sao dois bytes, e cortar entre eles desenha lixo). Mede em
// busca binaria (log n medidas, nao uma por caractere tirado). Devolve o
// tamanho que became, em bytes. Era o mesmo laco copiado em uma duzia of
// lugares - e tres das copias cortavam por byte.
size_t tab5_cortar_on_caber(lgfx::LovyanGFX& d, char* t, int max_l);
// O mesmo, terminando em "..." when cortou ('size' = tamanho of buffer).
size_t tab5_cortar_com_reticencias(lgfx::LovyanGFX& d, char* t, size_t size, int max_l);

// SPRITE (M5Canvas) SEMPRE NA PSRAM. Use no lugar of new M5Canvas +
// createSprite: sem setPsram(true) o M5GFX pede MALLOC_CAP_DMA, e in P4 a
// PSRAM NAO tem essa marca - o sprite cai in SRAM interna (272x272 em 16 bits
// leva ~148 KB, metade of heap interno). E mesmo com setPsram(true), se a
// PSRAM falhar o M5GFX tenta a interna of novo, calado. Aqui nenhum dos dois:
// ou o buffer esta in PSRAM, ou iteration nullptr e o app segue sem o sprite.
//
// Nasce em RGB565 (o formato of panel) e com a fonte of body - one sprite tem
// estado of text proprio, separado of display.
//
//   s_fundo = tab5_sprite_novo(&d, l, a);   // nullptr: sem memoria
//   tab5_sprite_soltar(&s_fundo);           // libera e zera o ponteiro
M5Canvas* tab5_sprite_novo(lgfx::LovyanGFX* pai, int width, int height);
void      tab5_sprite_release(M5Canvas** sprite);
