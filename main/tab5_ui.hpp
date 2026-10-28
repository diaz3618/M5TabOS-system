// COMPONENTES VISUAIS REAPROVEITAVEIS DO KERNEL.
//
// -------------------------------------------------------------------------
// POR QUE ISTO EXISTE
// -------------------------------------------------------------------------
// tab5_botao() foi escrita, byte por byte igual, em QUATRO arquivos
// diferentes deste sistema (arquivos, azimutal, config, mesh - o de mesh se
// chamava "caixa") antes de virar isto aqui. O comentario que cada copia
// carregava contava a mesma historia: "getTextStyle()/setTextStyle() evita
// que o datum middle_center vaze para quem desenhar depois - a MESMA
// pegadinha que ja mordeu tab5_power_mostrar". Quatro app diferentes
// precisaram redescobrir o mesmo bug, cada um com seu proprio remendo local.
//
// Isso e exatamente o tipo de coisa que um app novo NAO deveria ter que
// aprender sozinho. Se voce esta escrevendo um botao, use tab5_botao(); se
// um dia ele tiver bug, o conserto vale para todo app de uma vez, em vez de
// precisar lembrar de visitar cada copia.
//
// Ver WRITING_APPS.md para o resto do contrato.
#pragma once

#include <M5Unified.h>

// HISTORICO DAS FONTES: Font0 -> Font4 -> DejaVu24 -> FreeSans12pt7b (13 a
// tudo bitmap de 1 bit escalado por fracao) -> Roboto suavizada em
// alturas nativas (abaixo). O titulo e FreeMonoBold12pt7b desde
// em qualquer lugar onde apareca (barra, home, secao de app).

// -------------------------------------------------------------------------
// FONTES SUAVES - o fim da letra serrilhada
// -------------------------------------------------------------------------
// Ate aqui TODO texto de corpo era a FreeSans12pt7b (bitmap de 1 bit, 29 px)
// multiplicada por uma fracao - tab5_fonte_tam(3.2) = 0,88x, (2.3) = 0,63x.
// Escala fracionaria de bitmap duplica umas linhas de pixel e some com
// outras: a barra de escrita (0,88x) saia visivelmente pior que o balao de
// mensagem (1,05x, quase nativo).
//
// Agora o corpo e a Roboto (Apache 2.0) SUAVIZADA - antialiasing de 8 bits,
// formato VLW - gerada por generate_fonts.py em ~20 alturas NATIVAS (12 a 56
// px), regular e negrito, com os acentos do portugues. tab5_texto() escolhe a
// altura nativa mais proxima do que foi pedido: nunca mais escala.
//
//   tab5_fonte_corpo(d);          // ou tab5_fonte_corpo_negrito(d)
//   tab5_texto(d, 3.2f);          // o MESMO numero de antes (N -> N*8 px)
//
// TITULO continua a FreeMonoBold12pt7b (tab5_fonte_titulo) - monoespacada de
// proposito - e a Font0 das tabelas de colunas fixas tambem.
void tab5_fontes_iniciar(void);                     // no boot, antes do 1o desenho
const lgfx::IFont* tab5_fonte_suave(bool negrito, int altura_px);

// A fonte de corpo padrao (a altura da antiga FreeSans12: 29 px -> 28).
#define TAB5_FONTE_CORPO         (tab5_fonte_suave(false, 29))
#define TAB5_FONTE_CORPO_ALT_PX  29   // a altura de referencia do "estilo antigo" (N*8/29)

// O tamanho do "estilo antigo" - so para quem ainda escala uma fonte que NAO e
// a de corpo (o titulo). Para o corpo, use tab5_texto().
inline float tab5_fonte_tam(float tamanho_estilo_antigo) {
  return tamanho_estilo_antigo * (8.0f / TAB5_FONTE_CORPO_ALT_PX);
}

// Poe o texto no tamanho N (altura N*8 px) na familia que estiver em uso: com
// a fonte de corpo ou negrito, troca para a altura NATIVA mais proxima (tamanho
// de texto 1); com outra fonte (titulo, Font0), escala como antes.
void tab5_texto(lgfx::LovyanGFX& d, float tamanho_estilo_antigo);

