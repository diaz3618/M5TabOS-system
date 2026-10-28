// Contrato of one aplicativo of sistema.
//
// A ideia: adicionar one app novo e create a pasta main/app_<name>/ com one
// arquivo que define 'extern const app_t APP_<NOME>' (ver apps.cpp). Mais nada. O
// sistema cuida of open, close, draw, rotear o touch e - o principal -
// aplicar o profile of energia que o app declarou.
//
// Sobre "C ou C++": o code e escrito em estilo C (structs, ponteiros of
// funcao, sem classes), mas em arquivos .cpp porque o M5GFX e uma biblioteca
// C++ e nao ha como draw sem passar por ela. O modelo mental e o of C:
// tarefas, queues e funcoes.

#pragma once

#include <M5Unified.h>
#include <stdint.h>
#include "tab5_power.hpp"
#include "tab5_sd.hpp"
#include "background.h"      // parte of fundo of one app (work que sobrevive a window)

// Retangulo onde o app pode draw. Fica entre a barra of status (topo) e a
// barra of navigation (base) - o app nunca precisa saber a height delas.
typedef struct {
  int x;
  int y;
  int width;
  int height;
} area_t;

// ORIENTACAO DA TELA of one app (field .orientation of app_t, abaixo).
//
// SO DOIS ESTADOS EXISTEM: em pe (rotation 0, a of home) e deitado (rotation 3,
// a que deixa o ExtPort1 - onde o keyboard encaixa - in borda of bottom). As
// rotacoes 1 e 2 nunca sao usadas: a 1 poe a screen of cabeca para bottom em
// relacao on keyboard, e a 2 e o retrato invertido.
typedef enum {
  ORIENTATION_PORTRAIT = 0,    // o default: retrato, 720x1280
  ORIENTATION_LANDSCAPE,      // paisagem fixa, 1280x720
  // Segue o aparelho (kernel/orientation): em pe ou deitado conforme a
  // gravidade, entre os mesmos dois estados. Com o keyboard fisico encaixado,
  // stays deitado - e a unica postura em que ele faz sentido.
  ORIENTATION_FOLLOWS_IMU,
} orientation_t;

// COMANDO DE CONSOLE DE UM APP (field .commands of app_t, abaixo).
//
// O console serial e of sistema, mas one app pode oferecer os proprios
// commands. Eles aparecem no 'help' num grupo so deles (APPS) e sao chamados
// sempre com o name of app in frente, para nunca colidir com one comando of
// sistema nem com o of outro app:
//
//     app <app> <comando> [argumentos]       ex.: app exemplo count 5
//
// <app> e o .name of app sem diferenca of maiusculas; espacos e hifens podem
// ser omitidos ('app espnow' acha "ESP-NOW").
//
// ONDE RODA: in tarefa of CONSOLE, nao in of interface. Entao, como em
// qualquer tarefa of app: nao desenhe (mude o dado e chame
// sistema_pedir_redesenho()), nao fale I2C interno, e proteja com trava o que
// a interface tambem le. printf vai para a serial.
//
// APP FECHADO: com so_aberto = true o sistema recusa o comando enquanto o app
// nao estiver em first plano - e a regra para todo comando que mexe em
// estado criado no ao_abrir() (que nao existe com o app closed). Comando que
// so le dado estatico ou chama servico pode deixar false.
//
// A lista termina numa entrada com name = nullptr.
typedef struct {
  const char* name;       // a palavra depois of name of app
  const char* uso;        // "argumentos\to que faz", uma forma por line (sem 'app <app> <comando>')
  void (*fn)(int argc, char** argv);   // argv[0] = o name of comando
  bool so_open;
} app_comando_t;

