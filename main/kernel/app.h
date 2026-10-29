// Contrato of one aplicativo of sistema.
//
// The idea: add a new app and create the main/app_<name>/ folder with one
// file that defines 'extern const app_t APP_<NAME>' (see apps.cpp). Nothing else. O
// sistema cuida of open, close, draw, rotear o touch e - o principal -
// aplicar o profile of energia que o app declarou.
//
// Sobre "C ou C++": o code e escrito em estilo C (structs, ponteiros of
// funcao, sem classes), mas em arquivos .cpp porque o M5GFX e uma biblioteca
// C++ and there is no way to draw without going through it. The mental model and the of C:
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
  // Follow the device (kernel/orientation): standing or lying down according to
  // gravidade, entre os mesmos dois estados. Com o keyboard fisico encaixado,
  // stays lying down - it's the only posture in which it makes sense.
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
// WHERE IT RUN: in task of CONSOLE, not in of interface. So, as in
// any task of app: do not draw (change the data and call
//sistema_pedir_redesenho()), do not speak internal I2C, and protect with a lock what
// the interface also reads. printf goes to serial.
//
// APP FECHADO: com so_aberto = true o sistema recusa o comando enquanto o app
// is not in the foreground - this is the rule for every command that touches
// state created in ao_abrir() (which does not exist with the app closed). command that
// so le dado estatico ou chama servico pode deixar false.
//
// A lista termina numa entrada com name = nullptr.
typedef struct {
  const char* name;       // a palavra depois of name of app
  const char* uso;        // "arguments\to that does", one form per line (without 'app <app> <command>')
  void (*fn)(int argc, char** argv);   // argv[0] = o name of comando
  bool so_open;
} app_comando_t;