// Troca de familia. setFont() e ESTADO GLOBAL do display, nao faz parte de
// TextStyle/getTextStyle() - quem troca promete trocar de volta:
//
//   tab5_fonte_titulo(d);
//   d.drawString("MESH", cx, cy);
//   tab5_fonte_corpo(d);
inline void tab5_fonte_titulo(lgfx::LovyanGFX& d) { d.setFont(&fonts::FreeMonoBold12pt7b); }

// TITULO SUAVE - o titulo do SISTEMA. Roboto negrito na altura
// nativa pedida (px), sem escala: e o par do corpo suave, com acento. A
// tab5_fonte_titulo() acima (monoespacada, 1 bit) ficou so para o jogo (a
// pasta app_cobra nao e mexida - e o jeito DELE).
#define TAB5_TITULO_PX 36     // titulo de secao/pagina
void tab5_titulo(lgfx::LovyanGFX& d, int altura_px = TAB5_TITULO_PX);
void tab5_fonte_corpo(lgfx::LovyanGFX& d);
void tab5_fonte_corpo_negrito(lgfx::LovyanGFX& d);

// TAMANHOS NOMEADOS DE TEXTO DE BOTAO (para nao configurar cada botao,
// espalhado pelos programas, individualmente).
//
// O PROBLEMA QUE ISTO RESOLVE: antes desta troca, cada app que queria um
// botao "grande" escrevia o numero na mao (4.0f, ou uma conta tipo 2.0f*2)
// no proprio lugar da chamada - espalhado por Azimutal, Arquivos, Mesh...
// Numa unica sessao, dois botoes ficaram de fora de um aumento "geral" so
// porque a chamada deles nao foi encontrada na varredura manual. Com um NOME
// em vez do numero cru, mudar quanto e "grande" muda aqui UMA vez e vale para
// todo app que usa o nome - e o proprio nome, no meio do codigo, e uma pista
// facil de achar numa busca (grep) da proxima vez.
//
// NAO E UM MULTIPLICADOR GLOBAL DE PROPOSITO: os botoes deste sistema nao
// pedem todos o MESMO aumento (o de Ajustes subiu 20%, os de Azimutal 100%) -
// um fator unico aplicado por cima do tamanho_texto de cada chamador
// resolveria "aumentar tudo X%" mas nao "esta duzia de botoes especificos
// precisa ficar grande". Os NOMES cobrem o caso real: poucas categorias
// (padrao/grande), reaproveitadas onde fizerem sentido.
#define TAB5_BOTAO_TEXTO_PADRAO  2.0f   // o botao comum do sistema, sem pedido especial
#define TAB5_BOTAO_TEXTO_GRANDE  4.0f   // +100% do padrao, usado em varios apps

// GRADE DOS GRAFICOS - NUM SO LUGAR (Energia e Tarefas usam a mesma).
// Nasceu em app_energy.cpp,
// escolhida a dedo (nao e TFT_DARKGREY: o cinza medio do LovyanGFX, 0x7BEF,
// competiria com a linha do grafico - a grade e so uma referencia de leitura,
// o olho acha ela quando procura e ignora quando olha o dado). Subiu duas
// vezes: (26,26,32) -> (44,44,54) (sumia no escuro do painel) ->
// (64,64,78). Um valor so, os dois graficos (Energia e Tarefas) acompanham
// juntos da proxima vez que mudar.
#define TAB5_GRAFICO_COR_GRADE  lgfx::color565(64, 64, 78)

// Retangulo arredondado preenchido com texto centralizado - o "botao" padrao
// deste sistema. Salva e restaura o estilo de texto sozinho.
//
//   tab5_botao(d, x, y, largura, altura, "OK", TFT_SKYBLUE);
//   tab5_botao(d, x, y, largura, altura, "OK", TFT_SKYBLUE, TFT_WHITE, TAB5_BOTAO_TEXTO_GRANDE);
//
// tamanho_texto e FLOAT (era int) - pedido de aumentar o
// rotulo do botao em 20% num app so (Ajustes) e 100% em outro (Azimutal) exige
// fracao; os chamadores com literal inteiro (2, 3...) continuam compilando
// identicos, e so ganham a opcao de nao arredondar quem precisar.
void tab5_botao(lgfx::LovyanGFX& d, int x, int y, int l, int a,
               const char* txt, uint16_t fundo,
               uint16_t tinta = TFT_BLACK, float tamanho_texto = TAB5_BOTAO_TEXTO_PADRAO);