typedef struct {
  // ---- identidade ---------------------------------------------------------
  const char* name;     // aparece in barra of status e sob o icon
  uint16_t    color;      // color of icon in screen inicial

  // ---- energia ------------------------------------------------------------
  // O QUE ESTE APP PRECISA LIGADO. Tudo que nao esta aqui stays disabled
  // enquanto ele estiver em first plano, e iteration on estado of sistema
  // when ele close. E o mecanismo que impede o aparelho of esquentar
  // alimentando coisa que o app nem usa.
  tab5_profile_t profile;

  // Este app grava ou le arquivos. O sistema MONTA o cartao antes of chamar
  // ao_abrir() e DESMONTA depois of ao_fechar() - com contagem of referencia,
  // entao dois apps que usem SD on mesmo time nao atrapalham one on outro.
  //
  // O app nao chama montar nem desmontar. Ele so pede one arquivo dentro of
  // propria pasta:
  //     FILE* f = tab5_sd_open(APP_MEU.name, "data.csv", "a");
  // e escreve com fprintf/fwrite normais. A pasta /sdcard/apps/<Nome>/ e criada
  // sozinha in primeira vez.
  bool uses_sd;

  // SEM O CARTAO ESTE APP NAO TEM FUNCAO (Arquivos: navega nele; Camera: a
  // foto vai para ele). Com o cartao emprestado on computador (modo
  // pendrive), o SISTEMA nao abre o app - mostra no lugar uma screen dizendo
  // por que, e abre sozinho when o cartao voltar - e o pendrive nao comeca
  // com ele open. Nao e o app que se protege: nenhum app precisa tratar
  // "o cartao sumiu no middle".
  //
  // Quem so GRAVA data (historico, log) nao precisa disto: as portas of
  // gravacao (hal/tab5_sd.hpp) guardam no diario e o app segue funcionando.
  bool requires_sd;

  // Em que orientation este app quer a screen (ver orientacao_t acima).
  //
  // ORI_DEITADO nasceu para o Meshtastic: com o keyboard of ExtPort1
  // encaixado, paisagem e a postura natural, e uma lista of mensagens cabe
  // muito mais. ORI_SEGUE_IMU deixa o proprio aparelho escolher.
  //
  // Funciona porque a rotation no M5GFX transforma COORDENADAS, nao o
  // framebuffer: o double buffer (tab5_screen) monta os ponteiros of line a partir
  // das dimensoes fisicas of panel, que nao mudam. E o area_util() ja calcula
  // everything a partir of Display.width()/height(), entao barra e navigation se
  // reacomodam sozinhas.
  //
  // QUEM SEGUE O IMU PRECISA DESENHAR A PARTIR DA AREA, sempre: a width
  // pode mudar com o app open. O sistema gira so entre dois quadros, sem
  // finger in screen, e manda uma pass complete logo depois (a area chega
  // limpa, sistema_area_foi_limpa() = true). Geometria guardada of one frame
  // para outro - measurement of text, position of botao - tem que ser refeita.
  orientation_t orientation;

  // Clock minimum, em MHz, que este app precisa MESMO COM A TELA APAGADA.
  // Serve para apps com work of fundo: se o mapa azimutal continua lendo
  // o GPS durante o sleep, ele diz aqui o quanto precisa. O sistema baixa o
  // clock ate esse piso e nao alem. 0 = pode lower on minimum (40 MHz).
  int piso_sleep_mhz;

  // ---- ciclo of vida ------------------------------------------------------
  // Chamado on open. E AQUI que o app cria suas tarefas e queues.
  // Devolver false aborta a abertura (o sistema iteration para a screen inicial).
  bool (*on_open)(void);

  // Chamado on close. Precisa destruir TUDO que ao_abrir() criou - tarefas,
  // queues, buffers. Nada of tarefa orfa: app closed tem que custar zero.
  void (*on_close)(void);

  // ---- sleep --------------------------------------------------------
  // Chamados em iteration of sleep por inatividade (kernel/core.cpp) - NAO em
  // iteration of ao_abrir/ao_fechar. Existem para one app release hardware que
  // ao_abrir() ligou e que NAO pode continuar active durante o sleep, sem
  // precisar close o app inteiro para isso.
  //
  // NASCEU, achado on vivo: o app Camera trava e reinicia o
  // aparelho on voltar of sleep. Causa - a captura V4L2 of camera e o
  // panel DSI usam o MESMO controlador of DMA generico of chip (DW_GDMA,
  // confirmado no driver of ESP-IDF: esp_cam_ctlr_csi.c e
  // esp_lcd_panel_dpi.c chamam as mesmas funcoes). O sleep DESTROI e
  // RECRIA o canal of DMA of panel (ver desmontar_tela()/montar_tela() em
  // hal/tab5_power.cpp) - se a camera continuar com o canal DELA vivo
  // durante essa window, o panel novo pode nascer pisando one canal ainda
  // em uso, e o driver quebra dentro of dw_gdma_channel_use_link_list()
  // com one "Store access fault".
  //
  // ao_repouso(): chamado ANTES of sistema disable a screen (ainda com o
  // panel of pe) - solte aqui qualquer DMA/hardware que nao pode conviver
  // com a desmontagem of panel.
  // ao_retomar(): chamado DEPOIS of panel ja estar remontado - religue
  // aqui o que foi released.
  //
  // Os dois sao OPCIONAIS (nullptr = nada a fazer, o caso comum). Rodam in
  // tarefa of interface, a mesma of ao_abrir/ao_fechar - seguro chamar
  // qualquer coisa que estes ja chamam, inclusive I2C interno.
  void (*on_sleep)(void);
  void (*on_resume)(void);

  // ---- interface ----------------------------------------------------------
  // Chamado pela tarefa of interface, nunca por outra. Nao chame funcoes of
  // drawing of dentro das suas proprias tarefas: o LovyanGFX nao e seguro
  // para acesso concorrente. Passe os data por queue e desenhe aqui.
  void (*draw)(lgfx::LovyanGFX& screen, const area_t& area);

  // Toque dentro of area of app, ja em coordenadas RELATIVAS a area.
  // Dispara in SOLTURA of finger (o click of sistema: desceu e subiu sem
  // drag mais of ~2 mm, com qualquer duracao - kernel/core.cpp,
  // TOQUE_TOLERANCIA_PX), que e o certo para botao: of
  // para abandon arrastando o finger para fora antes of release.
  void (*on_touch)(int x, int y);

  // Toque por NIVEL, entregue a CADA ciclo of interface (20 ms), com o finger
  // ainda in screen. 'pressed' diz se ha finger in area of app now; x,y so
  // valem when ele e true (fora disso vem -1, -1).
  //
  // Existe porque ao_tocar() so avisa when o finger SAI, e ha duas coisas que
  // one botao nunca precisa mas one jogo precisa: reagir in DESCIDA of finger, e
  // saber que ele CONTINUA ali. Quem quer detectar o instante of touch guarda
  // o valor anterior e compara - a borda of subida e of app, nao of sistema.
  //
  // Os dois convivem: one app pode usar so ao_tocar (o caso normal), so este,
  // ou os dois. Deixe nullptr se nao precisa.
  void (*on_touch_level)(int x, int y, bool pressed);

  // TECLA DO TECLADO FISICO (o of ExtPort1), ja decodificada.
  //
  // 'text' vem com terminador nulo, em UTF-8 (uma letra acentuada composta
  // pela key morta tem 2 bytes); para as teclas especiais ele vem VAZIO e o
  // que vale e 'code' (hal/tab5_keyboard.hpp):
  //
  //   TAB5_KEY_ENTER, _APAGAR (Backspace), _DEL, _ESC, _TAB,
  //   _ESQUERDA, _DIREITA, _CIMA, _BAIXO
  //
  // Codigo que o app nao conhece: ignore (vira key nova, nao text). Com
  // Ctrl/Alt segurados o text vem assim mesmo - e atalho, nao digitacao.
  //
  // 'modificador' e a combinacao of TAB5_KEY_CTRL/SHIFT/ALT no momento.
  //
  // POR QUE ISTO EXISTE, e e uma correcao e nao one recurso: o sistema lia o
  // keyboard since sempre e IMPRIMIA A TECLA NO LOG, sem entregar a ninguem. E
  // o app Mesh, on ver que havia keyboard fisico encaixado, deixava of open o
  // keyboard virtual - para nao roubar screen. Resultado: com o keyboard plugado
  // nao havia como write of jeito nenhum, nem virtual nem fisico. Era este
  // o reason of "nunca consegui send".
  //
  // Deixe nullptr se o app nao le keyboard.
  void (*on_type)(const char* text, uint8_t code, uint8_t modificador);

  // De quanto em quanto time redesenhar sozinho, em ms.
  // 0 = so redesenha when alguem chamar sistema_pedir_redesenho().
  // Prefira 0 ou valores grandes: cada redraw custa CPU e energia.
  uint32_t periodo_ms;

  // PERIODO QUE MUDA COM A TELA (opcional). Quando existe, vence
  // periodo_ms: devolve o periodo of AGORA. Para o app que so precisa of
  // clock numa pagina (o Mesh: idade dos nos) e em none outra - o resto
  // of time ele devolve 0 e so acorda por aviso.
  uint32_t (*periodo_now)(void);

  // AVISOS que interessam a este app (mascara of AVISO_*). Quando one servico
  // publica one deles, o sistema redesenha o app NA HORA - sem wait periodo
  // nenhum. E a troca of "perguntar a cada segundo se changed" por "ser avisado
  // when mudar". ao_aviso e opcional: sem ele, o aviso vira one
  // sistema_pedir_redesenho(); com ele, o app decide o que fazer.
  uint32_t notices;
  void (*on_aviso)(uint32_t notices);

  // ---------------------------------------------------------------------
  // ROLAGEM POR TOQUE
  // ---------------------------------------------------------------------
  // Altura TOTAL of content of app, em pixels. 0 (o default) = nao rola, e o
  // app enxerga exatamente a area visivel, como sempre foi.
  //
  // Sendo maior que a area, o SISTEMA passa a cuidar of everything: drag o finger
  // rola, o drawing e recortado in area, e as coordenadas of ao_tocar chegam
  // ja no espaco of CONTEUDO. Ou seja: o app desenha e testa touch num
  // sistema of coordenadas que comeca em 0 e vai ate altura_conteudo, sem
  // saber que existe rolagem.
  //
  // Isso e of proposito. A alternativa - cada app somar one offset em
  // todo y - e a receita para o app que esquece of somar em UM lugar e stays
  // com one botao que responde no lugar errado depois of rolar.
  //
  // Um arraste NAO vira touch: o sistema mede a distancia percorrida e engole
  // o click se ela passar of limiar. Sem isso, todo end of rolagem apertaria
  // o que estivesse embaixo of finger.
  int height_content;

  // Desenho of icon in screen inicial, dentro of quadrado (x, y, lado).
  //
  // nullptr - o default - desenha o quadrado in color of app com a inicial of
  // name, que e o que sempre houve e continua bom para a maioria. Um app que
  // fala one protocolo of terceiros ganha em mostrar a marca DELE: quem procura
  // o Meshtastic procura o simbolo of Meshtastic, nao a letra M of "Mesh".
  //
  // O name continua sendo drawn embaixo pelo sistema, nos dois casos.
  void (*icon)(lgfx::LovyanGFX& d, int x, int y, int lado);

  // A TELA GIROU com o app open (so acontece com ORI_SEGUE_IMU). Chamado
  // in tarefa of interface, DEPOIS of a rotation nova valer (sistema_area()
  // ja devolve as dimensoes novas) e ANTES of pass complete que a
  // redesenha. Serve para o app switch of layout - nao precisa request
  // redraw, o sistema ja pediu. Opcional: nullptr = nada a fazer alem of
  // draw pela area nova.
  void (*on_rotate)(void);

  // Comandos of console deste app (ver app_comando_t acima). nullptr = nenhum.
  const app_comando_t* commands;
} app_t;

