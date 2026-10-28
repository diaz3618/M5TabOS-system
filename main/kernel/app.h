// Contrato de um aplicativo do sistema.
//
// A ideia: adicionar um app novo e criar a pasta main/app_<nome>/ com um
// arquivo que define 'extern const app_t APP_<NOME>' (ver apps.cpp). Mais nada. O
// sistema cuida de abrir, fechar, desenhar, rotear o toque e - o principal -
// aplicar o perfil de energia que o app declarou.
//
// Sobre "C ou C++": o codigo e escrito em estilo C (structs, ponteiros de
// funcao, sem classes), mas em arquivos .cpp porque o M5GFX e uma biblioteca
// C++ e nao ha como desenhar sem passar por ela. O modelo mental e o de C:
// tarefas, queues e funcoes.

#pragma once

#include <M5Unified.h>
#include <stdint.h>
#include "tab5_power.hpp"
#include "tab5_sd.hpp"
#include "fundo.h"      // parte de fundo de um app (trabalho que sobrevive a janela)

// Retangulo onde o app pode desenhar. Fica entre a barra de status (topo) e a
// barra de navegacao (base) - o app nunca precisa saber a altura delas.
typedef struct {
  int x;
  int y;
  int largura;
  int altura;
} area_t;

// ORIENTACAO DA TELA de um app (campo .orientacao do app_t, abaixo).
//
// SO DOIS ESTADOS EXISTEM: em pe (rotacao 0, a da home) e deitado (rotacao 3,
// a que deixa o ExtPort1 - onde o teclado encaixa - na borda de baixo). As
// rotacoes 1 e 2 nunca sao usadas: a 1 poe a tela de cabeca para baixo em
// relacao ao teclado, e a 2 e o retrato invertido.
typedef enum {
  ORI_EM_PE = 0,    // o padrao: retrato, 720x1280
  ORI_DEITADO,      // paisagem fixa, 1280x720
  // Segue o aparelho (kernel/orientacao): em pe ou deitado conforme a
  // gravidade, entre os mesmos dois estados. Com o teclado fisico encaixado,
  // fica deitado - e a unica postura em que ele faz sentido.
  ORI_SEGUE_IMU,
} orientacao_t;

// COMANDO DE CONSOLE DE UM APP (campo .comandos do app_t, abaixo).
//
// O console serial e do sistema, mas um app pode oferecer os proprios
// comandos. Eles aparecem no 'help' num grupo so deles (APPS) e sao chamados
// sempre com o nome do app na frente, para nunca colidir com um comando do
// sistema nem com o de outro app:
//
//     app <app> <comando> [argumentos]       ex.: app exemplo contar 5
//
// <app> e o .nome do app sem diferenca de maiusculas; espacos e hifens podem
// ser omitidos ('app espnow' acha "ESP-NOW").
//
// ONDE RODA: na tarefa do CONSOLE, nao na de interface. Entao, como em
// qualquer tarefa do app: nao desenhe (mude o dado e chame
// sistema_pedir_redesenho()), nao fale I2C interno, e proteja com trava o que
// a interface tambem le. printf vai para a serial.
//
// APP FECHADO: com so_aberto = true o sistema recusa o comando enquanto o app
// nao estiver em primeiro plano - e a regra para todo comando que mexe em
// estado criado no ao_abrir() (que nao existe com o app fechado). Comando que
// so le dado estatico ou chama servico pode deixar false.
//
// A lista termina numa entrada com nome = nullptr.
typedef struct {
  const char* nome;       // a palavra depois do nome do app
  const char* uso;        // "argumentos\to que faz", uma forma por linha (sem 'app <app> <comando>')
  void (*fn)(int argc, char** argv);   // argv[0] = o nome do comando
  bool so_aberto;
} app_comando_t;