// Celula de painel: rotulo pequeno em cima, valor grande embaixo, subtexto
// opcional por baixo dele - o mesmo desenho usado na tela inicial
// (shell.cpp, "GROVE"/"CARTAO") e no painel de posicao do Azimutal. Usar
// esta em vez de inventar outra faz o app novo ler como parte do mesmo
// sistema, nao como um widget a parte.
//
// 'cx' e o CENTRO horizontal da celula: com duas ou mais lado a lado, cada
// uma recebe o centro da sua fatia e o texto se centraliza sozinho.
void tab5_celula(lgfx::LovyanGFX& d, int cx, int y0,
                 const char* rotulo, const char* valor, uint16_t cor_valor,
                 const char* subtexto = nullptr);

// LINHA GROSSA SUAVE QUE RESPEITA O RECORTE. Use no lugar de drawWideLine /
// drawWedgeLine / drawSpot do M5GFX: aquelas TROCAM o recorte de quem chama
// pela caixa da propria linha e, no fim, APAGAM o recorte (clearClipRect).
// Dentro de uma lista rolavel isso solta tudo o que vem depois - o resto do
// item e os itens seguintes - e o que passa da janela fica como rastro em
// cima do cabecalho e do rodape. Aqui o desenho e o mesmo (borda suavizada
// pela distancia ao segmento, ponta redonda), so que cada pixel passa pelo
// recorte em vigor e o recorte nao e tocado. 'r' e o raio (meia espessura).
void tab5_linha_grossa(lgfx::LovyanGFX& d, float x0, float y0, float x1, float y1,
                       float r, uint16_t cor);

// O PONTO (x, y) ESTA NO RETANGULO (rx, ry, rl, ra)? O teste de toque de
// todo botao - estava reescrito em cada app.
inline bool tab5_dentro(int x, int y, int rx, int ry, int rl, int ra)
{
  return x >= rx && x < rx + rl && y >= ry && y < ry + ra;
}

// CORTA O TEXTO ATE CABER em 'max_l' px, na fonte que estiver ativa em 'd'
// - por CARACTERE UTF-8, nunca no meio de um (em portugues quase todo texto
// tem acento: "ã" sao dois bytes, e cortar entre eles desenha lixo). Mede em
// busca binaria (log n medidas, nao uma por caractere tirado). Devolve o
// tamanho que ficou, em bytes. Era o mesmo laco copiado em uma duzia de
// lugares - e tres das copias cortavam por byte.
size_t tab5_cortar_ao_caber(lgfx::LovyanGFX& d, char* t, int max_l);
// O mesmo, terminando em "..." quando cortou ('tam' = tamanho do buffer).
size_t tab5_cortar_com_reticencias(lgfx::LovyanGFX& d, char* t, size_t tam, int max_l);

// SPRITE (M5Canvas) SEMPRE NA PSRAM. Use no lugar de new M5Canvas +
// createSprite: sem setPsram(true) o M5GFX pede MALLOC_CAP_DMA, e na P4 a
// PSRAM NAO tem essa marca - o sprite cai na SRAM interna (272x272 em 16 bits
// leva ~148 KB, metade do heap interno). E mesmo com setPsram(true), se a
// PSRAM falhar o M5GFX tenta a interna de novo, calado. Aqui nenhum dos dois:
// ou o buffer esta na PSRAM, ou volta nullptr e o app segue sem o sprite.
//
// Nasce em RGB565 (o formato do painel) e com a fonte de corpo - um sprite tem
// estado de texto proprio, separado do display.
//
//   s_fundo = tab5_sprite_novo(&d, l, a);   // nullptr: sem memoria
//   tab5_sprite_soltar(&s_fundo);           // libera e zera o ponteiro
M5Canvas* tab5_sprite_novo(lgfx::LovyanGFX* pai, int largura, int altura);
void      tab5_sprite_soltar(M5Canvas** sprite);