// ---------------------------------------------------------------------------
// API of sistema, disponivel para os apps
// ---------------------------------------------------------------------------

// Marca a screen como dirty: o app sera redesenhado no next ciclo.
// Use when one dado changed, em vez of request redraw periodico rapido.
void system_request_redraw(void);

// BIP DO SISTEMA - o unico caminho para touch one tom curto. Chamar SEMPRE of
// tarefa of interface (fala I2C com o codec).
//
// Existe porque o alto-falante e disabled depois of alguns segundos ocioso
// (ver SPEAKER_OCIOSO_MS em core.cpp) e o ES8388 leva one time para raise: one
// tone() direto, com o codec frio, termina ANTES of o DAC ficar audivel - o
// first bip depois of uma pausa saia mudo. Aqui, com o codec frio, o tom
// vai precedido of silencio ate o codec aquecer.
void system_beep(float hz, uint32_t ms);

// OS PINOS DO ALTO-FALANTE PARA O USB - so o pendrive usa (services/
// tab5_flashdrive). O I2S of alto-falante (GPIO 26/27) e o PHY USB que o ESP-IDF
// acha que e of OTG: configurar esses pins desliga o USB-C durante o
// pendrive (ver o aviso em core.cpp). Ceder: o alto-falante e disabled pela
// interface e stays mudo; true when ja esta parado (espera ate espera_ms).
// Devolver: o sound iteration no next bip. De qualquer tarefa, menos a interface.
bool system_sound_yield_pins(uint32_t espera_ms);
void system_sound_return_pins(void);