typedef struct {
  // ---- identidade ---------------------------------------------------------
  const char* nome;     // aparece na barra de status e sob o icone
  uint16_t    cor;      // cor do icone na tela inicial

  // ---- energia ------------------------------------------------------------
  // O QUE ESTE APP PRECISA LIGADO. Tudo que nao esta aqui fica desligado
  // enquanto ele estiver em primeiro plano, e volta ao estado do sistema
  // quando ele fechar. E o mecanismo que impede o aparelho de esquentar
  // alimentando coisa que o app nem usa.
  tab5_perfil_t perfil;

  // Este app grava ou le arquivos. O sistema MONTA o cartao antes de chamar
  // ao_abrir() e DESMONTA depois de ao_fechar() - com contagem de referencia,
  // entao dois apps que usem SD ao mesmo tempo nao atrapalham um ao outro.
  //
  // O app nao chama montar nem desmontar. Ele so pede um arquivo dentro da
  // propria pasta:
  //     FILE* f = tab5_sd_abrir(APP_MEU.nome, "dados.csv", "a");
  // e escreve com fprintf/fwrite normais. A pasta /sdcard/apps/<Nome>/ e criada
  // sozinha na primeira vez.
  bool usa_sd;

  // SEM O CARTAO ESTE APP NAO TEM FUNCAO (Arquivos: navega nele; Camera: a
  // foto vai para ele). Com o cartao emprestado ao computador (modo
  // pendrive), o SISTEMA nao abre o app - mostra no lugar uma tela dizendo
  // por que, e abre sozinho quando o cartao voltar - e o pendrive nao comeca
  // com ele aberto. Nao e o app que se protege: nenhum app precisa tratar
  // "o cartao sumiu no meio".
  //
  // Quem so GRAVA dados (historico, log) nao precisa disto: as portas de
  // gravacao (hal/tab5_sd.hpp) guardam no diario e o app segue funcionando.
  bool exige_sd;

  // Em que orientacao este app quer a tela (ver orientacao_t acima).
  //
  // ORI_DEITADO nasceu para o Meshtastic: com o teclado do ExtPort1
  // encaixado, paisagem e a postura natural, e uma lista de mensagens cabe
  // muito mais. ORI_SEGUE_IMU deixa o proprio aparelho escolher.
  //
  // Funciona porque a rotacao no M5GFX transforma COORDENADAS, nao o
  // framebuffer: o double buffer (tab5_tela) monta os ponteiros de linha a partir
  // das dimensoes fisicas do painel, que nao mudam. E o area_util() ja calcula
  // tudo a partir de Display.width()/height(), entao barra e navegacao se
  // reacomodam sozinhas.
  //
  // QUEM SEGUE O IMU PRECISA DESENHAR A PARTIR DA AREA, sempre: a largura
  // pode mudar com o app aberto. O sistema gira so entre dois quadros, sem
  // dedo na tela, e manda uma passada completa logo depois (a area chega
  // limpa, sistema_area_foi_limpa() = true). Geometria guardada de um quadro
  // para outro - medida de texto, posicao de botao - tem que ser refeita.
  orientacao_t orientacao;

  // Clock minimo, em MHz, que este app precisa MESMO COM A TELA APAGADA.
  // Serve para apps com trabalho de fundo: se o mapa azimutal continua lendo
  // o GPS durante o repouso, ele diz aqui o quanto precisa. O sistema baixa o
  // clock ate esse piso e nao alem. 0 = pode descer ao minimo (40 MHz).
  int piso_repouso_mhz;

  // ---- ciclo de vida ------------------------------------------------------
  // Chamado ao abrir. E AQUI que o app cria suas tarefas e queues.
  // Devolver false aborta a abertura (o sistema volta para a tela inicial).
  bool (*ao_abrir)(void);

  // Chamado ao fechar. Precisa destruir TUDO que ao_abrir() criou - tarefas,
  // queues, buffers. Nada de tarefa orfa: app fechado tem que custar zero.
  void (*ao_fechar)(void);

  // ---- repouso --------------------------------------------------------
  // Chamados em volta do repouso por inatividade (kernel/core.cpp) - NAO em
  // volta de ao_abrir/ao_fechar. Existem para um app soltar hardware que
  // ao_abrir() ligou e que NAO pode continuar ativo durante o repouso, sem
  // precisar fechar o app inteiro para isso.
  //
  // NASCEU, achado ao vivo: o app Camera trava e reinicia o
  // aparelho ao voltar do repouso. Causa - a captura V4L2 da camera e o
  // painel DSI usam o MESMO controlador de DMA generico do chip (DW_GDMA,
  // confirmado no driver do ESP-IDF: esp_cam_ctlr_csi.c e
  // esp_lcd_panel_dpi.c chamam as mesmas funcoes). O repouso DESTROI e
  // RECRIA o canal de DMA do painel (ver desmontar_tela()/montar_tela() em
  // hal/tab5_power.cpp) - se a camera continuar com o canal DELA vivo
  // durante essa janela, o painel novo pode nascer pisando um canal ainda
  // em uso, e o driver quebra dentro de dw_gdma_channel_use_link_list()
  // com um "Store access fault".
  //
  // ao_repouso(): chamado ANTES do sistema desligar a tela (ainda com o
  // painel de pe) - solte aqui qualquer DMA/hardware que nao pode conviver
  // com a desmontagem do painel.
  // ao_retomar(): chamado DEPOIS do painel ja estar remontado - religue
  // aqui o que foi solto.
  //
  // Os dois sao OPCIONAIS (nullptr = nada a fazer, o caso comum). Rodam na
  // tarefa de interface, a mesma de ao_abrir/ao_fechar - seguro chamar
  // qualquer coisa que estes ja chamam, inclusive I2C interno.
  void (*ao_repouso)(void);
  void (*ao_retomar)(void);

  // ---- interface ----------------------------------------------------------
  // Chamado pela tarefa de interface, nunca por outra. Nao chame funcoes de
  // desenho de dentro das suas proprias tarefas: o LovyanGFX nao e seguro
  // para acesso concorrente. Passe os dados por queue e desenhe aqui.
  void (*desenhar)(lgfx::LovyanGFX& tela, const area_t& area);

  // Toque dentro da area do app, ja em coordenadas RELATIVAS a area.
  // Dispara na SOLTURA do dedo (o clique do sistema: desceu e subiu sem
  // arrastar mais de ~2 mm, com qualquer duracao - kernel/core.cpp,
  // TOQUE_TOLERANCIA_PX), que e o certo para botao: da
  // para desistir arrastando o dedo para fora antes de soltar.
  void (*ao_tocar)(int x, int y);

  // Toque por NIVEL, entregue a CADA ciclo da interface (20 ms), com o dedo
  // ainda na tela. 'pressionado' diz se ha dedo na area do app agora; x,y so
  // valem quando ele e true (fora disso vem -1, -1).
  //
  // Existe porque ao_tocar() so avisa quando o dedo SAI, e ha duas coisas que
  // um botao nunca precisa mas um jogo precisa: reagir na DESCIDA do dedo, e
  // saber que ele CONTINUA ali. Quem quer detectar o instante do toque guarda
  // o valor anterior e compara - a borda de subida e do app, nao do sistema.
  //
  // Os dois convivem: um app pode usar so ao_tocar (o caso normal), so este,
  // ou os dois. Deixe nullptr se nao precisa.
  void (*ao_tocar_nivel)(int x, int y, bool pressionado);

  // TECLA DO TECLADO FISICO (o do ExtPort1), ja decodificada.
  //
  // 'texto' vem com terminador nulo, em UTF-8 (uma letra acentuada composta
  // pela tecla morta tem 2 bytes); para as teclas especiais ele vem VAZIO e o
  // que vale e 'codigo' (hal/tab5_teclado.hpp):
  //
  //   TAB5_TECLA_ENTER, _APAGAR (Backspace), _DEL, _ESC, _TAB,
  //   _ESQUERDA, _DIREITA, _CIMA, _BAIXO
  //
  // Codigo que o app nao conhece: ignore (vira tecla nova, nao texto). Com
  // Ctrl/Alt segurados o texto vem assim mesmo - e atalho, nao digitacao.
  //
  // 'modificador' e a combinacao de TAB5_TECLA_CTRL/SHIFT/ALT no momento.
  //
  // POR QUE ISTO EXISTE, e e uma correcao e nao um recurso: o sistema lia o
  // teclado desde sempre e IMPRIMIA A TECLA NO LOG, sem entregar a ninguem. E
  // o app Mesh, ao ver que havia teclado fisico encaixado, deixava de abrir o
  // teclado virtual - para nao roubar tela. Resultado: com o teclado plugado
  // nao havia como escrever de jeito nenhum, nem virtual nem fisico. Era este
  // o motivo de "nunca consegui enviar".
  //
  // Deixe nullptr se o app nao le teclado.
  void (*ao_teclar)(const char* texto, uint8_t codigo, uint8_t modificador);

  // De quanto em quanto tempo redesenhar sozinho, em ms.
  // 0 = so redesenha quando alguem chamar sistema_pedir_redesenho().
  // Prefira 0 ou valores grandes: cada redesenho custa CPU e energia.
  uint32_t periodo_ms;

  // PERIODO QUE MUDA COM A TELA (opcional). Quando existe, vence
  // periodo_ms: devolve o periodo de AGORA. Para o app que so precisa de
  // relogio numa pagina (o Mesh: idade dos nos) e em nenhuma outra - o resto
  // do tempo ele devolve 0 e so acorda por aviso.
  uint32_t (*periodo_agora)(void);

  // AVISOS que interessam a este app (mascara de AVISO_*). Quando um servico
  // publica um deles, o sistema redesenha o app NA HORA - sem esperar periodo
  // nenhum. E a troca de "perguntar a cada segundo se mudou" por "ser avisado
  // quando mudar". ao_aviso e opcional: sem ele, o aviso vira um
  // sistema_pedir_redesenho(); com ele, o app decide o que fazer.
  uint32_t avisos;
  void (*ao_aviso)(uint32_t avisos);

  // ---------------------------------------------------------------------
  // ROLAGEM POR TOQUE
  // ---------------------------------------------------------------------
  // Altura TOTAL do conteudo do app, em pixels. 0 (o padrao) = nao rola, e o
  // app enxerga exatamente a area visivel, como sempre foi.
  //
  // Sendo maior que a area, o SISTEMA passa a cuidar de tudo: arrastar o dedo
  // rola, o desenho e recortado na area, e as coordenadas do ao_tocar chegam
  // ja no espaco do CONTEUDO. Ou seja: o app desenha e testa toque num
  // sistema de coordenadas que comeca em 0 e vai ate altura_conteudo, sem
  // saber que existe rolagem.
  //
  // Isso e de proposito. A alternativa - cada app somar um deslocamento em
  // todo y - e a receita para o app que esquece de somar em UM lugar e fica
  // com um botao que responde no lugar errado depois de rolar.
  //
  // Um arraste NAO vira toque: o sistema mede a distancia percorrida e engole
  // o clique se ela passar do limiar. Sem isso, todo fim de rolagem apertaria
  // o que estivesse embaixo do dedo.
  int altura_conteudo;

  // Desenho do icone na tela inicial, dentro do quadrado (x, y, lado).
  //
  // nullptr - o padrao - desenha o quadrado na cor do app com a inicial do
  // nome, que e o que sempre houve e continua bom para a maioria. Um app que
  // fala um protocolo de terceiros ganha em mostrar a marca DELE: quem procura
  // o Meshtastic procura o simbolo do Meshtastic, nao a letra M de "Mesh".
  //
  // O nome continua sendo desenhado embaixo pelo sistema, nos dois casos.
  void (*icone)(lgfx::LovyanGFX& d, int x, int y, int lado);

  // A TELA GIROU com o app aberto (so acontece com ORI_SEGUE_IMU). Chamado
  // na tarefa de interface, DEPOIS de a rotacao nova valer (sistema_area()
  // ja devolve as dimensoes novas) e ANTES da passada completa que a
  // redesenha. Serve para o app trocar de layout - nao precisa pedir
  // redesenho, o sistema ja pediu. Opcional: nullptr = nada a fazer alem de
  // desenhar pela area nova.
  void (*ao_girar)(void);

  // Comandos de console deste app (ver app_comando_t acima). nullptr = nenhum.
  const app_comando_t* comandos;
} app_t;