typedef struct {
  // ---- identidade ---------------------------------------------------------
  const char* name;     // aparece in barra of status e sob o icon
  uint16_t    color;      // color of icon in screen inicial

  // ---- energia ------------------------------------------------------------
  // WHAT THIS APP NEEDS ON. Everything that is not here stays disabled
  // enquanto ele estiver em first plano, e iteration on estado of sistema
  // when it closes. And the mechanism that prevents the device from heating up
  // alimentando coisa que o app nem usa.
  tab5_profile_t profile;

  // This app writes or reads files. The system MOUNT the card before calling
  // ao_abrir() e DESMONTA depois of ao_fechar() - com contagem of referencia,
  // so two apps that use SD at the same time do not interfere with one another.
  //
  // The app does not call for mounting or dismounting. It only asks for one file within
  // own folder:
  //     FILE* f = tab5_sd_open(APP_MEU.name, "data.csv", "a");
  // and writes with normal fprintf/fwrite. The folder /sdcard/apps/<Name>/ is created
  // sozinha in primeira vez.
  bool uses_sd;

  // WITHOUT THE CARD THIS APP HAS NO FUNCTION (Files: browse it; Camera:
  // photo goes to him). With the borrowed card on the computer (mode
  // pendrive), the SYSTEM does not open the app - instead it shows a screen saying
  // why, and it opens by itself when the card comes back - and the pendrive doesn't start
  // with it open. It is not the app that protects itself: no app needs to deal with
  // "the card disappeared in the middle".
  //
  // Those who only RECORD data (history, log) do not need this: the ports of
  // recordings (hal/tab5_sd.hpp) are saved in the diary and the app continues to work.
  bool requires_sd;

  // Em que orientation este app quer a screen (ver orientacao_t acima).
  //
  // ORI_DEITADO nasceu para o Meshtastic: com o keyboard of ExtPort1
  // docked, landscape and natural posture, and a list of messages fit
  // much more. ORI_SEGUE_IMU lets the device itself choose.
  //
  // It works because the rotation in M5GFX transforms COORDINATES, not the
  // framebuffer: o double buffer (tab5_screen) monta os ponteiros of line a partir
  // of the physical dimensions of the panel, which do not change. And area_util() already calculates
  // everything a partir of Display.width()/height(), entao barra e navigation se
  // reacomodam sozinhas.
  //
  // QUEM SEGUE O IMU PRECISA DESENHAR A PARTIR DA AREA, sempre: a width
  // can change with app open. The system rotates only between two frames, without
  // finger in screen, e manda uma pass complete logo depois (a area chega
  // limpa, sistema_area_foi_limpa() = true). Geometria guardada of one frame
  // para outro - measurement of text, position of botao - tem que ser refeita.
  orientation_t orientation;

  // Clock minimum, em MHz, que este app precisa MESMO COM A TELA APAGADA.
  // Serve para apps com work of fundo: se o mapa azimutal continua lendo
  // o GPS durante o sleep, ele diz aqui o quanto precisa. O sistema baixa o
  // clock up to this floor and not beyond. 0 = can lower on minimum (40 MHz).
  int sleep_clock_floor_mhz;

  // ---- ciclo of vida ------------------------------------------------------
  // Chamado on open. E AQUI que o app cria suas tarefas e queues.
  // Devolver false aborta a abertura (o sistema iteration para a screen inicial).
  bool (*on_open)(void);

  // Chamado on close. Precisa destruir TUDO que ao_abrir() criou - tarefas,
  // queues, buffers. Nada of tarefa orfa: app closed tem que custar zero.
  void (*on_close)(void);

  // ---- sleep --------------------------------------------------------
  // Called in iteration of sleep due to inactivity (kernel/core.cpp) - NOT in
  // iteration of ao_abrir/ao_fechar. Existem para one app release hardware que
  // ao_abrir() called and that it CANNOT remain active during sleep, without
  // precisar close o app inteiro para isso.
  //
  // BORN, found live: the Camera app crashes and restarts
  // device on return from sleep. Cause - the V4L2 of camera capture and the
  // panel DSI usam o MESMO controlador of DMA generico of chip (DW_GDMA,
  // confirmado no driver of ESP-IDF: esp_cam_ctlr_csi.c e
  // esp_lcd_panel_dpi.c chamam as mesmas funcoes). O sleep DESTROI e
  // RECRIA o canal of DMA of panel (ver desmontar_tela()/montar_tela() em
  // hal/tab5_power.cpp) - se a camera continuar com o canal DELA vivo
  // durante essa window, o panel novo pode nascer pisando one canal ainda
  // in use, and the driver breaks inside of dw_gdma_channel_use_link_list()
  // com one "Store access fault".
  //
  // ao_repouso(): chamado ANTES of sistema disable a screen (ainda com o
  // panel of pe) - drop here any DMA/hardware that cannot coexist
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
  // Called by the of interface task, never by another. Do not call functions of
  // drawing of within your own tasks: LovyanGFX is not safe
  // para acesso concorrente. Passe os data por queue e desenhe aqui.
  void (*draw)(lgfx::LovyanGFX& screen, const area_t& area);

  // Tap inside the area of the app, now in coordinates RELATIVE to the area.
  // Dispara in SOLTURA of finger (o click of sistema: desceu e subiu sem
  // drag mais of ~2 mm, com qualquer duracao - kernel/core.cpp,
  // TOQUE_TOLERANCIA_PX), que e o certo para botao: of
  // to abandon by dragging the finger out before release.
  void (*on_touch)(int x, int y);

  // Play by LEVEL, delivered at EACH cycle of interface (20 ms), with the finger
  // ainda in screen. 'pressed' diz se ha finger in area of app now; x,y so
  // are valid when it is true (otherwise -1, -1).
  //
  // Existe porque ao_tocar() so avisa when o finger SAI, e ha duas coisas que
  // one botao nunca precisa mas one jogo precisa: reagir in DESCIDA of finger, e
  // saber que ele CONTINUA ali. Quem quer detectar o instante of touch guarda
  // the previous value and compare - the edge of rising and of app, not of system.
  //
  // Os dois convivem: one app pode usar so ao_tocar (o caso normal), so este,
  // or both. Leave nullptr if you don't need it.
  void (*on_touch_level)(int x, int y, bool pressed);

  // PHYSICAL KEYBOARD KEY (of ExtPort1), already decoded.
  //
  // 'text' vem com terminador nulo, em UTF-8 (uma letra acentuada composta
  // for the dead key is 2 bytes); for the special keys it comes EMPTY and the
  // que vale e 'code' (hal/tab5_keyboard.hpp):
  //
  //   TAB5_KEY_ENTER, _APAGAR (Backspace), _DEL, _ESC, _TAB,
  //   _ESQUERDA, _DIREITA, _CIMA, _BAIXO
  //
  // Code that the app does not know: ignore (turns into a new key, not text). With
  // Ctrl/Alt held down the text comes like that - it's a shortcut, not typing.
  //
  // 'modifier' is the combination of TAB5_KEY_CTRL/SHIFT/ALT at the moment.
  //
  // WHY DOES THIS EXIST, and it is a fix and not a feature: the system read the
  // keyboard since always and PRINTED THE KEY IN THE LOG, without handing it over to anyone. And
  // o app Mesh, on ver que havia keyboard fisico encaixado, deixava of open o
  // virtual keyboard - so as not to steal screen. Result: with the keyboard plugged in
  // there was no way to write in any way, neither virtual nor physical. It was this
  // o reason of "nunca consegui send".
  //
  // Leave nullptr if the app does not read keyboard.
  void (*on_type)(const char* text, uint8_t code, uint8_t modificador);

  // How often do you redesign the team alone, in months.
  // 0 = so redesenha when alguem chamar sistema_pedir_redesenho().
  // Prefer 0 or large values: each redraw costs CPU and energy.
  uint32_t redraw_period_ms;

  // PERIODO QUE MUDA COM A TELA (opcional). Quando existe, vence
  // periodo_ms: devolve o periodo of AGORA. Para o app que so precisa of
  // clock numa pagina (o Mesh: idade dos nos) e em none outra - o resto
  // of time ele devolve 0 e so acorda por aviso.
  uint32_t (*current_redraw_period)(void);

  // AVISOS que interessam a este app (mascara of AVISO_*). Quando one servico
  // publica one deles, o sistema redesenha o app NA HORA - sem wait periodo
  // none. And the exchange of "ask every second if changed" for "be warned
  // when mudar". ao_aviso e opcional: sem ele, o aviso vira one
  // sistema_pedir_redesenho(); com ele, o app decide o que fazer.
  uint32_t notices;
  void (*on_notice)(uint32_t notices);

  // ---------------------------------------------------------------------
  // ROLAGEM POR TOQUE
  // ---------------------------------------------------------------------
  // TOTAL height of content of app, in pixels. 0 (the default) = does not scroll, and the
  // app enxerga exatamente a area visivel, como sempre foi.
  //
  // Sendo maior que a area, o SISTEMA passa a cuidar of everything: drag o finger
  // rola, o drawing e recortado in area, e as coordenadas of ao_tocar chegam
  // already in the space of CONTEUDO. In other words: the app draws and tests touch on a
  // system of coordinates that starts at 0 and goes to content_height, without
  // saber que existe rolagem.
  //
  // That's on purpose. The alternative - each app adds one offset into
  // todo y - e a receita para o app que esquece of somar em UM lugar e stays
  // com one botao que responde no lugar errado depois of rolar.
  //
  // A drag DOES NOT become a touch: the system measures the distance covered and swallows
  // o click se ela passar of limiar. Sem isso, todo end of rolagem apertaria
  // o que estivesse embaixo of finger.
  int height_content;

  // Design of icon in home screen, within square (x, y, side).
  //
  // nullptr - o default - desenha o quadrado in color of app com a inicial of
  // name, que e o que sempre houve e continua bom para a maioria. Um app que
  // fala one protocolo of terceiros ganha em mostrar a marca DELE: quem procura
  // Meshtastic looks for the symbol of Meshtastic, not the letter M of "Mesh".
  //
  // O name continua sendo drawn embaixo pelo sistema, nos dois casos.
  void (*icon)(lgfx::LovyanGFX& d, int x, int y, int lado);

  // A TELA GIROU com o app open (so acontece com ORI_SEGUE_IMU). Chamado
  // in tarefa of interface, DEPOIS of a rotation nova valer (sistema_area()
  // ja devolve as dimensoes novas) e ANTES of pass complete que a
  // redraws. Used to switch app layout - no request required
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
// is preceded by silence until the codec warms up.
void system_beep(float hz, uint32_t ms);

// OS PINOS DO ALTO-FALANTE PARA O USB - so o pendrive usa (services/
// tab5_flashdrive). O I2S of alto-falante (GPIO 26/27) e o PHY USB que o ESP-IDF
// acha que e of OTG: configurar esses pins desliga o USB-C durante o
// pendrive (ver o aviso em core.cpp). Ceder: o alto-falante e disabled pela
// interface and stays mute; true when it is already stopped (wait until wait_ms).
// Devolver: o sound iteration no next bip. De qualquer tarefa, menos a interface.
bool system_sound_yield_pins(uint32_t espera_ms);
void system_sound_return_pins(void);

// ACORDA A INTERFACE AGORA - Etapa 2 of docs/ARCHITECTURE.md. Em
// sleep, the interface may be sleeping up to ~1s waiting for the next deadline
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
// block). Several identical notices before the interface wake up become one so.
#define AVISO_MESH_MSG    (1u << 0)   // message nova (recebida ou enviada)
#define AVISO_MESH_ENVIO  (1u << 1)   // estado of send_state changed (ACK, erro)
#define AVISO_MESH_NOS    (1u << 2)   // table of nos changed
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
// content_height (the field is fixed, not for switching by screen at runtime -
// ver o app_config, Ajustes/galeria of fontes). Chame on switch of screen por
// inside the app: without this, the scrolling of one screen leaks to the other, visual or
// funcionalmente (one touch interpretado com o offset errado).
void system_reset_scroll(void);

// Liga/desliga a rolagem por arraste of sistema inteiro, pelo time que
// the lock lasts (don't forget it yourself - turn it off yourself on switch of iteration,
// there is a security network only for when the APP CLOSES with the lock enabled).
//
// Mesmo problema of sistema_zerar_rolagem() acima, faltando a outra metade:
// reset puts the scroll to 0 once, but doesn't stop the finger from moving it.
// novo no next arraste - so ISTO impede. Chame com true on entrar numa
// internal screen that does not use system scrolling (e.g. the font gallery,
// que pagina) e com false on exit dela.
void system_lock_scroll(bool lock);

// Verdadeiro when o sistema acabou of LIMPAR a area antes of chamar o
// draw() of app - in abertura, on voltar of sleep, on switch of pagina.
// An app that paints by difference MUST consult this and repaint everything; if not
// consultar, iteration of sleep com a screen em branco.
bool system_area_foi_limpa(void);

// QUANTO TEMPO ESTE QUADRO REPRESENTA, em segundos - para quem anima por
// time. E a distancia entre as varreduras of panel em que este frame e o
// previous go live (kernel/frame_pacing.hpp), and not between the moments when
// foram desenhados: o que aparece andou exatamente o que became in screen.
// Call of inside of draw(). Always > 0; limit as suits you (after
// of uma pausa longa ele e grande).
float system_frame_dt(void);

// A area of drawing of app - a MESMA que chega no draw().
//
// It exists because ao_tocar() only receives x and y (already related to the area) and not the area
// em si. Quem precisava dela - keyboard in screen, barra anchored no rodape -
// rebuild by copying the heights of bar of status and of navigation of inside
// of que hoje e kernel/internal.hpp (ALTURA_BARRA/ALTURA_NAV):
//
//     M5.Display.height() - 64 - 96      <-- era assim no app Mesh
//
// The danger is not the magic number: it is that the DRAWING uses the area of truth and the
// TOQUE usava a copia. Mudar a height of uma barra deixaria a screen certa e o
// wrong finger - silent failure, which only appears as "not clicking".
//
// Use this anywhere you need geometry outside of draw().
area_t system_area(void);

// SUPERFICIE PARA DMA. Um app que produz imagem por hardware
// (camera, and one day video/JPEG) does not draw pixel by pixel: the PPA writes
// in an image of screen size, of the app itself, allocated in the first request
// e devolvida when o app fecha. sistema_superficie() of o buffer (RGB565,
// o formato of panel, alinhado para DMA) e onde stays o rectangle 'r'
// (coordinates of TELA, like those of area_t) inside it. Only in portrait
// (rotation 0) - false em paisagem. O rectangle precisa ter a LARGURA INTEIRA
// of screen e y/height multiplos of 8: o PPA invalidate o cache por lines
// inteiras (ver virtual_screen.cpp). Quem escreveu avisa com
// sistema_superficie_mudou(r), NA TAREFA DE INTERFACE (no draw): e ai
// que a area vai, por DMA, para a pagina of tras of panel.
typedef struct {
  void*  buf;
  size_t bytes;
  int    pic_l, pic_a;   // a imagem inteira (a screen)
  int    x, y;           // the corner of 'r' inside her
} surface_t;
bool system_surface(const area_t& r, surface_t* s);
void system_surface_changed(const area_t& r);

// Impoe no profile everything que o user escolheu e o sistema garante: brilho,
// radio, 5V of Grove. Call at boot, at home and on open each app - if one
// caminho novo aplicar profile, TEM que passar por aqui, senao a preferencia
// recorded is not valid in that path (see the comment in kernel/core.cpp).
void system_apply_preferences(tab5_profile_t& p);

// Reaplica o profile of app ATUAL (ou of home, se nenhum estiver open) com as
// preferences of AGORA. E o caminho para one ajuste valer in hora, sem wait
// a next troca of app.
//
// Born from one bug: disabling the 5V of Grove in Settings did not go off
// nada - o trilho so caia on voltar para a home, when o sistema remontava o
// profile. O app Ajustes ligava in mao com tab5_power_grove(true) e o sentido
// otherwise had no pair, so "OFF" on screen and 5V on the wire coexisted.
//
// Tinkering with hardware by hand from one app is the trap: who knows how to assemble
// o estado final e o sistema, porque so ele conhece a regra of que o app SOMA e
// nunca subtrai. Se one dia o app Ajustes chamasse tab5_power_grove(false) com one app
// que precisa of Grove open, ele cortaria energia of bottom of quem estava
// usando. Passando por aqui isso e impossivel.
//
// So of tarefa of interface: mexe no expansor I2C.
// SYSTEM PALETTE. Here and not in kernel/core.cpp nor kernel/shell.cpp because
// os apps precisam dela:
// a screen that invents its own gray stops seeming part of itself
// device, and that's how the old bar ended up too light.
//
// TFT_DARKGREY of LovyanGFX DOES NOT serve as a frame despite the name - it is gray
// MEDIO, rgb(123,123,123), e compete com o content em vez of recuar.
// PREFIXADAS. Sem o "SIS_" o COR_BORDA daqui colidia com uma constante of
// same name inside Cobra app - macro in shared header does not respect
// scope, and the error exit in the file of those who didn't even know of its existence.
#define SYSTEM_BACKGROUND   0x2124      /* rgb(36, 38, 40) - fundo of barra e coluna */
#define SYSTEM_LINE   0x39E7      /* rgb(56, 60, 60) - separador discreto      */
#define SYSTEM_BORDER   0x5B0D      /* rgb(90, 96,104) - moldura of coluna       */

void system_reapply_profile(void);

// Pede uma nova sondagem of Grove - para when alguem TROCA o modulo of cabo
// with the device enabled. Does not block: marks the request and the loop of interface
// responds in the next cycle, so the screen does not freeze without explanation.
// false = recusado porque one app esta usando a porta now.
bool system_grove_redetect(void);

// true while the requested poll has not yet been answered. Used for the screen
// tell you what is happening during the ~4s it takes.
bool system_grove_probing(void);

// -------------------------------------------------------------------------
// CAMERA - pode ser chamada of QUALQUER tarefa
// -------------------------------------------------------------------------
// Until now, these two functions were a BRIDGE: the request expected the task of
// interface executar, porque o SCCB of sensor (driver I2C of IDF) e o resto
// of internal I2C (M5GFX) competed for the same controller without locking each other -
// e isso ja tinha quebrado ("Set format regs fail"). O SCCB now passa pelas
// transacoes of M5GFX, que travam a porta (hal/tab5_sccb.cpp): todo o I2C
// internal and serialized by ONE lock, including automatic control of
// exposure that esp_video makes during capture. Here are the names, for whom
// already calls; inside, direct call.
bool system_camera_start(int width, int height);
void system_camera_stop(void);

// ALL FINGERS FROM THE CURRENT SAMPLE, for those who need more of one - the
// keyboard virtual. O ao_tocar_nivel() so conta o first finger: digitando
// rapido, o finger seguinte desce antes of o anterior raise, vira "o first"
// when ele exit, e a key anterior nunca via a soltura - a letra sumia.
// 'id' e o of controlador of touch: o mesmo finger mantem o mesmo id enquanto
// estiver in screen. x/y no MESMO idioma of ao_tocar_nivel (area of app, com a
// scroll added); just the fingers inside the area and pressed on THIS sample
// (o finger que subiu some in primeira sample sem ele). Valido durante o
// ao_tocar_nivel (a tarefa of interface sample e entrega in mesma iteration).
#define SYSTEM_MAX_TOUCHES 5
typedef struct { uint8_t id; int16_t x, y; } system_touch_t;
int system_touches(system_touch_t* out, int max);

// THE TOUCH IN PROGRESS HAS ALREADY BEEN TREATLED BY LEVEL: releasing it does not become a click.
// Chamar no ao_tocar_nivel, in mesma iteration em que se trata o finger. Sem isto
// o click of sistema (que chega in MESMA iteration of soltura, logo depois of
// level) caia no que estivesse embaixo of finger DEPOIS of que o level fez -
// a key X of keyboard fechava o keyboard e o click caia no botao of send
// of barra who went down to his place. It's worth it until the end of touch.
void system_consume_touch(void);

// BLUETOOTH LIGADO/DESLIGADO PELO USUARIO: grava a escolha (vale no next
// boot), liga ou desliga a stack e leva junto o Mesh when ele esta
// configurado para o Bluetooth. Da tarefa of interface (enable toca o I2C of
// trilho of C6).
void system_bluetooth(bool enable);

// Turn on diagnostics of touch: each click prints in the log where it fell, raw and
// converted. It exists to measure the deviation of fingers rather than guessing it.
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
int          system_home_position(const app_t* app);   // -1 = not registered
bool         system_home_move(const app_t* app, int pos);
void         system_home_order_default(void);
void         system_home_apps_reload(void);   // order e ocultos of NVS

// ONDE A INTERFACE ESTA - para diagnosticar "travou sem log".
// The monitor watches (loop stopped > 3 s becomes an event) and the console shows ('ui').
typedef struct {
  const char* etapa;           // o passo of loop em que ela esta (ou parou)
  uint32_t    voltas;          // voltas complete since o boot
  uint32_t    ms_since_iteration;  // ha quanto time a ultima terminou
  uint32_t    ms_since_finger;   // ha quanto time o touch leu one finger (UINT32_MAX = nunca)
  // Monitor of touch (core.cpp): pulses of GT911 since boot, failures of I2C,
  // reports stuck, seconds "deaf" (pulses without fingers read), and the last
  // registrador of estado lido (0x814E).
  uint32_t    touch_pulsos, touch_falhas, touch_presos, touch_surdos;
  uint8_t     touch_status;
  bool        sleep;
  const char* app;             // nullptr = home
} system_diag_t;
void system_diag_interface(system_diag_t* d);

// Locks/unlocks the HOME button of the navigation bar.
//
// The caller is the virtual keyboard (tecvirt_abrir/tecvirt_fechar) - app no
// needs. The HOME stays centered just below the key of space, and one
// touch que passa of alvo fecharia o app no middle of digitacao. Travada, a
// faixa of navigation vira of app: o touch nela (click e level) chega in
// ULTIMA line of area, e o keyboard pode encostar in barra. Travada, a
// status bar also does not open the notice center, and requests from outside
// para switch of app (sistema_simular_abrir) sao recusados. O sistema destrava
// alone on close any app.
void system_lock_navigation(bool lock);

// KEEP THE SCREEN ON while there is a request - sleep due to inactivity does not
// happens. For work that cannot be stopped and that the person follows in
// screen sem touch nela. Um bit por reason, of QUALQUER tarefa: o request of one
// not released or otherwise. A request that arrives with the screen off turns ON (the
// sleep in progress also holds the clock to the floor). The of app (SISTEMA_ACORDADO_APP) the system released
// alone on close any app.
#define SYSTEM_KEEP_AWAKE_APP      (1u << 0)
#define SISTEMA_ACORDADO_PENDRIVE  (1u << 1)   // computer using card (services/tab5_flashdrive)
void system_manter_acordado(uint32_t reason, bool sim);

// The app in the foreground REQUIRES the card (app_t.exige_sd). Insurance
// any task: this is what the pendrive consults before lending the card.
bool system_app_requires_sd(void);

// A screen of bloqueio esta in frente (home ou app por bottom). Seguro of
// any task (reading a flag).
bool system_locked(void);

// Nome of app em first plano, ou NULL se estiver in screen inicial.
const char* system_app_current(void);

// ---------------------------------------------------------------------------
// Inicializacao
// ---------------------------------------------------------------------------

// Profile to use in M5.begin(). And the UNIAO of that all registered apps
// precisam que EXISTA - porque flags como internal_imu e internal_mic so podem
// be decided BEFORE begin, and not to initialize the IMU afterwards.
// What is switchable at runtime (5V rails, camera, radio, brightness) DOES NOT enter
// aqui: isso e aplicado por app, in hora of open.
tab5_profile_t system_profile_of_boot(void);

// Cria a tarefa of interface. Chamar depois of M5.begin().
void system_start(void);

// Table of applications. Defined in apps.cpp from the main/app_* folders
// (gerada no build - ninguem edita).
extern const app_t* const SISTEMA_APPS[];
extern const int         SISTEMA_NUM_APPS;