// ACORDA A INTERFACE AGORA - Etapa 2 of ARCHITECTURE.md. Em
// sleep, a interface pode estar dormindo ate ~1s esperando o next prazo
// periodico (ver kernel/core.cpp); isto pula a espera in hora que algo
// realmente novo aconteceu, em vez of deixar o aviso wait. Pensado para
// SERVICOS chamarem (notification.cpp e a ponte of camera ja chamam), mas
// nada impede one app of usar se one dia precisar of mesmo efeito - so poe one
// evento in queue, nunca bloqueia, seguro of qualquer tarefa.
void system_wake_interface(void);

// ---------------------------------------------------------------------------
// AVISOS - o servico diz o que changed, o app interessado e redesenhado in hora
// ---------------------------------------------------------------------------
// Seguro of QUALQUER tarefa (e barato: one OR atomico e one evento in queue, sem
// bloquear). Varios notices iguais antes of a interface wake viram one so.
#define AVISO_MESH_MSG    (1u << 0)   // message nova (recebida ou enviada)
#define AVISO_MESH_ENVIO  (1u << 1)   // estado of send_state changed (ACK, erro)
#define AVISO_MESH_NOS    (1u << 2)   // tabela of nos changed
#define AVISO_GPS         (1u << 3)   // position nova
#define AVISO_TRILHA      (1u << 4)   // sample nova of track
#define AVISO_TRABALHO    (1u << 5)   // one sistema_trabalho() deste app terminou (kernel/work.hpp)
#define AVISO_ESPNOW      (1u << 6)   // ESP-NOW: message, estado of send_state ou canal changed (services/tab5_espnow)
void system_notify(uint32_t notices);