// ---------------------------------------------------------------------------
// API do sistema, disponivel para os apps
// ---------------------------------------------------------------------------

// Marca a tela como suja: o app sera redesenhado no proximo ciclo.
// Use quando um dado mudou, em vez de pedir redesenho periodico rapido.
void sistema_pedir_redesenho(void);

// BIP DO SISTEMA - o unico caminho para tocar um tom curto. Chamar SEMPRE da
// tarefa de interface (fala I2C com o codec).
//
// Existe porque o alto-falante e desligado depois de alguns segundos ocioso
// (ver SPEAKER_OCIOSO_MS em core.cpp) e o ES8388 leva um tempo para subir: um
// tone() direto, com o codec frio, termina ANTES de o DAC ficar audivel - o
// primeiro bip depois de uma pausa saia mudo. Aqui, com o codec frio, o tom
// vai precedido de silencio ate o codec aquecer.
void sistema_beep(float hz, uint32_t ms);

// OS PINOS DO ALTO-FALANTE PARA O USB - so o pendrive usa (services/
// tab5_pendrive). O I2S do alto-falante (GPIO 26/27) e o PHY USB que o ESP-IDF
// acha que e do OTG: configurar esses pinos desliga o USB-C durante o
// pendrive (ver o aviso em core.cpp). Ceder: o alto-falante e desligado pela
// interface e fica mudo; true quando ja esta parado (espera ate espera_ms).
// Devolver: o som volta no proximo bip. De qualquer tarefa, menos a interface.
bool sistema_som_ceder_pinos(uint32_t espera_ms);
void sistema_som_devolver_pinos(void);

// ACORDA A INTERFACE AGORA - Etapa 2 do ARQUITETURA.md. Em
// repouso, a interface pode estar dormindo ate ~1s esperando o proximo prazo
// periodico (ver kernel/core.cpp); isto pula a espera na hora que algo
// realmente novo aconteceu, em vez de deixar o aviso esperar. Pensado para
// SERVICOS chamarem (notificacao.cpp e a ponte da camera ja chamam), mas
// nada impede um app de usar se um dia precisar do mesmo efeito - so poe um
// evento na queue, nunca bloqueia, seguro de qualquer tarefa.
void sistema_acordar_interface(void);

// ---------------------------------------------------------------------------
// AVISOS - o servico diz o que mudou, o app interessado e redesenhado na hora
// ---------------------------------------------------------------------------
// Seguro de QUALQUER tarefa (e barato: um OR atomico e um evento na queue, sem
// bloquear). Varios avisos iguais antes de a interface acordar viram um so.
#define AVISO_MESH_MSG    (1u << 0)   // mensagem nova (recebida ou enviada)
#define AVISO_MESH_ENVIO  (1u << 1)   // estado de envio mudou (ACK, erro)
#define AVISO_MESH_NOS    (1u << 2)   // tabela de nos mudou
#define AVISO_GPS         (1u << 3)   // posicao nova
#define AVISO_TRILHA      (1u << 4)   // amostra nova da trilha
#define AVISO_TRABALHO    (1u << 5)   // um sistema_trabalho() deste app terminou (kernel/trabalho.hpp)
#define AVISO_ESPNOW      (1u << 6)   // ESP-NOW: mensagem, estado de envio ou canal mudou (services/tab5_espnow)
void sistema_avisar(uint32_t avisos);