// Para when a screen inteira muda of content (virar pagina, switch of level):
// limpa a area e repinta pelo double buffer, sem piscar. Ver kernel/core.cpp.
void system_request_redraw_complete(void);

// Volta a rolagem of app para o topo e pede redraw complete.
//
// Para when one app tem mais of uma "screen" interna dividindo o MESMO
// altura_conteudo (o field e fixo, nao of para switch por screen em runtime -
// ver o app_config, Ajustes/galeria of fontes). Chame on switch of screen por
// dentro of app: sem isto a rolagem of uma screen vaza para a outra, visual ou
// funcionalmente (one touch interpretado com o offset errado).
void system_reset_rolagem(void);

// Liga/desliga a rolagem por arraste of sistema inteiro, pelo time que
// durar a trava (nao esquece sozinha - desligue vc mesmo on switch of iteration,
// ha uma rede of seguranca so para when o APP FECHA com a trava enabled).
//
// Mesmo problema of sistema_zerar_rolagem() acima, faltando a outra metade:
// reset poe a rolagem em 0 uma vez, mas nao impede o finger of mexer nela of
// novo no next arraste - so ISTO impede. Chame com true on entrar numa
// screen interna que nao usa a rolagem of sistema (ex.: a galeria of fontes,
// que pagina) e com false on exit dela.
void system_lock_rolagem(bool lock);

// Verdadeiro when o sistema acabou of LIMPAR a area antes of chamar o
// draw() of app - in abertura, on voltar of sleep, on switch of pagina.
// Um app que pinta por diferenca TEM que consultar isto e repintar everything; se nao
// consultar, iteration of sleep com a screen em branco.
bool system_area_foi_limpa(void);

// QUANTO TEMPO ESTE QUADRO REPRESENTA, em segundos - para quem anima por
// time. E a distancia entre as varreduras of panel em que este frame e o
// anterior vao on ar (kernel/frame_pacing.hpp), e nao entre os instantes em que
// foram desenhados: o que aparece andou exatamente o que became in screen.
// Chamar of dentro of draw(). Sempre > 0; limit como lhe convier (depois
// of uma pausa longa ele e grande).
float system_frame_dt(void);