// Para quando a tela inteira muda de conteudo (virar pagina, trocar de nivel):
// limpa a area e repinta pelo double buffer, sem piscar. Ver kernel/core.cpp.
void sistema_pedir_redesenho_completo(void);

// Volta a rolagem do app para o topo e pede redesenho completo.
//
// Para quando um app tem mais de uma "tela" interna dividindo o MESMO
// altura_conteudo (o campo e fixo, nao da para trocar por tela em runtime -
// ver o app_config, Ajustes/galeria de fontes). Chame ao trocar de tela por
// dentro do app: sem isto a rolagem de uma tela vaza para a outra, visual ou
// funcionalmente (um toque interpretado com o deslocamento errado).
void sistema_zerar_rolagem(void);

// Liga/desliga a rolagem por arraste do sistema inteiro, pelo tempo que
// durar a trava (nao esquece sozinha - desligue vc mesmo ao trocar de volta,
// ha uma rede de seguranca so para quando o APP FECHA com a trava ligada).
//
// Mesmo problema do sistema_zerar_rolagem() acima, faltando a outra metade:
// zerar poe a rolagem em 0 uma vez, mas nao impede o dedo de mexer nela de
// novo no proximo arraste - so ISTO impede. Chame com true ao entrar numa
// tela interna que nao usa a rolagem do sistema (ex.: a galeria de fontes,
// que pagina) e com false ao sair dela.
void sistema_travar_rolagem(bool travar);

// Verdadeiro quando o sistema acabou de LIMPAR a area antes de chamar o
// desenhar() do app - na abertura, ao voltar do repouso, ao trocar de pagina.
// Um app que pinta por diferenca TEM que consultar isto e repintar tudo; se nao
// consultar, volta do repouso com a tela em branco.
bool sistema_area_foi_limpa(void);

// QUANTO TEMPO ESTE QUADRO REPRESENTA, em segundos - para quem anima por
// tempo. E a distancia entre as varreduras do painel em que este quadro e o
// anterior vao ao ar (kernel/ritmo.hpp), e nao entre os instantes em que
// foram desenhados: o que aparece andou exatamente o que ficou na tela.
// Chamar de dentro de desenhar(). Sempre > 0; limite como lhe convier (depois
// de uma pausa longa ele e grande).
float sistema_quadro_dt(void);

// A area de desenho do app - a MESMA que chega no desenhar().
//
// Existe porque o ao_tocar() so recebe x e y (ja relativos a area) e nao a area
// em si. Quem precisava dela - teclado na tela, barra ancorada no rodape -
// reconstruia copiando as alturas da barra de status e da navegacao de dentro
// do que hoje e kernel/internal.hpp (ALTURA_BARRA/ALTURA_NAV):
//
//     M5.Display.height() - 64 - 96      <-- era assim no app Mesh
//
// O perigo nao e o numero magico: e que o DESENHO usa a area de verdade e o
// TOQUE usava a copia. Mudar a altura de uma barra deixaria a tela certa e o
// dedo errado - falha silenciosa, que so aparece como "nao esta clicando".
//
// Use isto em qualquer lugar que precise da geometria fora do desenhar().
area_t sistema_area(void);

// SUPERFICIE PARA DMA. Um app que produz imagem por hardware
// (camera, e um dia video/JPEG) nao desenha pixel a pixel: o PPA escreve
// numa imagem do tamanho da tela, do proprio app, alocada no primeiro pedido
// e devolvida quando o app fecha. sistema_superficie() da o buffer (RGB565,
// o formato do painel, alinhado para DMA) e onde fica o retangulo 'r'
// (coordenadas de TELA, como as de area_t) dentro dele. So em retrato
// (rotacao 0) - false em paisagem. O retangulo precisa ter a LARGURA INTEIRA
// da tela e y/altura multiplos de 8: o PPA invalida o cache por linhas
// inteiras (ver tela_virtual.cpp). Quem escreveu avisa com
// sistema_superficie_mudou(r), NA TAREFA DE INTERFACE (no desenhar): e ai
// que a area vai, por DMA, para a pagina de tras do painel.
typedef struct {
  void*  buf;
  size_t bytes;
  int    pic_l, pic_a;   // a imagem inteira (a tela)
  int    x, y;           // o canto de 'r' dentro dela
} superficie_t;
bool sistema_superficie(const area_t& r, superficie_t* s);
void sistema_superficie_mudou(const area_t& r);

// Impoe no perfil tudo que o usuario escolheu e o sistema garante: brilho,
// radio, 5V do Grove. Chamada pelo boot, pela home e ao abrir cada app - se um
// caminho novo aplicar perfil, TEM que passar por aqui, senao a preferencia
// gravada nao vale naquele caminho (ver o comentario em kernel/core.cpp).
void sistema_impor_preferencias(tab5_perfil_t& p);

// Reaplica o perfil do app ATUAL (ou da home, se nenhum estiver aberto) com as
// preferencias de AGORA. E o caminho para um ajuste valer na hora, sem esperar
// a proxima troca de app.
//
// Nasceu de um bug: desligar o 5V do Grove em Ajustes nao apagava
// nada - o trilho so caia ao voltar para a home, quando o sistema remontava o
// perfil. O app Ajustes ligava na mao com tab5_power_grove(true) e o sentido
// contrario nao tinha par, entao "OFF" na tela e 5V no fio conviviam.
//
// Mexer no hardware na mao a partir de um app e a armadilha: quem sabe montar
// o estado final e o sistema, porque so ele conhece a regra de que o app SOMA e
// nunca subtrai. Se um dia o app Ajustes chamasse tab5_power_grove(false) com um app
// que precisa do Grove aberto, ele cortaria energia de baixo de quem estava
// usando. Passando por aqui isso e impossivel.
//
// So da tarefa de interface: mexe no expansor I2C.
// PALETA DO SISTEMA. Aqui e nao em kernel/core.cpp nem kernel/shell.cpp porque
// os apps precisam dela:
// uma tela que inventa o proprio cinza deixa de parecer parte do mesmo
// aparelho, e foi assim que a barra antiga acabou clara demais.
//
// TFT_DARKGREY do LovyanGFX NAO serve de moldura apesar do nome - ele e cinza
// MEDIO, rgb(123,123,123), e compete com o conteudo em vez de recuar.
// PREFIXADAS. Sem o "SIS_" o COR_BORDA daqui colidia com uma constante de
// mesmo nome dentro do app Cobra - macro em header compartilhado nao respeita
// escopo, e o erro sai no arquivo de quem nem sabia da existencia dela.
#define SIS_FUNDO   0x2124      /* rgb(36, 38, 40) - fundo de barra e coluna */
#define SIS_LINHA   0x39E7      /* rgb(56, 60, 60) - separador discreto      */
#define SIS_BORDA   0x5B0D      /* rgb(90, 96,104) - moldura de coluna       */