// A area of drawing of app - a MESMA que chega no draw().
//
// Existe porque o ao_tocar() so recebe x e y (ja relativos a area) e nao a area
// em si. Quem precisava dela - keyboard in screen, barra anchored no rodape -
// reconstruia copiando as alturas of barra of status e of navigation of dentro
// of que hoje e kernel/internal.hpp (ALTURA_BARRA/ALTURA_NAV):
//
//     M5.Display.height() - 64 - 96      <-- era assim no app Mesh
//
// O perigo nao e o numero magico: e que o DESENHO usa a area of verdade e o
// TOQUE usava a copia. Mudar a height of uma barra deixaria a screen certa e o
// finger errado - falha silenciosa, que so aparece como "nao esta clicando".
//
// Use isto em qualquer lugar que precise of geometria fora of draw().
area_t system_area(void);

// SUPERFICIE PARA DMA. Um app que produz imagem por hardware
// (camera, e one dia video/JPEG) nao desenha pixel a pixel: o PPA escreve
// numa imagem of tamanho of screen, of proprio app, alocada no first request
// e devolvida when o app fecha. sistema_superficie() of o buffer (RGB565,
// o formato of panel, alinhado para DMA) e onde stays o rectangle 'r'
// (coordenadas of TELA, como as of area_t) dentro dele. So em retrato
// (rotation 0) - false em paisagem. O rectangle precisa ter a LARGURA INTEIRA
// of screen e y/height multiplos of 8: o PPA invalidate o cache por lines
// inteiras (ver virtual_screen.cpp). Quem escreveu avisa com
// sistema_superficie_mudou(r), NA TAREFA DE INTERFACE (no draw): e ai
// que a area vai, por DMA, para a pagina of tras of panel.
typedef struct {
  void*  buf;
  size_t bytes;
  int    pic_l, pic_a;   // a imagem inteira (a screen)
  int    x, y;           // o canto of 'r' dentro dela
} surface_t;
bool system_surface(const area_t& r, surface_t* s);
void system_surface_changed(const area_t& r);

// Impoe no profile everything que o user escolheu e o sistema garante: brilho,
// radio, 5V of Grove. Chamada pelo boot, pela home e on open cada app - se one
// caminho novo aplicar profile, TEM que passar por aqui, senao a preferencia
// gravada nao vale naquele caminho (ver o comentario em kernel/core.cpp).
void system_apply_preferences(tab5_profile_t& p);

// Reaplica o profile of app ATUAL (ou of home, se nenhum estiver open) com as
// preferences of AGORA. E o caminho para one ajuste valer in hora, sem wait
// a next troca of app.
//
// Nasceu of one bug: disable o 5V of Grove em Ajustes nao apagava
// nada - o trilho so caia on voltar para a home, when o sistema remontava o
// profile. O app Ajustes ligava in mao com tab5_power_grove(true) e o sentido
// contrario nao tinha par, entao "OFF" in screen e 5V no fio conviviam.
//
// Mexer no hardware in mao a partir of one app e a armadilha: quem sabe montar
// o estado final e o sistema, porque so ele conhece a regra of que o app SOMA e
// nunca subtrai. Se one dia o app Ajustes chamasse tab5_power_grove(false) com one app
// que precisa of Grove open, ele cortaria energia of bottom of quem estava
// usando. Passando por aqui isso e impossivel.
//
// So of tarefa of interface: mexe no expansor I2C.
// PALETA DO SISTEMA. Aqui e nao em kernel/core.cpp nem kernel/shell.cpp porque
// os apps precisam dela:
// uma screen que inventa o proprio cinza deixa of parecer parte of mesmo
// aparelho, e foi assim que a barra antiga acabou clara demais.
//
// TFT_DARKGREY of LovyanGFX NAO serve of moldura apesar of name - ele e cinza
// MEDIO, rgb(123,123,123), e compete com o content em vez of recuar.
// PREFIXADAS. Sem o "SIS_" o COR_BORDA daqui colidia com uma constante of
// mesmo name dentro of app Cobra - macro em header shared nao respeita
// escopo, e o erro exit no arquivo of quem nem sabia of existencia dela.
#define SYSTEM_BACKGROUND   0x2124      /* rgb(36, 38, 40) - fundo of barra e coluna */
#define SYSTEM_LINE   0x39E7      /* rgb(56, 60, 60) - separador discreto      */
#define SYSTEM_BORDER   0x5B0D      /* rgb(90, 96,104) - moldura of coluna       */