void sistema_reaplicar_perfil(void);

// Pede uma nova sondagem do Grove - para quando alguem TROCA o modulo do cabo
// com o aparelho ligado. Nao bloqueia: marca o pedido e o loop de interface o
// atende no ciclo seguinte, para a tela nao congelar sem explicacao.
// false = recusado porque um app esta usando a porta agora.
bool sistema_grove_redetectar(void);

// true enquanto a sondagem pedida ainda nao foi atendida. Serve para a tela
// dizer o que esta acontecendo durante os ~4s que ela leva.
bool sistema_grove_sondando(void);

// -------------------------------------------------------------------------
// CAMERA - pode ser chamada de QUALQUER tarefa
// -------------------------------------------------------------------------
// Ate aqui estas duas funcoes eram uma PONTE: o pedido esperava a tarefa de
// interface executar, porque o SCCB do sensor (driver I2C do IDF) e o resto
// do I2C interno (M5GFX) disputavam o mesmo controlador sem trava entre si -
// e isso ja tinha quebrado ("Set format regs fail"). O SCCB agora passa pelas
// transacoes do M5GFX, que travam a porta (hal/tab5_sccb.cpp): todo o I2C
// interno e serializado por UMA trava, inclusive o controle automatico de
// exposicao que o esp_video faz durante a captura. Ficam os nomes, para quem
// ja chama; por dentro, chamada direta.
bool sistema_camera_iniciar(int largura, int altura);
void sistema_camera_parar(void);

// TODOS OS DEDOS DA AMOSTRA ATUAL, para quem precisa de mais de um - o
// teclado virtual. O ao_tocar_nivel() so conta o primeiro dedo: digitando
// rapido, o dedo seguinte desce antes de o anterior subir, vira "o primeiro"
// quando ele sai, e a tecla anterior nunca via a soltura - a letra sumia.
// 'id' e o do controlador de toque: o mesmo dedo mantem o mesmo id enquanto
// estiver na tela. x/y no MESMO idioma do ao_tocar_nivel (area do app, com a
// rolagem somada); so os dedos dentro da area e pressionados NESTA amostra
// (o dedo que subiu some na primeira amostra sem ele). Valido durante o
// ao_tocar_nivel (a tarefa de interface amostra e entrega na mesma volta).
#define SISTEMA_TOQUES_MAX 5
typedef struct { uint8_t id; int16_t x, y; } sistema_toque_t;
int sistema_toques(sistema_toque_t* out, int max);

// O TOQUE EM CURSO JA FOI TRATADO POR NIVEL: a soltura dele nao vira clique.
// Chamar no ao_tocar_nivel, na mesma volta em que se trata o dedo. Sem isto
// o clique do sistema (que chega na MESMA volta da soltura, logo depois do
// nivel) caia no que estivesse embaixo do dedo DEPOIS do que o nivel fez -
// a tecla X do teclado fechava o teclado e o clique caia no botao de enviar
// da barra que desceu para o lugar dele. Vale ate o fim do toque.
void sistema_consumir_toque(void);

// BLUETOOTH LIGADO/DESLIGADO PELO USUARIO: grava a escolha (vale no proximo
// boot), liga ou desliga a stack e leva junto o Mesh quando ele esta
// configurado para o Bluetooth. Da tarefa de interface (ligar toca o I2C do
// trilho do C6).
void sistema_bluetooth(bool ligar);

// Liga o diagnostico de toque: cada clique imprime no log onde caiu, cru e
// convertido. Existe para medir o desvio do dedo em vez de adivinha-lo.
void sistema_diag_toque(bool ligar);
// BANCADA (console): um clique em x,y de TELA (como o dedo, barra e navegacao
// inclusive), e abrir um app (nullptr = voltar para a home). A interface
// executa na volta seguinte; acorda a tela se estiver em repouso.
void sistema_simular_toque(int x, int y);
void sistema_simular_abrir(const app_t* app);

// ORDEM DOS ICONES NA TELA INICIAL. SISTEMA_APPS (apps.cpp) e a ordem de
// fabrica; por cima dela vale a do usuario, gravada na NVS (ajustes.h) e
// mudada segurando um icone na home e arrastando, ou por 'ordem' no console.
// Posicoes de 0 a SISTEMA_NUM_APPS-1. Mover/padrao gravam e repintam a home.
const app_t* sistema_home_app(int pos);
int          sistema_home_posicao(const app_t* app);   // -1 = nao registrado
bool         sistema_home_mover(const app_t* app, int pos);
void         sistema_home_ordem_padrao(void);
void         sistema_home_apps_recarregar(void);   // ordem e ocultos da NVS

// ONDE A INTERFACE ESTA - para diagnosticar "travou sem log".
// O monitor vigia (loop parado > 3 s vira evento) e o console mostra ('ui').
typedef struct {
  const char* etapa;           // o passo do loop em que ela esta (ou parou)
  uint32_t    voltas;          // voltas completas desde o boot
  uint32_t    ms_desde_volta;  // ha quanto tempo a ultima terminou
  uint32_t    ms_desde_dedo;   // ha quanto tempo o toque leu um dedo (UINT32_MAX = nunca)
  // Vigia do toque (core.cpp): pulsos do GT911 desde o boot, falhas de I2C,
  // relatorios presos, segundos "surdos" (pulsos sem dedo lido), e o ultimo
  // registrador de estado lido (0x814E).
  uint32_t    toque_pulsos, toque_falhas, toque_presos, toque_surdos;
  uint8_t     toque_status;
  bool        repouso;
  const char* app;             // nullptr = home
} sistema_diag_t;
void sistema_diag_interface(sistema_diag_t* d);

// Trava/destrava o botao INICIO da barra de navegacao.
//
// Quem chama e o teclado virtual (tecvirt_abrir/tecvirt_fechar) - app nao
// precisa. O INICIO fica centralizado logo abaixo da tecla de espaco, e um
// toque que passa do alvo fecharia o app no meio da digitacao. Travada, a
// faixa de navegacao vira do app: o toque nela (clique e nivel) chega na
// ULTIMA linha da area, e o teclado pode encostar na barra. Travada, a
// barra de status tambem nao abre a central de avisos, e pedidos de fora
// para trocar de app (sistema_simular_abrir) sao recusados. O sistema destrava
// sozinho ao fechar qualquer app.
void sistema_travar_navegacao(bool travar);

// MANTER A TELA ACESA enquanto houver pedido - o repouso por inatividade nao
// acontece. Para trabalho que nao pode parar e que a pessoa acompanha na
// tela sem tocar nela. Um bit por motivo, de QUALQUER tarefa: o pedido de um
// nao solta o do outro. Um pedido que chega com a tela apagada a ACENDE (o
// repouso em curso tambem segura o clock no piso). O do app (SISTEMA_ACORDADO_APP) o sistema solta
// sozinho ao fechar qualquer app.
#define SISTEMA_ACORDADO_APP       (1u << 0)
#define SISTEMA_ACORDADO_PENDRIVE  (1u << 1)   // computador usando o cartao (services/tab5_pendrive)
void sistema_manter_acordado(uint32_t motivo, bool sim);

// O app em primeiro plano EXIGE o cartao (app_t.exige_sd). Seguro de
// qualquer tarefa: e o que o pendrive consulta antes de emprestar o cartao.
bool sistema_app_exige_sd(void);

// A tela de bloqueio esta na frente (home ou app por baixo). Seguro de
// qualquer tarefa (leitura de uma bandeira).
bool sistema_bloqueado(void);

// Nome do app em primeiro plano, ou NULL se estiver na tela inicial.
const char* sistema_app_atual(void);

// ---------------------------------------------------------------------------
// Inicializacao
// ---------------------------------------------------------------------------

// Perfil a usar no M5.begin(). E a UNIAO do que todos os apps registrados
// precisam que EXISTA - porque flags como internal_imu e internal_mic so podem
// ser decididas ANTES do begin, e nao da para inicializar o IMU depois.
// O que e chaveavel em runtime (trilhos de 5V, camera, radio, brilho) NAO entra
// aqui: isso e aplicado por app, na hora de abrir.
tab5_perfil_t sistema_perfil_de_boot(void);

// Cria a tarefa de interface. Chamar depois do M5.begin().
void sistema_iniciar(void);

// Tabela de aplicativos. Definida em apps.cpp a partir das pastas main/app_*
// (gerada no build - ninguem edita).
extern const app_t* const SISTEMA_APPS[];
extern const int         SISTEMA_NUM_APPS;