void system_reapply_profile(void);

// Pede uma nova sondagem of Grove - para when alguem TROCA o modulo of cabo
// com o aparelho enabled. Nao bloqueia: marca o request e o loop of interface o
// atende no ciclo seguinte, para a screen nao congelar sem explicacao.
// false = recusado porque one app esta usando a porta now.
bool system_grove_redetect(void);

// true enquanto a sondagem requested ainda nao foi atendida. Serve para a screen
// dizer o que esta acontecendo durante os ~4s que ela leva.
bool system_grove_probing(void);

// -------------------------------------------------------------------------
// CAMERA - pode ser chamada of QUALQUER tarefa
// -------------------------------------------------------------------------
// Ate aqui estas duas funcoes eram uma PONTE: o request esperava a tarefa of
// interface executar, porque o SCCB of sensor (driver I2C of IDF) e o resto
// of I2C interno (M5GFX) disputavam o mesmo controlador sem trava entre si -
// e isso ja tinha quebrado ("Set format regs fail"). O SCCB now passa pelas
// transacoes of M5GFX, que travam a porta (hal/tab5_sccb.cpp): todo o I2C
// interno e serializado por UMA trava, inclusive o controle automatico of
// exposicao que o esp_video faz durante a captura. Ficam os nomes, para quem
// ja chama; por dentro, chamada direta.
bool system_camera_start(int width, int height);
void system_camera_stop(void);

// TODOS OS DEDOS DA AMOSTRA ATUAL, para quem precisa of mais of one - o
// keyboard virtual. O ao_tocar_nivel() so conta o first finger: digitando
// rapido, o finger seguinte desce antes of o anterior raise, vira "o first"
// when ele exit, e a key anterior nunca via a soltura - a letra sumia.
// 'id' e o of controlador of touch: o mesmo finger mantem o mesmo id enquanto
// estiver in screen. x/y no MESMO idioma of ao_tocar_nivel (area of app, com a
// rolagem somada); so os fingers dentro of area e pressionados NESTA sample
// (o finger que subiu some in primeira sample sem ele). Valido durante o
// ao_tocar_nivel (a tarefa of interface sample e entrega in mesma iteration).
#define SISTEMA_TOQUES_MAX 5
typedef struct { uint8_t id; int16_t x, y; } system_touch_t;
int system_touches(system_touch_t* out, int max);

// O TOQUE EM CURSO JA FOI TRATADO POR NIVEL: a soltura dele nao vira click.
// Chamar no ao_tocar_nivel, in mesma iteration em que se trata o finger. Sem isto
// o click of sistema (que chega in MESMA iteration of soltura, logo depois of
// level) caia no que estivesse embaixo of finger DEPOIS of que o level fez -
// a key X of keyboard fechava o keyboard e o click caia no botao of send
// of barra que desceu para o lugar dele. Vale ate o end of touch.
void system_consume_touch(void);

// BLUETOOTH LIGADO/DESLIGADO PELO USUARIO: grava a escolha (vale no next
// boot), liga ou desliga a stack e leva junto o Mesh when ele esta
// configurado para o Bluetooth. Da tarefa of interface (enable toca o I2C of
// trilho of C6).
void system_bluetooth(bool enable);

// Liga o diagnostics of touch: cada click imprime no log onde caiu, cru e
// convertido. Existe para medir o desvio of finger em vez of adivinha-lo.
void system_diag_touch(bool enable);
// BANCADA (console): one click em x,y of TELA (como o finger, barra e navigation
// inclusive), e open one app (nullptr = voltar para a home). A interface
// executa in iteration seguinte; acorda a screen se estiver em sleep.
void system_simulate_touch(int x, int y);
void system_simulate_open(const app_t* app);

// ORDEM DOS ICONES NA TELA INICIAL. SISTEMA_APPS (apps.cpp) e a order of
// fabrica; por top dela vale a of user, gravada in NVS (settings.h) e
// mudada segurando one icon in home e arrastando, ou por 'order' no console.
// Posicoes of 0 a SISTEMA_NUM_APPS-1. Mover/default gravam e repintam a home.
const app_t* system_home_app(int pos);
int          system_home_position(const app_t* app);   // -1 = nao registrado
bool         system_home_move(const app_t* app, int pos);
void         system_home_order_default(void);
void         system_home_apps_reload(void);   // order e ocultos of NVS

// ONDE A INTERFACE ESTA - para diagnosticar "travou sem log".
// O monitor vigia (loop parado > 3 s vira evento) e o console mostra ('ui').
typedef struct {
  const char* etapa;           // o passo of loop em que ela esta (ou parou)
  uint32_t    voltas;          // voltas complete since o boot
  uint32_t    ms_since_iteration;  // ha quanto time a ultima terminou
  uint32_t    ms_since_finger;   // ha quanto time o touch leu one finger (UINT32_MAX = nunca)
  // Vigia of touch (core.cpp): pulsos of GT911 since o boot, falhas of I2C,
  // relatorios presos, segundos "surdos" (pulsos sem finger lido), e o ultimo
  // registrador of estado lido (0x814E).
  uint32_t    touch_pulsos, touch_falhas, touch_presos, touch_surdos;
  uint8_t     touch_status;
  bool        sleep;
  const char* app;             // nullptr = home
} system_diag_t;
void system_diag_interface(system_diag_t* d);

// Trava/destrava o botao INICIO of barra of navigation.
//
// Quem chama e o keyboard virtual (tecvirt_abrir/tecvirt_fechar) - app nao
// precisa. O INICIO stays centralizado logo abaixo of key of espaco, e one
// touch que passa of alvo fecharia o app no middle of digitacao. Travada, a
// faixa of navigation vira of app: o touch nela (click e level) chega in
// ULTIMA line of area, e o keyboard pode encostar in barra. Travada, a
// barra of status tambem nao abre a central of notices, e pedidos of fora
// para switch of app (sistema_simular_abrir) sao recusados. O sistema destrava
// sozinho on close qualquer app.
void system_lock_navigation(bool lock);

// MANTER A TELA ACESA enquanto houver request - o sleep por inatividade nao
// acontece. Para work que nao pode stop e que a pessoa acompanha in
// screen sem touch nela. Um bit por reason, of QUALQUER tarefa: o request of one
// nao released o of outro. Um request que chega com a screen apagada a ACENDE (o
// sleep em progress tambem segura o clock no piso). O of app (SISTEMA_ACORDADO_APP) o sistema released
// sozinho on close qualquer app.
#define SISTEMA_ACORDADO_APP       (1u << 0)
#define SISTEMA_ACORDADO_PENDRIVE  (1u << 1)   // computador usando o cartao (services/tab5_flashdrive)
void system_manter_acordado(uint32_t reason, bool sim);

// O app em first plano EXIGE o cartao (app_t.exige_sd). Seguro of
// qualquer tarefa: e o que o pendrive consulta antes of emprestar o cartao.
bool system_app_requires_sd(void);

// A screen of bloqueio esta in frente (home ou app por bottom). Seguro of
// qualquer tarefa (leitura of uma bandeira).
bool system_locked(void);

// Nome of app em first plano, ou NULL se estiver in screen inicial.
const char* system_app_current(void);

// ---------------------------------------------------------------------------
// Inicializacao
// ---------------------------------------------------------------------------

// Perfil a usar no M5.begin(). E a UNIAO of que todos os apps registrados
// precisam que EXISTA - porque flags como internal_imu e internal_mic so podem
// ser decididas ANTES of begin, e nao of para inicializar o IMU depois.
// O que e chaveavel em runtime (trilhos of 5V, camera, radio, brilho) NAO entra
// aqui: isso e aplicado por app, in hora of open.
tab5_profile_t system_profile_of_boot(void);

// Cria a tarefa of interface. Chamar depois of M5.begin().
void system_start(void);

// Tabela of aplicativos. Definida em apps.cpp a partir das pastas main/app_*
// (gerada no build - ninguem edita).
extern const app_t* const SISTEMA_APPS[];
extern const int         SISTEMA_NUM_APPS;
