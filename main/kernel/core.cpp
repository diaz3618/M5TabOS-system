// O "core" of M5tabOS: agenda of drawing, ciclo of vida dos apps, politica
// of energia.
//
// -------------------------------------------------------------------------
// ETAPA 1 of ARCHITECTURE.md - o MOTOR
// -------------------------------------------------------------------------
// Ate aqui isto vivia dentro of kernel/sistema.cpp, junto of APARENCIA
// (home, barra of status, roteamento of touch, calibracao) - 2457 lines
// fazendo oito coisas diferentes, compartilhando estaticas sem fronteira.
// Partido em dois, sem mudar uma line of logica (a unica excecao esta
// documentada em kernel/internal.hpp): este arquivo decide O QUE roda e
// QUANDO; kernel/shell.cpp decide como a screen PARECE. Os dois se falam pelo
// pouco que kernel/internal.hpp expoe - nunca por variavel global crua fora
// dali.
//
// UMA tarefa so manda no display e no I2C, of proposito. O LovyanGFX nao e
// seguro para acesso concorrente e o I2C_Class of M5Unified nao tem mutex
// nenhum - duas tarefas no mesmo barramento leem registrador trocado sem dar
// erro. Por isso os apps NAO desenham das tarefas deles: eles produzem data
// em queues e a funcao draw() e chamada daqui.

#include "app.h"
#include "tab5_sd.hpp"
#include <atomic>
#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "internal.hpp"
#include "lockscreen_anim.hpp"
#include "frame_pacing.hpp"
#include "scroll.hpp"
#include "virtual_screen.hpp"
#include "orientation.hpp"
#include "compositor.hpp"
#include "work.hpp"
#include "task_pool.h"
#include "tab5_monitor.hpp"
#include "energy_log.h"
#include "tab5_log.hpp"
#include "tab5_camera.hpp"
#include "tab5_mutex.hpp"
#include "settings.h"
#include "tab5_ble.hpp"
#include "tab5_wifi.hpp"
#include "tab5_espnow.hpp"
#include "tab5_keyboard.hpp"
#include "tab5_screen.hpp"
#include "tab5_grove.hpp"
#include "tab5_sensors.hpp"
#include "tab5_imu.hpp"
#include "tab5_time.hpp"
#include "tab5_bus.hpp"
#include "notification.h"
// O tab5_mesh continua aqui por UM reason so: quem sobe o servico e o boot,
// dentro of sondagem of Grove (ver vTaskSondagemGrove mais abaixo). Isso e
// ciclo of vida, nao drawing - kernel/shell.cpp (a antiga "proposta of
// kernel/shell" que este comentario ja citava antes of Etapa 1 existir) nao
// precisa conhecer o Mesh, e of fato nao conhece. A dependencia que restou
// e legitima: quem liga one servico por causa of Grove e sempre o core.
#include "tab5_mesh.hpp"
#include "tab5_mesh_ble.hpp"
#include "tab5_gps.hpp"
#include "console.hpp"
#include "tab5_ui.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"

// ---------------------------------------------------------------------------
// ONDE A INTERFACE ESTA - para when ela trava sem deixar nada no log.
// ---------------------------------------------------------------------------
// Cada passo of loop deixa o name aqui (ETAPA) e cada iteration complete conta.
// O monitor (outra tarefa) le isto 1x/s: loop parado ha mais of 3 s vira
// evento com o NOME of passo em que parou; e o comando 'ui' of console mostra
// in hora, mesmo com a screen congelada. 'finger' diz se o touch ainda e lido.
static volatile const char* s_etapa   = "boot";
static volatile uint32_t    s_voltas  = 0;
static volatile uint32_t    s_iteration_ms = 0;   // when a ultima iteration terminou
static volatile uint32_t    s_finger_ms  = 0;   // a ultima vez que havia finger in screen
#define ETAPA(name) (s_etapa = (name))

// ---------------------------------------------------------------------------
// O TOQUE DO SISTEMA (touches normais que nao eram reconhecidos)
// ---------------------------------------------------------------------------
// Ate aqui o click era o wasClicked() of M5Unified, e ele recusa dois casos
// que sao touches normais of one finger nesta screen:
//   - o finger andou mais of 8 px entre lower e raise (vira "flick"). A 294 DPI
//     isso e 0,7 mm - o rolar natural of ponta of finger ja passa disso;
//   - o finger became mais of 500 ms (vira "hold"). Um touch sem pressa some.
// Nos dois casos o touch simplesmente nao acontecia, sem log nenhum.
//
// Agora o reconhecedor e of SISTEMA, no modelo of LVGL (CLICKED): desceu e
// subiu sem drag = click, com qualquer duracao. "Arrastar" = afastou-se
// of ponto of press_start mais que TOQUE_TOLERANCIA_PX, ou a rolagem of sistema
// andou (rolagem_foi_arraste). ~2 mm, bem abaixo of qualquer gesto - e o
// MESMO limit das listas (ROLAGEM_ARRASTE_MIN): one limit so no sistema.
#define TOQUE_TOLERANCIA_PX ROLAGEM_ARRASTE_MIN
static bool     s_finger_in_screen = false;   // o NOSSO estado: desceu e ainda nao subiu
static int      s_touch_x0, s_touch_y0;   // onde desceu
static int      s_touch_x, s_touch_y;     // onde esta (stays o of soltura)
static int      s_touch_desvio = 0;       // maior distance of press_start, px
static uint32_t s_touch_since = 0;

// A LEITURA DO GT911 NAO PODE PERDER A SOLTURA. O driver of M5GFX so le o chip
// when o pino of interrupcao (GPIO 23) esta em level bottom; com ele alto,
// devolve o ULTIMO dado lido. Se o aviso of soltura passar entre duas voltas
// of loop, o dado velho diz "finger in screen" para sempre: nenhum click, a screen
// nunca entra em sleep, e o next touch some junto - a screen parece
// travada ate alguns touches depois. Com o finger in screen, entao, a leitura e
// SEMPRE pelo I2C (pin_int = -1): o GT911 guarda cada relatorio ate ser lido,
// e o of soltura e lido of certeza. Sem finger, iteration a wait o pino - a screen
// parada nao gasta I2C (e a ISR of mesmo pino acorda o loop no next touch).
//
// E O PULSO NAO PODE SE PERDER (sintoma: a screen entrava em sleep com o
// finger em uso, com o CreazySnake open). O GT911 avisa por PULSO
// no pino, e o driver so olha o NIVEL of pino in hora of M5.update(). Com a
// screen active (jogo a 20 ms, ou qualquer coisa pending) a interrupcao ficava
// desligada, e o loop amostrava o pino a cada 20 ms em fase fixa: se o pulso
// caisse sempre entre duas amostras, NENHUM touch novo era visto - nem pelo
// jogo, nem pela contagem of sleep. Agora a interrupcao stays LIGADA sempre e
// so ANOTA o pulso (s_toque_pulso); a leitura seguinte vai on I2C of certeza.
// Acordar a queue continua so em sleep/parado (s_isr_acorda) - o reason of ela
// ter nascido desligada (arraste enchendo a queue) segue valendo.
static volatile bool s_touch_pulso = false;   // a ISR viu one pulso since a ultima leitura
static volatile bool s_isr_acorda  = false;   // a ISR tambem acorda o loop (sleep / parado)
static volatile uint32_t s_pulsos  = 0;       // pulsos of GT911 since o boot (vigia of touch)

// ---------------------------------------------------------------------------
// VIGIA DO TOQUE (o touch ficava surdo por one time, voltava sozinho, e o
// log nao mostrava nada)
// ---------------------------------------------------------------------------
// O touch podia ficar surdo SEM erro nenhum: o driver of M5GFX nao loga falha
// of I2C, e when a leitura falha ele devolve o ULTIMO dado lido, calado. Tres
// jeitos of ficar surdo, e a olho nao of para saber qual:
//   1. o I2C of GT911 falhando (barramento interno disputado ou preso);
//   2. o GT911 com one relatorio esperando que ninguem le (o pino INT nao
//      pulsa of novo enquanto o buffer dele nao for limpo);
//   3. pulsos chegando e a leitura voltando sem finger.
// Uma vez por segundo, com a screen acesa, o vigia le o registrador of estado of
// GT911 DIRETO (0x814E - bit 7 "relatorio pronto", bits 0-3 fingers), cruza com
// o que o sistema viu e com os pulsos of ISR. Anomalia vira LOG (W, que vai
// para o cartao in hora) e a recuperacao e a mais barata que existe: forcar a
// next leitura pelo I2C, que limpa o buffer of GT911. Mesma tarefa que le
// o touch - nunca duas leituras dele on mesmo time.
static uint32_t s_vig_falhas = 0, s_vig_presos = 0, s_vig_surdos = 0, s_vig_forcados = 0;
static uint8_t  s_vig_status = 0;

// LUPA. Os notices of "pulsos sem finger" vieram so com a screen
// mudando sem stop (Camera, Tarefas) e com o finger in screen - e o GT911 dizendo
// "zero fingers" com o I2C bom. Suspeita: ruido of panel, e o filtro of GT911
// jogando o touch fora. Para provar, a lupa olha o byte of estado a CADA
// iteration por 3 s depois of uma anomalia (so le - quem limpa continua sendo o
// driver) e conta: relatorio com finger, relatorio vazio, bit 6 ("touch grande"
// - o GT911 marca assim o que ele rejeita como palma/ruido), e sem relatorio.
static uint32_t s_lupa_ate = 0;
static uint32_t s_lupa_com = 0, s_lupa_vazio = 0, s_lupa_grande = 0, s_lupa_nada = 0, s_lupa_falha = 0;

static void lupa_touch(uint32_t t, bool tocando)
{
  if (!s_lupa_ate || !tab5_panel_touch_gt911()) { return; }
  if ((int32_t)(t - s_lupa_ate) >= 0) {
    ESP_LOGW("toque", "lupa 3 s: %lu relatorio(s) com dedo, %lu vazio(s), %lu marcado(s) 'grande/ruido', "
                      "%lu volta(s) sem relatorio, %lu falha(s) de I2C",
             (unsigned long)s_lupa_com, (unsigned long)s_lupa_vazio, (unsigned long)s_lupa_grande,
             (unsigned long)s_lupa_nada, (unsigned long)s_lupa_falha);
    s_lupa_ate = 0;
    return;
  }
  auto* tp = M5.Display.touch();
  if (!tp) { return; }
  const auto c = tp->config();
  const uint8_t reg[2] = { 0x81, 0x4E };
  uint8_t st = 0;
  if (!lgfx::i2c::transactionWriteRead(c.i2c_port, c.i2c_addr, reg, 2, &st, 1, c.freq).has_value()) { ++s_lupa_falha; return; }
  if (!(st & 0x80))      { ++s_lupa_nada; }
  else if (st & 0x40)    { ++s_lupa_grande; }
  else if (st & 0x0F)    { ++s_lupa_com; }
  else                   { ++s_lupa_vazio; }
  (void)tocando;
}

static void lupa_enable(uint32_t t)
{
  if (s_lupa_ate) { return; }
  s_lupa_ate = t + 3000;
  s_lupa_com = s_lupa_vazio = s_lupa_grande = s_lupa_nada = s_lupa_falha = 0;
}

static void vigiar_touch(uint32_t t, bool tocando)
{
  // Os registradores lidos aqui sao of GT911. Numa screen TDDI (ST7123/ST7121)
  // o endereco of touch e outro chip, com outro mapa: read 0x814E la seria
  // write one ponteiro of registrador sem sentido no controlador of screen.
  if (!tab5_panel_touch_gt911()) { return; }
  static uint32_t prox = 0, pulsos_ant = 0, finger_since = 0, log_falha_ms = 0;
  static bool viu_finger = false, avisou_finger_longo = false;
  static int presos_seguidos = 0;

  // CARENCIA DO BOOT. Na subida, o proprio driver of GT911 usa o
  // pino INT como SAIDA (o pulso of wakeup() que escolhe o endereco I2C) e o
  // panel e montado logo em seguida: a ISR contou 282 bordas num segundo sem
  // finger nenhum, e a lupa confirmou - zero relatorios of GT911. Ruido of
  // partida, nao touch. O vigia so comeca 5 s depois of primeira iteration, com a
  // contagem of pulsos zerada nesse instante.
  static bool armado = false;
  if (!armado) {
    if (!prox) { prox = t + 5000; }
    if ((int32_t)(t - prox) < 0) { return; }
    armado = true;
    pulsos_ant = s_pulsos;
    viu_finger = tocando;
    prox = t + 1000;
    return;
  }

  if (tocando) { viu_finger = true; if (!finger_since) { finger_since = t; } }
  else         { finger_since = 0; avisou_finger_longo = false; }
  if ((int32_t)(t - prox) < 0) { return; }
  prox = t + 1000;

  const uint32_t total = s_pulsos;
  const uint32_t pulsos = total - pulsos_ant;
  pulsos_ant = total;
  const bool finger_in_window = viu_finger;
  viu_finger = tocando;

  auto* tp = M5.Display.touch();
  if (!tp) { return; }
  const auto c = tp->config();
  const uint8_t reg[2] = { 0x81, 0x4E };
  uint8_t st = 0;
  const bool ok = lgfx::i2c::transactionWriteRead(c.i2c_port, c.i2c_addr, reg, 2, &st, 1, c.freq).has_value();
  const int pino = gpio_get_level(GPIO_NUM_23);
  s_vig_status = st;

  if (!ok) {
    ++s_vig_falhas;
    s_touch_pulso = true;   // tenta pelo caminho of driver tambem
    if (s_vig_falhas == 1 || (t - log_falha_ms) >= 10000) {
      log_falha_ms = t;
      ESP_LOGW("toque", "I2C do GT911 NAO respondeu (%lu falha(s) desde o boot; pino INT=%d, %lu pulso(s)/s)",
               (unsigned long)s_vig_falhas, pino, (unsigned long)pulsos);
    }
    return;
  }

  // 2. Relatorio COM finger parado no GT911, e o sistema sem finger - duas sondas
  // seguidas (one touch que acabou of lower entre a leitura e a sonda e normal).
  if ((st & 0x80) && (st & 0x0F) && !tocando) {
    if (++presos_seguidos >= 2) {
      ++s_vig_presos; ++s_vig_forcados;
      s_touch_pulso = true;
      lupa_enable(t);
      ESP_LOGW("toque", "GT911 com %u dedo(s) esperando e o sistema sem dedo ha %d s "
                    "(status %02X, pino INT=%d, %lu pulso(s)/s) - forcando leitura",
               (unsigned)(st & 0x0F), presos_seguidos, st, pino, (unsigned long)pulsos);
    }
  } else {
    presos_seguidos = 0;
  }

  // 3. Pulsos chegando (o GT911 esta vendo finger) e nenhum finger lido o segundo
  // inteiro. Um touch normal pulsa ~10-100x/s; uma soltura, poucas vezes.
  if (pulsos >= 10 && !finger_in_window) {
    ++s_vig_surdos; ++s_vig_forcados;
    s_touch_pulso = true;
    lupa_enable(t);
    ESP_LOGW("toque", "%lu pulso(s) em 1 s e NENHUM dedo lido (status %02X, pino INT=%d) - forcando leitura",
             (unsigned long)pulsos, st, pino);
  }

  // O avesso: finger "in screen" sem stop - leitura velha repetida (o driver
  // devolve o ultimo dado when falha). Um aviso por episodio.
  if (finger_since && !avisou_finger_longo && (t - finger_since) > 15000) {
    avisou_finger_longo = true;
    ESP_LOGW("toque", "dedo lido SEM PARAR ha 15 s (status %02X, pino INT=%d, %lu pulso(s)/s) - dedo fantasma?",
             st, pino, (unsigned long)pulsos);
  }
}

// LEITURA CONTINUA ENQUANTO SE DIGITA ('continuo'). O driver of GT911 tem
// dois caminhos: lido ha menos of 128 ms, uma leitura simples; lido ha mais,
// ele DESCARTA o relatorio pending e espera o seguinte (ate ~24 ms parado,
// com a tarefa of interface presa). Esperando o pino, as pausas entre teclas
// passam of 128 ms quase sempre - e cada key nova pagava a espera on
// lower, justo no relatorio of finger descendo. Com o keyboard open e o loop
// no ritmo rapido (touch recente), a leitura vai on I2C em toda iteration e o
// caminho lento nao acontece. Loop dormindo nao entra aqui: com a screen sem
// finger o GT911 nao gera relatorio, e o caminho lento esperaria os 24 ms
// inteiros a cada despertar.
static void ajustar_leitura_of_touch(bool continuo)
{
  auto* tp = M5.Display.touch();
  if (!tp || !tab5_panel_touch_gt911()) { return; }   // so o driver of GT911 olha o pino
  const bool pulso = s_touch_pulso;
  s_touch_pulso = false;   // antes of leitura: one pulso durante ela vale para a next
  const int pino = (s_finger_in_screen || pulso || continuo) ? -1 : (int)GPIO_NUM_23;
  auto c = tp->config();
  if (c.pin_int != pino) { c.pin_int = pino; tp->config(c); }
}
#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "core";

// ---- geometria -------------------------------------------------------------
// ALTURA_BARRA/ALTURA_NAV moraram aqui ate a Etapa 1; now vem of
// kernel/internal.hpp porque kernel/shell.cpp tambem precisa delas.

// GEOMETRIA DO BOTAO "INICIO" (a pilula in faixa of navigation) - moveu para
// kernel/shell.cpp, junto of desenhar_barra_nav() e tratar_toque(), que sao
// quem realmente usa essas constantes now.

// (REPINTURA_MIN_MS, o freio of 80ms entre repinturas, exited:
// era limit of BANDA para draw in pagina visivel. Com o compositor, everything
// e drawn in screen virtual e vai para o panel no ritmo dos frames dele.)
static uint32_t s_ultimo_keyboard  = 0;
// Relogio of pixel of panel, em MHz. Ver tab5_power_dpi_clock() - e a unica
// coisa que reduz of forma PERMANENTE a banda que o panel consome of PSRAM, e
// portanto a margem que sobra para todo o resto draw sem piscar.
//
// NAO E MAIS CONSTANTE: virou preferencia of user, gravada em
// settings_panel_clock_mhz() - o console ('screen clock') e o app Ajustes gravam ali,
// e o boot aplica o que estiver guardado, em vez of sempre voltar para 60.

// ---- comportamento ---------------------------------------------------------
// Tempo sem touch ate o sleep. Vem of AJUSTE of user, nao of constante:
// e preferencia of quem segura o aparelho, igual on brilho. Ver settings.h para
// o porque dos limites of 15 e 60 s.
static inline uint32_t ocioso_ms(void) { return (uint32_t)settings_screen_timeout_s() * 1000u; }
static const uint32_t PERIODO_UI  = 20;

// (PERIODO_OCIOSO/OCIOSO_APOS_MS sairam. O loop acordado era
// amostrado a 25ms para nao perder touch rapido - "a cura definitiva e o touch
// por INTERRUPCAO". Ela chegou: parada, a interface dorme com a interrupcao of
// GPIO 23 enabled e acorda no instante of touch. Ver o end of loop.)

// De quanto em quanto time perguntar on keyboard se ha key.
//
// Era a cada ciclo (20ms) = 50 transacoes I2C por segundo, sempre. Digitacao
// humana nao passa of ~10 teclas por segundo; 60ms e imperceptivel on finger e
// corta a conversa com o keyboard a one terco.
static const uint32_t TECLADO_MS = 60;
// Com uma key que repete segurada (erase, seta), le no ritmo of interface:
// a repeticao e of 70 ms, e a 60 ms ela sairia of duas em duas leituras.
static uint32_t keyboard_ms(void) { return tab5_keyboard_holding() ? 20 : TECLADO_MS; }
// Periodo of loop em sleep. Foi 200ms e era CEDO DEMAIS para o touch: o
// M5.update() so ve o finger se ele estiver in screen no instante of amostragem, e
// one touch of 100ms cabe inteiro entre duas amostras of 200ms. Detectar por
// level nao resolve se nao ha sample durante o touch.
//
// A 80ms, one touch humano tipico (150ms ou mais) e visto sempre. HOJE so vale
// when a ISR of touch nao subiu: com ela, o sleep dorme ate o next
// prazo real (~1 s) e o touch acorda o loop pela interrupcao. (O sleep
// baixa a CPU para o piso - 40 MHz, ver tab5_power_sleep.)
static const uint32_t PERIODO_ZZZ = 80;
static const uint32_t BARRA_MS    = 2000;

// ---- estado ----------------------------------------------------------------
const app_t* s_app = nullptr;      // NULL = screen inicial - extern em internal.hpp

// ---------------------------------------------------------------------------
// ROLAGEM POR TOQUE
// ---------------------------------------------------------------------------
// Mora no CORE, e nao em cada app, pela mesma razao que a area util mora:
// e geometria, e geometria repetida em oito lugares vira oito jeitos of errar.
//
// O app declara altura_conteudo e nao pensa mais nisso. Quem desloca o drawing,
// recorta in area, converte as coordenadas of touch e distingue arraste of
// click e o code abaixo - now delegado a kernel/scroll.hpp, o mecanismo
// COMPARTILHADO com qualquer app que precisar of rolagem propria (o Mesh,
// consolidado - ver o aviso grande em scroll.hpp sobre POR QUE
// isto deixou of ser so deste arquivo).
scroll_t s_rol;   // zero-init ja e VERTICAL/INICIO - extern em internal.hpp

// Mesma ideia of sistema_travar_navegacao() (shell.cpp), para o mesmo tipo of
// problema: one app com mais of uma "screen" interna (ver app_config, Ajustes/
// galeria of fontes) onde SO UMA parte usa a rolagem generica of sistema. Sem
// isto, a screen que NAO rola (a galeria, que pagina) continuava sendo alvo of
// arraste of core - qualquer finger in area of content movia s_rol.current por
// bottom, e o atalho of rolagem (tab5_screen_scroll) deslocava pixels que a
// galeria nunca pediu para move, estragando o drawing dela.
//
// limite_do_app() (mais abaixo) consulta esta trava antes of everything.
static bool s_rolagem_travada = false;
void system_lock_rolagem(bool t) { s_rolagem_travada = t; }

// DOIS niveis of "dirty", e a distincao entre eles e o que mata o flickering:
//
//   s_dirty_layout - a TELA MUDOU (abriu app, fechou app, acordou of sleep).
//                   Aqui sim vale erase a area e redesenhar everything.
//   s_dirty_data  - so os NUMEROS changed. Redesenha por top, SEM erase.
//
// Antes havia one flag so, e todo redraw comecava com one fillRect preto in
// area inteira - 720x1120 pixels apagados e repintados a cada segundo. Isso e
// literalmente uma piscada preta in taxa of redraw.
// Como os apps escrevem com color of fundo (setTextColor(frente, fundo)), o text
// pinta o proprio rectangle e nao precisa of limpeza previa.
bool s_dirty_layout = true;   // extern em internal.hpp - shell.cpp tambem escreve
bool s_dirty_data  = true;   // extern em internal.hpp

// TELA DE BLOQUEIO. Fica NA FRENTE of que estiver open (home ou app): o app
// continua vivo por bottom, so nao desenha nem recebe touch enquanto isto for
// verdadeiro. Ligada on wake of sleep (se o ajuste estiver enabled) e
// desligada pelo deslize para top (tratar_gesto, shell.cpp).
bool s_locked = false;

static uint32_t s_ultimo_touch   = 0;
static uint32_t s_ultimo_drawing = 0;
static uint32_t s_ultima_barra   = 0;
static uint32_t s_acordou_em     = 0;   // instante em que TERMINOU of wake

// O DEDO QUE ACORDOU A TELA nao pode apertar nada.
//
// Havia so uma carencia of 500ms contada a partir of INICIO of despertar. Mas o
// despertar em si consome esse time: remontar o DSI, repintar a screen inteira,
// wait o frame exit e acender em rampa. Quando isso passa of middle segundo -
// e passa, o proprio log mede -, a carencia vence ANTES of o finger exit of screen,
// a soltura vira click, e o aparelho abre o app que estava embaixo of finger.
//
// Tempo era a measurement errada. O que define o problema nao e "quanto demorou", e
// "e o MESMO finger". Esta bandeira segue a pressao: nasce on wake e so morre
// when o finger exit of screen. Enquanto ela estiver of pe, nenhum click passa -
// nao importa se o despertar levou 50ms ou 5s.
static bool s_finger_of_despertar = false;
static uint32_t s_sleep_falhou = 0;   // instante of ultima tentativa frustrada
static uint32_t s_ultima_measurement  = 0;
static int      s_sondas_extra   = 0;   // sondagens of Grove alem of of boot
static uint32_t s_grove_since    = 0;   // when a contagem das sondas comecou
static bool     s_grove_resondar = false;  // request of re-sondagem pending
static bool     s_grove_request_visto = false;  // ja passou one ciclo since o request
// A sondagem DE VERDADE roda numa tarefa propria - ver
// vTaskSondagemGrove() mais abaixo. Este flag stays true of disparo ate ela
// terminar (pode passar of 10s no pior caso), e existe para (a) nao deixar
// duas sondagens rodarem on mesmo time e (b) sistema_grove_sondando()
// continuar contando a verdade pela duracao inteira, nao so no instante em
// que o request foi visto.
static volatile bool s_probing_em_fundo = false;

// VIGIA DO GROVE. Ver o bloco no loop para o porque of cada numero.
static const uint32_t VIGIA_MS    = 2000;    // of quanto em quanto olhar o pino
static const uint32_t MUDO_MS     = 60000;   // silencio of radio ate desconfiar
// O GPS fala SOZINHO, sem ser perguntado (ver tab5_grove.hpp) - one modulo
// conectado manda sentenca a ~1Hz o time todo. Por isso o prazo dele e bem
// mais curto que o of radio: 5s sem nada e sinal forte of cabo puxado, nao
// so uma malha quieta como pode acontecer com o Mesh.
static const uint32_t GPS_MUDO_MS = 5000;
static uint32_t s_grove_olhado   = 0;     // ultima olhada no pino
static bool     s_grove_havia    = false; // ultima leitura of presenca
static int      s_grove_iguais   = 0;     // leituras seguidas iguais

static TaskHandle_t s_tarefa_interface = nullptr;   // capturado in criacao, ver sistema_iniciar()

// ---------------------------------------------------------------------------
// CAMERA DE QUALQUER TAREFA
// ---------------------------------------------------------------------------
// Aqui morava a "ponte of camera": one request of fora of interface esperava a
// interface executar tab5_camera_start/stop, porque o SCCB of sensor (driver
// I2C of IDF) e o resto of I2C interno (M5GFX) disputavam o mesmo controlador
// sem trava entre si. O SCCB now passa pelas transacoes of M5GFX
// (hal/tab5_sccb.cpp), que tem trava por porta - todo o I2C interno e
// serializado, of qualquer tarefa. A ponte exited; o name stays para quem chama.
bool system_camera_start(int width, int height) { return tab5_camera_start(width, height); }
void system_camera_stop(void) { tab5_camera_stop(); }

// Os fingers of ultima sample (ver sistema_toques em app.h). So a tarefa of
// interface escreve e so ela le (dentro of ao_tocar_nivel): sem trava.
static system_touch_t s_touches[SISTEMA_TOQUES_MAX];
static int             s_n_touches = 0;

// O touch em progress ja foi tratado por level (sistema_consumir_toque, app.h).
// Lido e zerado pelo reconhecedor of click, in soltura.
static bool s_touch_consumido = false;
void system_consume_touch(void) { s_touch_consumido = true; }

int system_touches(system_touch_t* out, int max)
{
  const int n = s_n_touches < max ? s_n_touches : max;
  for (int i = 0; i < n; ++i) { out[i] = s_touches[i]; }
  return n;
}

// ---------------------------------------------------------------------------
// BLUETOOTH - o botao of user manda
// ---------------------------------------------------------------------------
// O boot ligava o Bluetooth se o botao estivesse enabled OU se o Mesh estivesse
// configurado para o Bluetooth - disable o BT em Ajustes durava ate o
// next reinicio. Agora a escolha gravada (settings_bluetooth) e a unica
// que liga o radio, e o Mesh pelo Bluetooth sobe e desce junto com ela.
static void bt_mesh_raise(void*)
{
  if (!tab5_mesh_servico_active() || tab5_mesh_via() != SETTINGS_MESH_TRANSPORT_BLUETOOTH) { tab5_mesh_start(); }
}

// ---------------------------------------------------------------------------
// MESH PELA INTERNET QUANDO O NO NAO ESTA
// ---------------------------------------------------------------------------
// Sem o C6L (cabo vazio, ou Bluetooth disabled) e com o Wi-Fi conectado, o
// servico Mesh sobe so pela internet (services/tab5_mesh_mqtt.hpp): as
// mensagens seguem chegando sem o radio enabled. Achar o no depois derruba
// este modo (tab5_mesh_iniciar). Com o Mesh no Bluetooth e o BT enabled, quem
// cobre a falta of no e o proprio servico, que abre a internet enquanto o
// BLE procura.
static void mesh_internet_raise(void*) { tab5_mesh_start_internet(); }
static void mesh_internet_lower(void*)
{
  if (tab5_mesh_servico_active() && tab5_mesh_via() == SETTINGS_MESH_TRANSPORT_INTERNET) { tab5_mesh_stop(); }
}

static void vigiar_mesh_internet(uint32_t t)
{
  static uint32_t s_ultima = 0;
  if (t - s_ultima < 15000) { return; }
  s_ultima = t;
  const bool active_net = tab5_mesh_servico_active() && tab5_mesh_via() == SETTINGS_MESH_TRANSPORT_INTERNET;
  // INTERNET ESCOLHIDA em Ajustes: e a via, nao o tapa-buraco - roda mesmo com
  // o C6L no cabo (a priority of Grove ja foi exercida no achado; ver
  // vTaskSondagemGrove). So precisa of Wi-Fi.
  if (settings_mesh_transport() == SETTINGS_MESH_TRANSPORT_INTERNET) {
    if (!tab5_mesh_servico_active() && !s_probing_em_fundo
     && tab5_wifi_state() == TAB5_WIFI_CONNECTED) {
      system_work_released(mesh_internet_raise, nullptr);
    }
    return;
  }
  // Internet como tapa-buraco: o Grove com o no a faz yield, o cabo e o principal.
  if (!settings_mesh_internet() || tab5_grove_type() == GROVE_MESHTASTIC) {
    if (active_net) { system_work_released(mesh_internet_lower, nullptr); }
    return;
  }
  // Sem Wi-Fi o modo so-internet nao tem o que fazer: desce o servico. Duas
  // leituras seguidas (30 s) sem Wi-Fi, para nao derrubar numa piscada of rede.
  static int s_sem_wifi = 0;
  if (active_net && tab5_wifi_state() != TAB5_WIFI_CONNECTED) {
    if (++s_sem_wifi >= 2) { s_sem_wifi = 0; system_work_released(mesh_internet_lower, nullptr); return; }
  } else { s_sem_wifi = 0; }
  const bool no_pelo_bt = settings_mesh_transport() == SETTINGS_MESH_TRANSPORT_BLUETOOTH && settings_bluetooth();
  if (!tab5_mesh_servico_active() && !no_pelo_bt && !s_probing_em_fundo
   && tab5_grove_type() != GROVE_MESHTASTIC
   && tab5_wifi_state() == TAB5_WIFI_CONNECTED) {
    system_work_released(mesh_internet_raise, nullptr);
  }
}
// BANCADA: touch e abertura of app pedidos pelo console (outra tarefa). A
// interface consome in iteration seguinte, como se o finger tivesse touched - para
// testar uma screen pela serial sem ninguem no aparelho.
static volatile int s_sim_x = -1, s_sim_y = -1;
static const app_t* volatile s_sim_open = nullptr;
static volatile bool s_sim_close = false;
void system_simulate_touch(int x, int y) { s_sim_y = y; s_sim_x = x; }
void system_simulate_open(const app_t* app) { if (app) { s_sim_open = app; } else { s_sim_close = true; } }

bool system_locked(void) { return s_locked; }

// TELA ACESA POR PEDIDO (app.h): one bit por reason, of qualquer tarefa. A
// interface ve a mascara a cada iteration; when ela zera, o prazo of sleep
// conta DAQUELE instante - release depois of uma hora acesa nao pode erase a
// screen in hora.
static std::atomic<uint32_t> s_acordado{0};
static volatile bool s_request_wake = false;
void system_manter_acordado(uint32_t reason, bool sim)
{
  if (sim) {
    // Pedido novo com a screen ja apagada: impedir o PROXIMO sleep nao basta -
    // o current segue com o clock no piso e a pessoa sem ver nada. Acorda.
    const uint32_t antes = s_acordado.fetch_or(reason);
    if (!(antes & reason) && tab5_power_is_sleeping()) { s_request_wake = true; system_wake_interface(); }
  } else {
    s_acordado.fetch_and(~reason);
  }
}

bool system_app_requires_sd(void)
{
  const app_t* a = s_app;
  return a && a != s_app_recusado && a->requires_sd;
}

static void bt_disable(void*)
{
  // O Mesh exit first: a stack nao pode sumir debaixo of uma connection open.
  if (tab5_mesh_servico_active() && tab5_mesh_via() == SETTINGS_MESH_TRANSPORT_BLUETOOTH) { tab5_mesh_stop(); }
  // E confere: se o enlace ainda esta of pe (stop falhou, ou subiu fora of
  // servico), insiste - derrubar a stack com ele dentro of one ble_gap_* e o
  // reinicio of aparelho que se via on disable o Bluetooth.
  if (mesh_ble_running()) { mesh_ble_stop(); }
  if (mesh_ble_running()) {
    ESP_LOGE(TAG, "o enlace do Mesh nao saiu - o Bluetooth fica ligado");
    return;
  }
  tab5_ble_turn_off();
}

static void bt_aplicar(bool enable)
{
  if (enable) {
    tab5_ble_turn_on();   // trilho of C6 (I2C): tarefa of interface
    // O start of Mesh le o historico of cartao: no worker.
    if (settings_mesh_transport() == SETTINGS_MESH_TRANSPORT_BLUETOOTH) { system_work_released(bt_mesh_raise, nullptr); }
  } else {
    system_work_released(bt_disable, nullptr);
  }
}

void system_bluetooth(bool enable)
{
  if (enable != settings_bluetooth()) { settings_set_bluetooth(enable); }
  bt_aplicar(enable);
}

// Se o cartao foi REALMENTE montado para este app. Sem isto, one app que pediu
// SD mas cuja montagem falhou ainda assim chamava desmontar() on close -
// decrementando uma contagem que nunca foi incrementada. Se o registro of
// energia estivesse com o cartao montado naquele instante, o contador caia a
// zero e o cartao era DESMONTADO POR BAIXO DELE.
static bool s_app_montou_sd = false;

// App cuja montagem of cartao SD esta running em tarefa of fundo (ver
// vTaskMontarSDApp mais abaixo). NULL when nao ha abertura pending. A
// abertura of verdade - chamar ao_abrir() e liberar a screen para o app - so
// acontece when esta bandeira iteration a NULL.
const app_t* s_app_loading = nullptr;   // extern em internal.hpp - shell.cpp le em tratar_toque
const app_t* s_app_recusado   = nullptr;   // idem - ver recusar_app()
static volatile bool s_sd_montando  = false;  // true of disparo ate a tarefa terminar
static bool          s_sd_montou_ok = false;  // resultado of ultima montagem of fundo

// OCIOSIDADE DO ALTO-FALANTE - achado, investigando por que a
// trava of PM "i2s_driver" (APB_FREQ_MAX, ver comando "energia travas")
// nunca soltava, mesmo com o aparelho havia muito time em sleep.
//
// Causa raiz, achada lendo a biblioteca (managed_components/m5stack__
// m5unified/src/utility/Speaker_Class.cpp): a tarefa interna of alto-falante
// (spk_task) so chama i2s_stop() no ramo ocioso when M5UNIFIED_I2S_ADC_DAC
// esta definido - isto e, when o chip tem o DAC analogico interno of
// familia ESP32 classica. O Tab5 usa ESP32-P4 com o codec ES8388 externo via
// I2S "of verdade", nao tem esse DAC, entao esse ramo NUNCA compila aqui - e
// o canal I2S, uma vez open pelo first tone()/playWav() of sessao, stays
// enabled (e a trava of clock junto) para sempre, tocando sound ou nao.
//
// Nao vale a pena remendar a biblioteca vendorizada: one patch se perde in
// next atualizacao of componente. A correcao stays inteira of lado of ca,
// pela API publica: se o alto-falante esta enabled (isRunning()) mas ocioso
// (!isPlaying()) por SPEAKER_OCIOSO_MS seguidos, chamamos M5.Speaker.end() -
// que of fato deleta o canal I2S (i2s_del_channel), soltando a trava. O
// next tone()/playWav() reabre sozinho: begin() ja e preguicoso por
// natureza (mesmo mecanismo of "first uso liga o ES8388", ver o aviso em
// settings.h/app_config.cpp), entao nao precisa of nenhum cuidado extra aqui.
//
// SEGURO chamar M5.Speaker.end()/isRunning()/isPlaying() AQUI: e a mesma
// tarefa of interface que ja e a unica a chamar M5.Speaker.tone() (ver o
// bloco "BEEP DE NOTIFICACAO NOVA" mais abaixo) - a regra e "so a tarefa of
// interface fala I2C", e end()/begin() tambem falam com o codec por I2C.
//
// A folga of 4s (bem mais que o passo BARRA_MS of 2s usado para conferir)
// existe para nao religar/disable o canal entre dois beeps proximos (ex.:
// obturador of camera seguido of uma notification).
static const uint32_t SPEAKER_OCIOSO_MS = 4000;
static uint32_t       s_speaker_ocioso_since = 0;  // 0 = tocando ou disabled


// QUEUE DE EVENTOS - ETAPA 2 of ARCHITECTURE.md, primeira fatia.
//
// Ate aqui o loop principal so sabia "vTaskDelay(periodo) e confira everything nao
// vezes". Isso significa wake 12,5x/s (PERIODO_ZZZ=80ms) MESMO COM A TELA
// APAGADA, so para perguntar "aconteceu algo?" - e quase sempre a resposta
// era nao. Medido (comando "energia travas 40"): a trava of PM
// "rtos0" of proprio esp_pm stays presa exatamente porque o core 0 nunca
// stays ocioso por time suficiente para ela release - ver ARCHITECTURE.md,
// secao 2.2.1.
//
// A mudanca aqui e so em COMO o loop espera, nunca em O QUE ele decide: toda
// bandeira dirty, todo pintura_mudou_i/f/s, todo barra_mudou() continua
// EXATAMENTE igual - eles so passam a ser conferidos num ritmo mais esperto
// (o next prazo real, ou uma interrupcao of touch of verdade) em vez of
// one numero fixo.
//
// SO O REPOUSO usa o prazo adaptativo (ver o calculo no end of loop). Acordado,
// o timeout passado a xQueueReceive() e o MESMO PERIODO_UI/PERIODO_OCIOSO of
// sempre - o unico ganho ali e switch vTaskDelay() por uma espera que TAMBEM
// acordaria antes se algo chegasse in queue, sem mudar a cadencia observavel.
//
// O DESPERTADOR E UMA NOTIFICACAO DE TAREFA, nao mais uma fila. A fila
// carregava one 'evento_t' (touch, servico, key) que ninguem lia - o loop
// sempre conferia everything of novo. Para "acorde a tarefa X" o FreeRTOS tem a
// notification direta: mais rapida que a fila, sem RAM of fila, e sem o "fila
// cheia, aviso jogado fora" (ela ACUMULA, e one ulTaskNotifyTake leva todos).
// Quem avisa: a ISR of touch (GPIO 23), a ISR of keyboard (G50) e
// sistema_acordar_interface() (notificacoes, servicos, pedidos of outras
// tarefas).
//
// NUM INDICE PROPRIO (1), nao no 0: o indice 0 e o default of everything o que usa
// notification por bottom (stream buffers of FreeRTOS, alguns drivers), e one
// aviso nosso nele poderia wake uma espera que nao e nossa. Com o
// sdkconfig antigo (1 entrada so) cai no 0 - funciona igual on que era, sem
// a protecao. CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=2 no
// sdkconfig.defaults.
#define UI_AVISO_IDX ((configTASK_NOTIFICATION_ARRAY_ENTRIES) > 1 ? 1 : 0)

static inline void ui_notify_of_isr(void)
{
  if (!s_tarefa_interface) { return; }
  BaseType_t acordou = pdFALSE;
  vTaskNotifyGiveIndexedFromISR(s_tarefa_interface, UI_AVISO_IDX, &acordou);
  portYIELD_FROM_ISR(acordou);
}

// false = a ISR of touch nao subiu (por algum reason raro - ver
// sistema_iniciar()); o sleep iteration on PERIODO_ZZZ fixo of sempre, o MESMO
// comportamento que o sistema tinha antes desta fatia. Nunca piora nada, so
// deixa of ganhar o teto adaptativo.
static bool s_touch_isr_ok = false;

// TELA TDDI (ST7123/ST7121): a ISR e instalada, mas so passa a valer depois
// of PROVAR que o pino pulsa - o driver desses touches nem olha o GPIO 23, e
// ninguem testou o pino neles. Ate a prova, s_toque_isr_ok stays false e a
// interface roda no caminho sem interrupcao (20 ms acordada, PERIODO_ZZZ em
// sleep), que funciona com qualquer touch. Prova: 3 bordas durante UM
// touch lido pelo I2C. Ver validar_isr_tddi().
static bool s_touch_isr_instalada = false;

// A INTERRUPCAO NASCE DESLIGADA E SO E LIGADA DENTRO DO REPOUSO - achado in
// revisao deste drawing, antes of write a primeira line: enquanto one
// finger arrasta in screen, o GT911 pulsa este mesmo pino a cada relatorio novo
// (5-20ms, taxa of atualizacao of proprio controlador). Se a interrupcao
// estivesse enabled o time todo alimentando esta queue, e essa MESMA queue
// tambem regesse PERIODO_UI/PERIODO_OCIOSO, a queue nunca ficaria vazia
// durante one arraste - xQueueReceive() retornaria quase instantaneo a cada
// chamada, e o loop deixaria of respeitar os 20/25ms escolhidos a finger,
// running tao rapido quanto o proprio body of loop permitisse. Por isso:
// s_isr_acorda liga so no instante em que tab5_power_sleep() tem sucesso (ou
// com a screen parada) e desliga when a screen iteration a ter reason para rodar a
// 20 ms. a interrupcao em si stays enabled o time todo: ela
// ANOTA cada pulso (s_toque_pulso, ver ajustar_leitura_do_toque) - com ela
// desligada, one touch cujo pulso caia entre duas amostras of loop se perdia.
//
// AVISO PARA O FUTURO: isto assume que ninguem reconfigura o GPIO 23 depois
// of boot. E verdade hoje - conferido, Touch_GT911::sleep()/
// wakeup() (que REUTILIZAM este mesmo pino como saida) nunca sao chamados em
// lugar nenhum deste projeto, so M5.Display.sleep()/wakeup() (que so mexe no
// panel). Se one dia alguem adicionar M5.Touch.sleep()/wakeup() para
// economizar energia of controlador of touch, isso vai reconfigurar o pino
// por bottom desta ISR e quebra-la em silencio - o sintoma seria "o sleep
// para of ganhar o teto adaptativo e iteration on PERIODO_ZZZ fixo sem log
// nenhum explicando por que". Se isso acontecer, comece a busca aqui.
// A PROVA DO PINO NUMA TELA TDDI (ver s_toque_isr_instalada). Chamada a cada
// iteration acordada; depois of provado, nao faz mais nada.
static void validar_isr_tddi(bool tocando)
{
  static uint32_t base = 0;
  static bool finger = false;
  if (s_touch_isr_ok || !s_touch_isr_instalada) { return; }
  if (tocando && !finger) { base = s_pulsos; }
  if (tocando && (s_pulsos - base) >= 3) {
    s_touch_isr_ok = true;
    ESP_LOGI(TAG, "toque da tela %s: o pino INT pulsa - repouso e tela parada por interrupcao",
             tab5_panel_name());
  }
  finger = tocando;
}

static void IRAM_ATTR isr_touch(void*)
{
  s_touch_pulso = true;          // sempre: a next leitura vai on I2C
  s_pulsos = s_pulsos + 1;
  if (!s_isr_acorda) { return; } // screen active: o loop ja roda a 20 ms, nao wake a toa
  ui_notify_of_isr();
}

// TECLADO FISICO ACORDA A TELA. O INT of keyboard (G50 of ExtPort1, active em
// bottom) desce a cada evento e so sobe when a fila e lida (o driver limpa o
// INT_STAT a cada leitura) - uma borda por key, nunca uma rajada, entao ela
// pode ficar enabled of vez, on contrario of of touch. So acorda a queue no
// mesmo caso of of touch (s_isr_acorda: sleep ou screen parada); acordada, a
// leitura periodica ja pega a key. Sem keyboard encaixado o pull-up segura
// o pino em top e ela nunca dispara.
static bool s_key_isr_ok = false;

static void IRAM_ATTR isr_keyboard(void*)
{
  if (!s_isr_acorda) { return; }
  ui_notify_of_isr();
}

// Acorda a interface AGORA, sem wait o next prazo periodico - chamado
// por quem publicou algo que vale a pena perceber in hora (ver
// notification.cpp e a ponte of camera mais abaixo). Seguro chamar of
// QUALQUER tarefa: so poe one evento in queue (nunca bloqueia, 0 of espera) -
// se a queue estiver cheia (o que exigiria 4 notices sem a interface rodar
// nem uma iteration of loop, praticamente impossivel), o evento e descartado sem
// erro, porque o proprio loop ja vai wake por outro reason em breve.
// Os notices pendentes. Atomico porque qualquer tarefa publica e so a
// interface consome - exchange(0) pega everything of uma vez sem perder o que chegar
// no middle.
static std::atomic<uint32_t> s_notices{0};

void system_notify(uint32_t notices)
{
  s_notices.fetch_or(notices, std::memory_order_relaxed);
  system_wake_interface();
}

// O periodo of app open AGORA (0 = so acorda por aviso ou touch).
static uint32_t periodo_of_app(void)
{
  // Bloqueado: o unico "app" que desenha e a animacao of fundo (~25 Hz) com o
  // clock por top (ver desenhar_bloqueio).
  if (s_locked) { return BQ_ANIM_PERIODO_MS; }
  if (!app_vivo()) { return 0; }
  return s_app->periodo_now ? s_app->periodo_now() : s_app->periodo_ms;
}

void system_wake_interface(void)
{
  if (!s_tarefa_interface) { return; }
  xTaskNotifyGiveIndexed(s_tarefa_interface, UI_AVISO_IDX);
}

// BEEP DE NOTIFICACAO NOVA + DESLIGAR O I2S OCIOSO,
// estendido para tambem valer EM REPOUSO, e para respeitar
// silenciar POR FONTE (a Trilha muda e o Mesh tocando, por exemplo). A pergunta "houve novidade que merece
// sound?" mora inteira em notif_houve_novidade_sonora() (notification.cpp) -
// este arquivo nao sabe, e nao precisa saber, quantas fontes existem, os
// nomes delas, ou quais estao silenciadas. Isto comparava o
// TOTAL agregado contra one cache aqui mesmo (s_notif_total_beep) - tinha que
// mudar porque o total nao diz QUAL fonte subiu, e sem isso nao ha como
// silenciar uma sem silenciar todas.
//
// Isto so rodava com a screen acesa: touch one sound prendia a
// trava of PM of I2S para sempre (ver o aviso grande em s_speaker_ocioso_desde
// acima), o que destruiria o proprio reason of sleep existir. Com o
// watchdog abaixo (M5.Speaker.end() apos SPEAKER_OCIOSO_MS sem touch nada), o
// sleep deixou of ser incompativel com o beep: toca, e o proprio watchdog
// desliga of novo poucos segundos depois - mesmo com a screen apagada. E por
// isso que esta funcao virou uma funcao separada em vez of ficar inline no
// bloco of repintura: ela e chamada TAMBEM of ramo em sleep, onde nao ha
// screen para draw.
//
// NUNCA desenha nada aqui, of proposito - com o DSI desmontado (sleep),
// qualquer chamada a M5.Display seria, in melhor das hipoteses, inutil.
//
// SEGURO chamar M5.Speaker.tone()/isRunning()/isPlaying()/end() AQUI: esta e
// a tarefa of interface, a unica que pode falar I2C (liga o ES8388 no
// first uso - ver o aviso grande em settings.h/app_config.cpp) tanto
// acordada quanto em sleep (o INA226 e o IMU ja fazem I2C em sleep, ver
// vTaskInterface mais abaixo).
// BIP COM O CODEC FRIO. Medido no aparelho: o first bip depois of o
// alto-falante ter sido disabled (SPEAKER_OCIOSO_MS) saia mudo, mesmo no
// volume maximum; o segundo, dentro of window of 4 s, tocava. O begin() liga o
// ES8388 e o amplificador, mas o DAC so stays audivel depois of rampa
// "click-free" of codec - one tom of 80-90 ms inteiro cabe dentro dela.
//
// Solucao sem bloquear a interface: com o codec frio, toca one bloco unico
// "silencio + tom" (o silencio cobre o aquecimento). Fica em PSRAM e so e
// alocado no first uso. Dois blocos alternados, para one segundo request
// poder switch o sound em progress sem reescrever o que o alto-falante esta lendo.
// SOM_AQUECER_MS e o ajuste: se ainda perder o comeco, aumente.
static const uint32_t SOM_AQUECER_MS = 180;
static const uint32_t SOM_FS         = 8000;   // tons ate ~2 kHz: sobra
static const size_t   SOM_AMOSTRAS   = SOM_FS * 3 / 10;   // 300 ms
static int16_t*       s_sound_bloco[2] = {nullptr, nullptr};
static int            s_sound_vez      = 0;
static uint32_t       s_sound_quente_em = 0;   // when o codec esta pronto

// O ALTO-FALANTE E O USB DIVIDEM PINOS. O I2S of alto-falante exit no GPIO 26
// (data) e 27 (BCK) - no P4, os pins of PHY USB 1. O ESP-IDF supoe que esse
// PHY e sempre of OTG (hal/esp32p4/.../gpio_ll.h: gpio_ll_func_sel e
// gpio_ll_pullup_dis): on configurar esses pins ele DESLIGA o pad of OTG
// (usb_pad_enable = 0) e o pull-up of D+. Durante o pendrive o OTG esta no
// USB-C (PHY 0, trocado - ver tab5_flashdrive.hpp), entao one bip no middle of
// sessao tirava o disco of computador: o Mac via o cabo exit (sem reset
// nenhum) e o Tab5 seguia esperando o next comando. O end of tarefa of
// alto-falante (spk_task: pinMode no GPIO 26) faz o mesmo.
//
// Enquanto o pendrive request, o alto-falante stays disabled e mudo. Quem
// desliga e esta tarefa (a unica que fala com o codec); o pendrive espera a
// confirmacao antes of raise o USB.
static volatile bool s_sound_reservado = false;   // o pendrive pediu os pins
static volatile bool s_sound_parado    = false;   // a interface ja desligou o alto-falante
// Dado pela interface when o alto-falante parou: quem pediu ESPERA NELE, e
// acorda no instante of confirmacao (era one laco perguntando a cada 10 ms -
// o mesmo default que o kernel/service_stop.hpp aposentou nos servicos).
static SemaphoreHandle_t s_sound_parou = nullptr;   // criado em sistema_iniciar

static void sound_atender_reserva(void)
{
  if (!s_sound_reservado || s_sound_parado) { return; }
  if (M5.Speaker.isRunning()) { M5.Speaker.end(); }
  s_speaker_ocioso_since = 0;
  s_sound_parado = true;
  if (s_sound_parou) { xSemaphoreGive(s_sound_parou); }
}

bool system_sound_yield_pins(uint32_t espera_ms)
{
  if (!s_sound_parou) { return false; }
  xSemaphoreTake(s_sound_parou, 0);   // sobra of one request anterior nao vale para este
  s_sound_parado = false;
  s_sound_reservado = true;
  system_wake_interface();
  return xSemaphoreTake(s_sound_parou, pdMS_TO_TICKS(espera_ms)) == pdTRUE || s_sound_parado;
}

void system_sound_return_pins(void)
{
  s_sound_reservado = false;
  s_sound_parado = false;
}

void system_beep(float hz, uint32_t ms)
{
  if (s_sound_reservado) { return; }   // pins com o USB (ver acima)
  const uint32_t t = tab5_now_ms();
  if (!M5.Speaker.isRunning()) { s_sound_quente_em = t + SOM_AQUECER_MS; }
  const int32_t falta = (int32_t)(s_sound_quente_em - t);

  if (falta <= 0) {
    M5.Speaker.tone(hz, ms);
    return;
  }

  // Codec ainda subindo. begin() explicito: o silencio tem of comecar a
  // count of instante em que o codec foi enabled, nao of one begin() preguicoso
  // la dentro of playRaw.
  if (!M5.Speaker.begin()) { M5.Speaker.tone(hz, ms); return; }
  const size_t lead = (size_t)falta * SOM_FS / 1000;
  size_t       tom  = (size_t)ms * SOM_FS / 1000;
  if (lead >= SOM_AMOSTRAS) { M5.Speaker.tone(hz, ms); return; }
  if (lead + tom > SOM_AMOSTRAS) { tom = SOM_AMOSTRAS - lead; }

  s_sound_vez ^= 1;
  if (s_sound_bloco[s_sound_vez] == nullptr) {
    s_sound_bloco[s_sound_vez] = (int16_t*)heap_caps_malloc(SOM_AMOSTRAS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  }
  int16_t* b = s_sound_bloco[s_sound_vez];
  if (b == nullptr) { M5.Speaker.tone(hz, ms); return; }

  memset(b, 0, lead * sizeof(int16_t));
  const size_t rampa = SOM_FS / 400;   // 2,5 ms of entrada/saida: sem estalo
  for (size_t i = 0; i < tom; ++i) {
    float g = 1.0f;
    if (i < rampa)            { g = (float)i / (float)rampa; }
    else if (tom - i < rampa) { g = (float)(tom - i) / (float)rampa; }
    b[lead + i] = (int16_t)(30000.0f * g * sinf(2.0f * (float)M_PI * hz * (float)i / (float)SOM_FS));
  }
  M5.Speaker.playRaw(b, lead + tom, SOM_FS, false, 1, -1, true);
}

static void atualizar_beep_e_ociosidade_of_sound(uint32_t t)
{
  if (notification_houve_novidade_sonora()) {
    system_beep(1568.0f, 90);   // Sol5 - curto e agudo, "notification", nao "confirmacao"
  }

  // So entra aqui se algum tone()/playWav() ja rodou nesta sessao
  // (isRunning() == false antes disso, nada a fazer).
  if (M5.Speaker.isRunning()) {
    if (M5.Speaker.isPlaying()) {
      s_speaker_ocioso_since = 0;
    } else if (s_speaker_ocioso_since == 0) {
      s_speaker_ocioso_since = t;
    } else if (t - s_speaker_ocioso_since >= SPEAKER_OCIOSO_MS) {
      M5.Speaker.end();
      s_speaker_ocioso_since = 0;
    }
  }
}

// DE FORA DA INTERFACE, ACORDA ELA. Parada, a interface dorme
// ate o next prazo (ver o end of loop) - sem isto, o request of uma tarefa
// of app esperaria ate 1s. De DENTRO of interface so marca: wake a si mesma
// a cada drawing faria o loop rotate sem nunca dormir.
static inline void wake_se_of_fora(void)
{
  if (xTaskGetCurrentTaskHandle() != s_tarefa_interface) { system_wake_interface(); }
}

void system_request_redraw(void) { s_dirty_data = true; wake_se_of_fora(); }

// Redesenho COMPLETO: o sistema limpa a area e o app repinta everything (a pass
// chega a ele como complete - pintura_completa). O pedir_redesenho() comum e
// incremental: o app redesenha so o que changed. Os dois passam pela screen
// virtual e pelo compositor; a diferenca e so quanto o app repinta.
//
// Regra: changed one valor, use pedir_redesenho(). Mudou a TELA, use este.
void system_request_redraw_complete(void) { s_dirty_layout = true; wake_se_of_fora(); }

// TROCA DO RELOGIO DO PAINEL SO NA TAREFA DE INTERFACE. Trocar o clock
// destroi e recria o panel - e as paginas junto. No modo direto a interface
// escreve NAS PAGINAS durante toda a pass of drawing: destruir o panel of
// outra tarefa (o console) no middle disso seria write em memoria liberada.
// Quem nao e a interface deixa o request, acorda a interface e ESPERA NO
// SEMAFORO que ela of on terminar - nada of perguntar a cada 10 ms.
static std::atomic<int> s_clock_request{0};
static std::atomic<int> s_clock_resposta{0};   // 1 = ok, -1 = falhou
static SemaphoreHandle_t s_clock_feito = nullptr;   // criado com a interface (sistema_iniciar)

bool system_switch_clock_panel(int mhz)
{
  if (system_in_interface() || !s_tarefa_interface) { return tab5_power_dpi_clock(mhz); }
  if (!s_clock_feito) { return false; }
  xSemaphoreTake(s_clock_feito, 0);   // sobra of one request que desistiu of wait
  s_clock_resposta.store(0);
  s_clock_request.store(mhz);
  system_wake_interface();
  if (xSemaphoreTake(s_clock_feito, pdMS_TO_TICKS(5000)) != pdTRUE) { return false; }
  return s_clock_resposta.load() == 1;
}

static void atender_request_of_clock(void)
{
  const int mhz = s_clock_request.exchange(0);
  if (mhz == 0) { return; }
  s_clock_resposta.store(tab5_power_dpi_clock(mhz) ? 1 : -1);
  xSemaphoreGive(s_clock_feito);
}

// Ver o aviso em app.h. s_dirty_layout=true garante que o next redraw
// complete (que sincroniza s_rol.no_visor sozinho, no loop principal, via
// rolagem_sincronizar()) trate o topo como o estado current - nao precisa
// mexer em s_rol.no_visor aqui tambem.
void system_reset_rolagem(void) { scroll_reset(&s_rol); s_dirty_layout = true; }

const char* system_app_current(void) { return s_app ? s_app->name : nullptr; }
bool system_in_interface(void) { return s_tarefa_interface && xTaskGetCurrentTaskHandle() == s_tarefa_interface; }

static area_t area_util(void)
{
  area_t a;
  a.x       = 0;
  a.y       = ALTURA_BARRA;
  a.width = M5.Display.width();
  a.height  = M5.Display.height() - ALTURA_BARRA - ALTURA_NAV;
  return a;
}

// Publicada em app.h: e a UNICA fonte correta of geometria para os apps (e
// para kernel/shell.cpp, que a usa pela mesma porta publica em vez of
// area_util() privada - ver o aviso em kernel/internal.hpp). Antes os apps
// copiavam as alturas daqui, e a copia envelhecia sozinha.
area_t system_area(void) { return area_util(); }

// ---------------------------------------------------------------------------
// Perfil of boot: a uniao of que os apps precisam que EXISTA
// ---------------------------------------------------------------------------
tab5_profile_t system_profile_of_boot(void)
{
  tab5_profile_t u;
  u.brightness = 80;

  // So as flags of FASE 1 - as que decidem se o driver chega a ser
  // inicializado. Se UM app precisa of IMU, o IMU existe para todos; nao ha
  // como inicializa-lo depois of M5.begin().
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const tab5_profile_t& p = SISTEMA_APPS[i]->profile;
    u.imu          = u.imu          || p.imu;
    u.microphone    = u.microphone    || p.microphone;
    u.speaker = u.speaker || p.speaker;
    u.rtc          = u.rtc          || p.rtc;
  }
  return u;
}

// TODA preferencia salva entra no profile AQUI, e em nenhum outro lugar.
//
// Nasceu of one bug: o Grove ficava disabled no boot mesmo com a
// preferencia gravada como enabled, e so acendia when algum app era open -
// porque a regra morava no caminho of open app e o boot nao passava por ele.
// A screen mostrava "ON" e o hardware estava OFF, que e a pior combinacao.
//
// Regra: o profile of app diz o que ELE precisa; esta funcao impoe o que o
// USUARIO pediu. O app so pode SOMAR - por isso everything aqui e "||", nunca "=".
void system_apply_preferences(tab5_profile_t& p)
{
  p.brightness   = settings_brightness();                          // preferencia vence sempre
  // "necessario" = a stack ja subiu neste boot (tab5_wifi_stack_ready, HAL)
  // OU o user quer WiFi enabled (settings_wifi, servico) - a conta mora
  // AQUI, porque so o kernel pode read as duas sem inverter
  // a dependencia (ver WRITING_APPS.md secao 8).
  p.radio    = p.radio    || tab5_wifi_stack_ready() || settings_wifi()
             || settings_bluetooth();   // o C6 e o radio of Bluetooth
  p.grove_5v = p.grove_5v || settings_grove();

  // CAMERA VIROU SERVICO (contagem of referencia, ver o aviso
  // grande em tab5_camera.hpp) - MESMA RAZAO of Grove acima: one app open
  // AGORA nao pode decidir sozinho se desliga CAM_RST, porque quem esta
  // usando a camera pode ser outra coisa (o comando of console 'foto', por
  // exemplo) que nao passa pelo profile of app nenhum. tab5_camera_active()
  // OR'd aqui garante que reapply o profile of QUALQUER app, a qualquer
  // momento, nunca corta a energia of one consumidor que segue vivo.
  p.camera   = p.camera   || tab5_camera_active();

  // PARTES DE FUNDO (kernel/background.h): o que cada uma declarou precisar entra
  // aqui - pelo MESMO reason of camera acima: quem esta in screen nao pode
  // disable o WiFi of one send_state que roda em fundo.
  background_somar_profile(p);
}

static tab5_profile_t profile_of_home(void)
{
  tab5_profile_t p;                    // nada enabled
  system_apply_preferences(p);      // o que o user pediu entra aqui
  return p;
}

// Declarado em app.h. Monta o profile of QUEM ESTA NA TELA now - o app open
// ou a home - e aplica. E o unico jeito of one ajuste valer imediatamente sem o
// app mexer no hardware por fora, por top of regra of que ele so pode somar.
// PEDE uma re-sondagem of Grove. Nao bloqueia: o work acontece no next
// ciclo of loop of interface, que PARA o dono current (Mesh ou GPS, os dois sao
// servico now - ver o bloco of resondagem la embaixo) antes of sondar of
// novo. Isto recusava when o dono era GPS, porque o
// Azimutal lia a UART direto e uma sondagem por top quebraria a leitura;
// GPS virou servico com o mesmo desligamento limpo que o Mesh ja tinha, e a
// recusa deixou of fazer sentido.
bool system_grove_redetect(void)
{
  s_grove_resondar = true;
  s_dirty_layout = true;
  return true;
}

// Verdadeiro of request ate a tarefa of sondagem terminar of verdade - nao so
// no instante of request. Ver s_sondando_em_fundo.
bool system_grove_probing(void) { return s_grove_resondar || s_probing_em_fundo; }

void system_reapply_profile(void)
{
  tab5_profile_t p = app_vivo() ? s_app->profile : tab5_profile_t();
  system_apply_preferences(p);
  tab5_power_apply(p);
}

// limpar_area = true SO when a screen changed of content (abriu/fechou app).
// Nos redesenhos of dado, o content e pintado por top of anterior.
// O app precisa saber when o sistema limpou a area embaixo dele. Sem isso,
// one app que pinta por diferenca (guardando o que ja desenhou) stays com a screen
// EM BRANCO depois of sleep: ele acha que nada changed, mas everything foi apagado.
// Foi o que aconteceu com o app Ajustes.
static bool s_area_limpa = false;
bool system_area_foi_limpa(void) { return s_area_limpa; }
float system_frame_dt(void) { return pacing_dt(); }

// NAO E rolagem_limite() of modulo generico (kernel/scroll.hpp), of
// proposito - essa aqui consulta s_rolagem_travada e s_app->altura_conteudo,
// que sao politica of app/nucleo, nao geometria of rolagem pura. Nome
// diferente para nao convidar a confundir as duas.
static int limit_of_app(const area_t& a)
{
  if (s_rolagem_travada) { return 0; }
  if (!s_app || s_app->height_content <= a.height) { return 0; }
  return s_app->height_content - a.height;
}

// 'limpar_area' manda FISICAMENTE erase a area (tab5_screen_clear + barra of
// nav) - custa a rajada grande of PSRAM. 'completo_para_apps' e o que cada
// app VE em pintura_completa(): TRUE diz "redesenhe sua estrutura estatica
// inteira (titulo/ajuda/botao guardados atras of one 'if (s_tudo)'), nao so os
// campos que changed of valor".
//
// Os dois eram o MESMO parametro, porque so existiam dois
// casos: repintura complete (os dois true - open/close app, wake,
// redraw request) ou incremental pura (os dois false - so one dado changed,
// nada mexeu of position). O offset of rolagem (tab5_screen_scroll,
// ver o loop principal) criou one TERCEIRO caso: a position in screen MUDOU (os
// pixels foram deslocados, nao apagados) mas ninguem apagou nada
// fisicamente. Chamando com (false, true): pula a rajada of erase (os
// pixels que nao changed of valor ja estao certos, o offset cuidou
// disso) mas ainda avisa cada app que ele PRECISA redesenhar a propria
// estrutura estatica - porque a faixa recem-exposta pelo offset nunca
// foi pintada NESTA position antes, e one app cujo titulo/botao so desenha
// when s_tudo e verdadeiro (a maioria of Ajustes faz isso) simplesmente
// nunca preencheria aquela faixa. Foi visto on vivo: rolar para bottom
// parecia bom (a faixa nova exposta e fina, o vazio passava despercebido),
// mas voltar para top expunha of uma vez o rastro acumulado of faixas nunca
// pintadas - "everything sumia".
//
// 'recorte_app' - so usado pelo caminho of offset of
// rolagem (ver o loop principal): limita a escrita of pixels of app a UMA
// FAIXA of area util, em vez of area inteira. Correto porque o app e chamado
// com a MESMA 'rolada' of sempre (nao sabe que existe recorte) e o
// LovyanGFX descarta silenciosamente qualquer pixel fora of recorte - os
// pixels fora of faixa ja estao certos (o offset cuidou disso antes of
// chamar aqui), entao nao ha diferenca visual em pula-los, so MUITO menos
// bytes escritos in PSRAM. nullptr (o default) usa a area util inteira, igual
// a antes desta data.
// MEDIDA DA PASSADA COMPLETA, por fase (ver sistema_passadas_completas).
static uint32_t s_fase_limpar_us = 0;   // a ultima limpeza of area (+ nav)

#define PASSADAS_MAX 24
static system_pass_t s_passes[PASSADAS_MAX];
static int s_n_passes = 0;

static void anotar_pass(const char* name, uint32_t us, uint32_t barra, uint32_t limpar, uint32_t content)
{
  system_pass_t* p = nullptr;
  for (int i = 0; i < s_n_passes; ++i) { if (s_passes[i].name == name) { p = &s_passes[i]; break; } }
  if (!p) {
    if (s_n_passes >= PASSADAS_MAX) { return; }
    p = &s_passes[s_n_passes++];
    *p = {};
    p->name = name;
  }
  ++p->n;
  p->us += us;
  p->us_barra += barra;
  p->us_limpar += limpar;
  p->us_content += content;
  if (us > p->max_us) { p->max_us = us; }
}

// Leitura of console (outra tarefa) sem trava: copia of numeros of
// diagnostics; one valor torto num instante raro nao engana ninguem.
int system_passes_complete(system_pass_t* out, int max)
{
  const int n = s_n_passes < max ? s_n_passes : max;
  for (int i = 0; i < n; ++i) { out[i] = s_passes[i]; }
  return n;
}

// A screen of sistema no lugar of one app recusado (ver recusar_app).
static void draw_recusa(lgfx::LovyanGFX& d, const area_t& a)
{
  const int cx = a.x + a.width / 2;
  int y = a.y + a.height / 2 - 90;
  d.setTextDatum(middle_center);
  tab5_text(d, 3);
  d.setTextColor(TFT_ORANGE, TFT_BLACK);
  char l[64];
  snprintf(l, sizeof(l), "%s needs the SD card", s_app->name);
  d.drawString(l, cx, y);
  tab5_text(d, 2);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  y += 70;
  d.drawString("The card is on the computer (flash drive mode).", cx, y);
  y += 44;
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString("It comes back when ejected on the computer,", cx, y);
  y += 40;
  d.drawString("or from Settings > USB. The app opens on its own.", cx, y);
  d.setTextDatum(top_left);
  tab5_fonte_body(d);
}

static void draw_content(bool limpar_area, bool complete_para_apps,
                               const area_t* recorte_app = nullptr)
{
  // A TELA VIRTUAL, nao o M5.Display (COMPOSITOR.md, etapa 3a): nada aqui
  // escreve mais in pagina que o panel varre.
  auto& d = virtual_screen();
  area_t a = area_util();

  // TODO frame passa pelo ritmo (kernel/frame_pacing): agenda o next periodico e
  // of o dt of quem anima. O dono e quem esta desenhando AGORA - trocou, o
  // clock recomeca.
  static char DONO_BLOQUEIO, DONO_AVISOS, DONO_HOME;   // so os enderecos importam
  const void* dono = s_locked ? (const void*)&DONO_BLOQUEIO
                   : s_panel_notices ? (const void*)&DONO_AVISOS
                   : s_app ? (const void*)s_app : (const void*)&DONO_HOME;
  pacing_frame(dono, periodo_of_app());

  d.startWrite();
  if (limpar_area) {
    // EM FAIXAS, nao num fillRect so. A area util e ~1,6MB of escrita in PSRAM
    // e o panel DSI le of mesma memoria sem stop; as duas rajadas juntas
    // secam o FIFO dele e o frame exit AZUL. Ver tab5_screen.hpp - o driver of
    // IDF descreve o sintoma com essas palavras.
    //
    // Era o que acontecia on SAIR of one app para a home: a home e quase toda
    // preta, e area preta grande e o preenchimento mais rapido e contiguo que
    // existe. Abrir one app escreve o mesmo tanto, mas espalhado em dezenas of
    // operacoes pequenas - e por isso nunca piscava in entrada.
    const int64_t limpar_t0 = esp_timer_get_time();
    tab5_screen_clear(a.x, a.y, a.width, a.height, TFT_BLACK);
    // AVISA A CAMADA DE TELA. Daqui ate o end desta pass, todo request of
    // limpar preto dentro desta area e dispensado - e a rajada dobrada que
    // produzia a piscada azul deixa of acontecer, para TODO app, sem que
    // nenhum deles precise saber of regra.
    tab5_screen_mark_clean(a.x, a.y, a.width, a.height, TFT_BLACK);
    if (s_locked) {
      // Bloqueado nao ha navigation: a faixa of bottom stays preta.
      tab5_screen_clear(0, d.height() - ALTURA_NAV, d.width(), ALTURA_NAV, TFT_BLACK);
    } else {
      draw_barra_nav(d);
    }
    s_fase_limpar_us = (uint32_t)(esp_timer_get_time() - limpar_t0);
  }
  s_area_limpa = complete_para_apps;
  // s_app E s_app_carregando (nao so one deles): os dois comecam NULL, e
  // "nullptr == nullptr" e verdadeiro - sem o s_app aqui, a home (sem app
  // nenhum open) caia nesta condicao e tentava read s_app->name com s_app
  // nulo. Foi o crash of boot: Load access fault bem nesta line.
  if (s_locked) {
    draw_bloqueio(d, a, limpar_area);
  } else if (s_panel_notices) {
    draw_panel_notices(d, a, limpar_area);
  } else if (s_app && s_app == s_app_recusado) {
    draw_recusa(d, a);
  } else if (s_app && s_app == s_app_loading) {
    // MONTANDO O SD AINDA. s_app ja aponta para o app (ver abrir_app()), mas
    // ao_abrir() dele NAO foi chamado - o app pode nao ter estado nenhum para
    // draw, e chamar s_app->draw() aqui seria confiar em memoria que o
    // proprio app ainda nao teve chance of prepare. Uma screen of espera simples
    // ate concluir_abertura_do_sd() liberar o app of verdade.
    d.setTextDatum(middle_center);
    tab5_text(d, 2);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    char msg[48];
    snprintf(msg, sizeof(msg), "abrindo %s...", s_app->name);
    d.drawString(msg, a.x + a.width / 2, a.y + a.height / 2);
  } else if (s_app) {
    const int max = limit_of_app(a);
    if (s_app->draw && max > 0) {
      // RECORTE PRIMEIRO. Com o content deslocado, o app desenharia por top
      // of barra of status e of navigation sem perceber - ele nao sabe que
      // existe rolagem. O recorte e o que torna essa ignorancia segura.
      //
      // recorte_app, when vem preenchido, ESTREITA esse recorte para so a
      // faixa recem-exposta (ver o aviso grande no topo of funcao) - o app
      // continua recebendo a MESMA 'rolada' of sempre, sem saber of
      // diferenca.
      const area_t& clip = recorte_app ? *recorte_app : a;
      d.setClipRect(clip.x, clip.y, clip.width, clip.height);
      area_t rolada = a;
      rolada.y     -= s_rol.current;
      rolada.height = s_app->height_content;
      s_app->draw(d, rolada);
      d.clearClipRect();
    } else if (s_app->draw) {
      s_app->draw(d, a);
    }
  } else if (limpar_area) {
    // ICONE NAO MUDA. A home e estatica: oito icones e oito rotulos, sempre os
    // mesmos. O que se mexe nela - battery, temperatura, a line of servicos -
    // e drawn por fora, com caminho proprio e comparacao antes of paint.
    //
    // Redesenhar everything numa pass of DADOS era repintar 578KB para chegar on
    // mesmo drawing. Numa transicao isso e a diferenca entre caber e nao caber
    // in banda que sobra of panel; e a pass of data acontece in pagina
    // VISIVEL, sem a protecao of troca of pagina.
    //
    // A condicao e exatamente a certa: so ha o que redesenhar aqui when a
    // area foi APAGADA - e ai o drawing realmente sumiu.
    draw_home(d, a);
  }
  d.endWrite();

  s_area_limpa = false;
  // SO LIMPA s_dirty_layout QUANDO FOI ESTA PASSADA QUE O ATENDEU
  // (limpar_area==true, ou seja: o loop principal chamou aqui exatamente
  // porque s_dirty_layout estava true). Esta line zerava
  // INCONDICIONALMENTE - e s_app->draw() (chamada acima, dentro of
  // pass INCREMENTAL tambem, limpar_area==false) pode legitimamente
  // request one repaint complete NO MEIO of propria pass of data (ex.:
  // app Camera chamando sistema_travar_navegacao(false) of dentro of
  // draw() dela, on notar que uma captura terminou). Zerar aqui sem
  // condicao apagava esse request antes que o loop principal chegasse a ve-lo
  // - a barra of navigation nunca era redesenhada destravada, e o INICIO
  // ficava escuro para sempre (defeito observado, "captura concluida" aparecia
  // no log e o botao continuava escuro of mesmo jeito).
  //
  // NAO E SO DA CAMERA: qualquer app que chame sistema_pedir_redesenho_completo()/
  // sistema_travar_navegacao()/sistema_zerar_rolagem() of dentro of proprio
  // draw() cairia in mesma armadilha - o conserto e aqui, nao em cada app.
  if (limpar_area) { s_dirty_layout = false; }
  s_dirty_data  = false;
  s_ultimo_drawing = tab5_now_ms();

  // A MARCA MORRE COM A PASSADA. Ela afirma "este rectangle esta desta color
  // AGORA"; depois que o app desenhou por top, deixa of ser verdade. Deixa-la
  // alive ate a next pass faria a limpeza seguinte ser dispensada sobre
  // content real - e o app ficaria com rastro por uma otimizacao of sistema.
  tab5_screen_forget_clean();
}

// Um frame of screen of bloqueio durante a rampa of brilho of despertar.
static void animar_in_rampa(void)
{
  if (!s_locked) { return; }
  draw_content(false, false);
  comp_apresentar();
}

// ---------------------------------------------------------------------------
// Ciclo of vida dos apps
// ---------------------------------------------------------------------------
// Declarada aqui e definida mais abaixo (ver o bloco "MONTAGEM (E DESMONTAGEM)
// DE SD EM TAREFA PROPRIA"): fechar_app() precisa dela antes of abrir_app()
// ser lido, e a definicao mora perto of irma vTaskMontarSDApp por clareza.
static void vTaskDesmontarSDApp(void*);

// NAO 'static' - declarada em kernel/internal.hpp, chamada por
// kernel/shell.cpp (tratar_toque, on close o app pelo botao INICIO).
void close_app(void)
{
  if (!s_app) { return; }
  ESP_LOGI(TAG, "fechando '%s'", s_app->name);

  // RECUSADO: o app nunca abriu (nem ao_abrir, nem profile, nem cartao) - so
  // a screen of reason of sistema exit.
  if (s_app == s_app_recusado) {
    s_app_recusado = nullptr;
    s_app = nullptr;
    scroll_reset(&s_rol);
    s_dirty_layout = true;
    return;
  }

  const bool return_sd = s_app_montou_sd;

  // O TRABALHO DO APP TERMINA ANTES DO ao_fechar(): a queue e descartada e o
  // que estiver executando e esperado - so entao o app pode liberar os
  // buffers em que o worker escreve (kernel/work.hpp).
  // FECHAR E TELA PARADA: everything daqui ate o end roda in tarefa of interface.
  // Passou of 150 ms, o log diz quem segurou - o work ou o ao_fechar().
  const int64_t t0 = esp_timer_get_time();
  work_stop_of_app();
  const int64_t t1 = esp_timer_get_time();
  if (s_app->on_close) { s_app->on_close(); }
  virtual_screen_release_surface();   // a camera ja parou no ao_fechar()
  const int64_t t2 = esp_timer_get_time();
  if (t2 - t0 > 150000) {
    ESP_LOGW(TAG, "fechar '%s' segurou a interface %lld ms (trabalho %lld, ao_fechar %lld)",
             s_app->name, (t2 - t0) / 1000, (t1 - t0) / 1000, (t2 - t1) / 1000);
  }
  s_nav_travada     = false;  // rede of seguranca: app esquecido nao prende ninguem
  system_manter_acordado(SISTEMA_ACORDADO_APP, false);   // idem: o request era of app
  s_rolagem_travada = false;  // idem, para o next app nao nascer sem rolar
  scroll_reset(&s_rol);
  s_app = nullptr;

  // A home e sempre em pe. Restaurar aqui, e nao no next open, para que a
  // screen inicial nunca apareca deitada por one frame. Vale tambem para o app
  // que seguia o IMU e abriu em pe mas fechou deitado.
  orientation_on_close();

  // Desmonta DEPOIS of ao_fechar(): o app pode querer close arquivos ali.
  // So devolve se ESTE app tiver of fato tomado.
  //
  // EM TAREFA DE FUNDO, fire-and-forget: nada daqui em diante espera a
  // desmontagem terminar, e o mutex + contagem of referencia dentro of
  // tab5_sd.cpp (ver trava()/destrava()/s_refs) ja protegem contra a corrida
  // obvia - open outro app com SD antes desta tarefa rodar so incrementa a
  // referencia sobre o cartao que ainda esta montado, em vez of montar of novo.
  if (return_sd) {
    pool_create(nullptr, vTaskDesmontarSDApp, nullptr, "sd_desmonta", 4096, 1);
    s_app_montou_sd = false;
  }
  registro_app_current(nullptr);

  tab5_power_apply(profile_of_home());
  s_dirty_layout = true;
}

// ---------------------------------------------------------------------------
// MONTAGEM (E DESMONTAGEM) DE SD EM TAREFA PROPRIA
// ---------------------------------------------------------------------------
// Tab5_sd_montar()/desmontar() rodavam DIRETO in tarefa of
// interface, dentro of abrir_app()/fechar_app() - a MESMA tarefa que chama
// M5.update() e processa touch. montar_de_verdade() (tab5_sd.cpp) liga o LDO
// of IO of cartao, negocia o host SDMMC e monta o FAT: nada catastrofico como
// os ~10s of Grove, mas a mesma classe of erro - hardware of verdade demorando
// dezenas a centenas of ms dentro of tarefa que precisa continuar lendo o
// finger. Um cartao lento para responder bastava para o mesmo sintoma of Grove
// aparecer em escala menor.
//
// A DESMONTAGEM e simples: fecha o app, some of screen, ninguem mais espera por
// ela. Vai para uma tarefa released (fire-and-forget) sem coordenacao none - o
// mutex e a contagem of referencia dentro of tab5_sd.cpp (ver trava()/
// destrava()/s_refs) ja garantem que montar() e desmontar() concorrentes of
// tarefas diferentes nao se atropelam, entao nao ha corrida a resolver aqui.
//
// A MONTAGEM e mais delicada: ao_abrir() of apps como Arquivos e Mesh presume
// que o cartao ja esta pronto (listam pasta, leem historico) in primeira
// chamada. Nao of para simplesmente disparar e follow chamando ao_abrir()
// como se nada tivesse mudado - a abertura of app precisa wait o resultado
// of montagem antes of chamar ao_abrir(). Por isso abrir_app() trava a screen no
// app (s_app = app, screen mostra "abrindo...") mas SO libera o app of verdade -
// chamando ao_abrir() - when a tarefa of fundo termina. Ver
// concluir_abertura_do_sd() e a checagem no loop of interface.
static void vTaskMontarSDApp(void*)
{
  s_sd_montou_ok = tab5_sd_mount();
  s_sd_montando  = false;   // POR ULTIMO - so now o loop pode concluir a abertura
}

static void vTaskDesmontarSDApp(void*)
{
  tab5_sd_unmount();
}

// ---------------------------------------------------------------------------
// APP RECUSADO PELO SISTEMA
// ---------------------------------------------------------------------------
// Um app que EXIGE o cartao (app_t.exige_sd: Arquivos, Camera) nao abre com
// ele emprestado on computador (modo pendrive) - e a regra e of sistema, nao
// of cada app. No lugar of app aparece uma screen of sistema dizendo por que;
// INICIO iteration normalmente. Se o cartao voltar com ela in screen, o app abre
// sozinho (loop of interface).
//
// s_app aponta para o app (a barra mostra o name, INICIO funciona), mas
// none funcao dele e chamada - app_vivo() e falso.
static void recusar_app(const app_t* app)
{
  ESP_LOGW(TAG, "'%s' nao abre: o cartao esta emprestado ao computador", app->name);
  s_app = app;
  s_app_recusado = app;
  scroll_reset(&s_rol);
  orientation_on_close();                 // a screen of reason e em pe
  tab5_power_apply(profile_of_home());   // nada of profile of app stays enabled
  s_dirty_layout = true;
}

// Segunda metade of abrir_app() when o app usa SD: chamada pelo loop of
// interface assim que a montagem of fundo termina (ok ou nao). Espelha
// exatamente o que abrir_app() fazia embutido, so que depois of espera.
static void concluir_abertura_of_sd(void)
{
  const app_t* app = s_app_loading;
  s_app_loading = nullptr;

  s_app_montou_sd = s_sd_montou_ok;
  // CORRIDA COM O PENDRIVE: o cartao foi emprestado entre a checagem of
  // abrir_app() e a montagem. Mesma regra: quem requires o cartao nao abre.
  if (!s_app_montou_sd && app->requires_sd && tab5_sd_lent()) {
    recusar_app(app);
    return;
  }
  if (!s_app_montou_sd) {
    ESP_LOGW(TAG, "'%s' pediu cartao SD e ele NAO montou - abrindo assim mesmo, "
                  "mas o app nao vai conseguir gravar", app->name);
  }

  if (app->on_open && !app->on_open()) {
    ESP_LOGE(TAG, "'%s' recusou abrir", app->name);
    if (s_app_montou_sd) {
      pool_create(nullptr, vTaskDesmontarSDApp, nullptr, "sd_desmonta", 4096, 1);
      s_app_montou_sd = false;
    }
    s_app = nullptr;      // iteration para a home: a abertura falhou of verdade
    tab5_power_apply(profile_of_home());
    s_dirty_layout = true;
    return;
  }

  registro_app_current(app->name);
  s_dirty_layout = true;   // troca a screen of "abrindo..." pelo content of verdade
}

// NAO 'static' - declarada em kernel/internal.hpp, chamada por
// kernel/shell.cpp (tratar_toque, on touch one icon of home).
bool open_app(const app_t* app)
{
  if (s_app) { close_app(); }
  ESP_LOGI(TAG, "abrindo '%s'", app->name);
  // A home exit of screen: o papel of parede pronto dela (PSRAM) vai junto.
  bq_parede_release();

  // O CARTAO ESTA NO COMPUTADOR e este app nao funciona sem ele: o sistema
  // nao abre (ver recusar_app). Antes of qualquer outra coisa - nem profile,
  // nem orientation, nem montagem.
  if (app->requires_sd && tab5_sd_lent()) {
    recusar_app(app);
    return true;
  }

  // A order importa: liga o que o app precisa ANTES of ele start.
  //
  // E o BRILHO NAO VEM DO APP. O profile of app diz que hardware ele precisa;
  // quao clara a screen stays e preferencia of quem segura o aparelho. Copiamos o
  // profile e sobrescrevemos - assim o app_t continua declarando o field (o
  // tab5_power e autocontido e usa ele), mas quem manda e o ajuste.
  // O MESMO VALE PARA O RADIO, e esta line nasceu of one bug caro.
  //
  // Todo app declara radio=false (o default), entao open QUALQUER app cortava o
  // WLAN_PWR_EN e matava o ESP32-C6 com o WiFi running. O log nao
  // deixa duvida:
  //
  //   sistema: abrindo 'Ajustes'
  //   tab5pwr: trilho of C6 (WLAN_PWR_EN) = 0
  //   sdmmc_send_cmd returned 0x108        <- 1ms depois
  //   eh_sdio: Unrecoverable host sdio state  -> reinicio of placa
  //
  // A regra geral: o profile of app diz o que ELE precisa enabled, nunca o que os
  // outros podem disable. Recurso of SERVICO of sistema - brilho, radio - e
  // decidido aqui, e o app so pode SOMAR, nunca subtrair.
  // Os DOIS caminhos precisam of regra, e por isso ela mora tambem em
  // perfil_da_home(): close one app tambem reaplica profile, e one WiFi que
  // sobrevive a open o app Ajustes mas morre on close seria pior que o bug antigo -
  // falharia so as vezes.
  // Orientacao ANTES of profile e of qualquer drawing: rotate muda width e
  // height, e everything que for calculado a partir delas precisa ver o valor novo.
  // Quais rotacoes existem e por que: kernel/orientation.cpp.
  orientation_on_open(app);

  tab5_profile_t profile = app->profile;
  system_apply_preferences(profile);
  tab5_power_apply(profile);

  // Cartao montado antes of ao_abrir(), pelo mesmo reason of sempre: o app
  // pode querer read configuracao ou open arquivo of log logo in abertura.
  //
  // A MONTAGEM EM SI NAO RODA MAIS AQUI (ver o bloco grande acima,
  // "MONTAGEM (E DESMONTAGEM) DE SD EM TAREFA PROPRIA"). Travamos a screen
  // neste app now - por isso s_app ja e atribuido nesta funcao, antes of
  // ao_abrir() ter sido chamado - e devolvemos true: a home some, a barra of
  // navigation mostra "INICIO" (a checagem of app open so olha s_app) e
  // desenhar_conteudo() mostra "abrindo..." enquanto s_app_carregando apontar
  // para este app. O touch stays engolido nesse meio-tempo (ver tratar_toque,
  // kernel/shell.cpp), por poucos frames in pratica - a mesma folga que o
  // cartao levava para montar, so que now fora of tarefa que le o finger.
  //
  // Falhar em montar NAO impede o app of open - ver concluir_abertura_do_sd().
  // Sem cartao, o Azimutal ainda desenha o mapa; ele so nao grava.
  s_app_montou_sd = false;
  if (app->uses_sd) {
    s_app_loading = app;
    s_sd_montando    = true;
    pool_create(nullptr, vTaskMontarSDApp, nullptr, "sd_monta", 4096, 1);

    s_app = app;
    scroll_reset(&s_rol);
    s_dirty_layout = true;
    return true;   // a abertura conclui em concluir_abertura_do_sd(), no loop
  }

  if (app->on_open && !app->on_open()) {
    ESP_LOGE(TAG, "'%s' recusou abrir", app->name);
    tab5_power_apply(profile_of_home());
    return false;
  }

  s_app = app;
  scroll_reset(&s_rol);   // todo app comeca no topo
  registro_app_current(app->name);
  s_dirty_layout = true;
  return true;
}

// ---------------------------------------------------------------------------
// SONDAGEM DO GROVE, EM TAREFA PROPRIA
// ---------------------------------------------------------------------------
// Parar os servicos e sondar of novo rodava DIRETO in tarefa
// of interface - a MESMA que chama M5.update() e processa touch. O preco:
// tab5_mesh_parar()/tab5_gps_stop() esperam ate 1s cada pela tarefa of
// servico confirm saida, e tab5_grove_detect() bloqueia mais uns 4-8s
// probing os dois protocolos. Toda vez que alguem pluga one modulo no
// Grove, isso e quase 10 SEGUNDOS of touch surdo - observado em
// user plugou o GPS, o touch parou of responder, e so voltou
// bem depois (o que coincidiu com o sleep ligando, mas a causa real era
// so a sondagem ainda nao ter terminado).
//
// A correcao e tirar esse work of tarefa of interface. Esta tarefa nova
// roda em paralelo - PRIORIDADE 1, a mais baixa of sistema, porque e
// work of fundo que nao pode disputar CPU nem com a interface (4) nem
// com os servicos (3). Enquanto ela para servico e sonda, a tarefa of
// interface continua livre, e o touch nunca mais stays refem disto.
//
// IDEMPOTENTE POR CONSTRUCAO: tab5_mesh_parar()/tab5_gps_stop() ja
// devolvem true sem fazer nada when o servico correspondente ja esta
// parado - por isso os DOIS pontos of loop que precisam of uma sondagem
// (a re-sondagem requested e as tentativas extras when a primeira nao achou
// nada) podem chamar a MESMA funcao, sem precisar saber se algo estava
// mesmo of pe.
// true so when o disparo e uma RE-sondagem of verdade (modulo trocado no
// cabo) - e nao uma das tentativas automaticas of achar algo num Grove que
// continua vazio. So esse caso reabre a contagem of tentativas; senao a
// tentativa automatica reiniciaria a propria contagem toda vez que
// encontrasse o cabo ainda vazio, e as "duas tentativas e para" nunca
// parariam of verdade.
static bool s_prox_sondagem_reinicia_contagem = false;

static void vTaskSondagemGrove(void*)
{
  const bool reiniciar = s_prox_sondagem_reinicia_contagem;
  const bool mesh_ok = tab5_mesh_release_grove();   // via Bluetooth o no nao esta no cabo
  const bool gps_ok  = tab5_gps_stop();
  if (!mesh_ok || !gps_ok) {
    ESP_LOGE(TAG, "nao consegui liberar o Grove - sondagem cancelada");
  } else {
    if (reiniciar) {
      s_sondas_extra = 0;          // reabre as tentativas automaticas
      s_grove_since  = tab5_now_ms(); // ... a partir of AGORA, nao of boot
    }
    if (settings_grove()) {
      const grove_type_t achado = tab5_grove_detect();
      if (achado == GROVE_MESHTASTIC) {
        // O GROVE TEM PRIORIDADE. Achar o C6L no cabo leva o Mesh para ele,
        // of qualquer via (Bluetooth ou internet). A troca acontece SO aqui,
        // no achado - boot, cabo plugado, re-sondagem -, e nao a cada iteration:
        // depois dela a escolha manual em Ajustes vale ate o next achado.
        // tab5_mesh_iniciar() para a via anterior antes of raise a serial.
        if (settings_mesh_transport() != SETTINGS_MESH_TRANSPORT_SERIAL) {
          ESP_LOGI(TAG, "Meshtastic no Grove: o cabo tem prioridade - Mesh vai para ele");
          settings_set_mesh_transport(SETTINGS_MESH_TRANSPORT_SERIAL);
        }
        tab5_mesh_start();
      }
      else if (achado == GROVE_GPS)        { tab5_gps_start(); }
    } else {
      ESP_LOGI(TAG, "Grove desligado no ajuste - parei o servico e nao sondei");
    }
  }
  // Zera a vigia: a sondagem acabou of estabelecer a verdade, e a leitura
  // seguinte of pino tem que ser comparada com ELA, nao com o que se
  // acreditava antes.
  s_grove_havia    = (tab5_grove_type() != GROVE_NOTHING);
  s_grove_iguais   = 0;
  s_dirty_layout    = true;
  s_probing_em_fundo = false;   // POR ULTIMO - so now a next pode comecar
}

// Dispara a sondagem em segundo plano, se ja nao houver uma correndo - dois
// disparos on mesmo time sondariam o mesmo cabo duas vezes por nada.
// 'reiniciar_contagem' ver o aviso em s_prox_sondagem_reinicia_contagem.
static void start_sondagem_em_fundo(bool reiniciar_contagem)
{
  if (s_probing_em_fundo) { return; }
  s_probing_em_fundo = true;
  s_prox_sondagem_reinicia_contagem = reiniciar_contagem;
  pool_create(nullptr, vTaskSondagemGrove, nullptr, "grove_sonda", 4096, 1);
}

// ---------------------------------------------------------------------------
// Tarefa of interface
// ---------------------------------------------------------------------------
static void vTaskInterface(void *pvParameters)
{
  (void)pvParameters;
  s_ultimo_touch = tab5_now_ms();

  // NO WATCHDOG DE VERDADE, e nao so o of tarefa ociosa.
  //
  // O watchdog of sistema (ver sdkconfig, ESP_TASK_WDT) por default so
  // acompanha as duas tarefas IDLE - ele pega uma tarefa que trava o CORE
  // inteiro (nunca devolve CPU, nem para o ocioso rodar), mas NAO pega one
  // DEADLOCK of verdade: esta tarefa bloqueada para sempre num mutex ou
  // semaphore que ninguem mais vai return, enquanto o resto of sistema
  // continua running normalmente por fora. Sem se inscrever, one travamento
  // assim deixaria a screen congelada para sempre, sem NENHUM sinal disso no
  // log - o aparelho pareceria "vivo" (CPU ociosa disponivel) e estaria
  // surdo e mudo por dentro.
  //
  // O maior periodo que este loop espera e ~1 s (o prazo adaptativo of
  // sleep e of screen parada, ver o end of loop) - bem dentro dos 5s of prazo
  // of watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S). Se one dia o loop nao voltar aqui dentro desse prazo, o
  // watchdog reinicia o aparelho - pior que congelado, melhor que travado
  // para sempre e invisivel.
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    ESP_LOGW(TAG, "nao consegui inscrever a interface no watchdog");
  }

  // O WiFi so sobe aqui, e nao no app_main, por uma razao concreta: enable o
  // trilho of C6 e uma escrita no expansor I2C, e esta tarefa e a dona of I2C.
  // Alem disso, subindo depois of interface, a screen ja esta desenhada when o
  // radio comeca a negociar - o user ve "conectando" em vez of wait.
  if (settings_wifi()) { tab5_wifi_turn_on(); }

  // BLUETOOTH. Mesma razao of WiFi para morar aqui: enable pede o
  // trilho of C6 (I2C). So o botao of user liga (ver sistema_bluetooth);
  // com o Mesh no Bluetooth o servico dele sobe junto - ele nao depende of
  // Grove, entao nao espera sondagem none.
  if (settings_bluetooth()) { bt_aplicar(true); }

  // Teclado of ExtPort1. Aqui, e nao no app_main, porque e I2C - e esta tarefa
  // e a dona of barramento. Sem keyboard encaixado ele so avisa e segue.
  //
  // A VONTADE DO USUARIO E EMPURRADA, nunca lida por dentro of HAL - por isso
  // o habilitar() vem ANTES of start(). Ver o aviso em tab5_keyboard.cpp.
  tab5_keyboard_enable(settings_keyboard());
  tab5_keyboard_init();

  // Sensor of temperatura of P4. Nao e I2C (e one ADC interno), mas nasce aqui
  // junto dos outros para o boot ter one lugar so onde os sensores sobem.
  tab5_sensors_init();

  // PRIORIDADE DA TELA NO BARRAMENTO. Antes of qualquer app draw: e a
  // arbitragem que decide se a piscada azul pode ou nao acontecer, e ela nao
  // depende of nada que os apps facam.
  tab5_bus_prioritize_screen(true);

  // A TELA VIRTUAL (COMPOSITOR.md, etapa 3a). Antes of first drawing: a
  // partir daqui, home, barras e apps desenham nela, e so o compositor
  // escreve nas paginas of panel.
  virtual_screen_start();

  // O WORKER DO SISTEMA (kernel/work.hpp): o que for lento exit
  // desta tarefa.
  work_start();

  // RELOGIO DO PAINEL. A outra metade of mesma briga: a priority decide quem
  // ganha a disputa, isto REDUZ a disputa. O M5GFX programa 80MHz para este
  // panel - 64 frames por segundo e 118,5 MB/s lidos of PSRAM sem stop. A
  // 60MHz sao 48 frames e 88,9 MB/s: one quarto a menos of demanda permanente,
  // e nenhum app precisa saber que isso existe.
  //
  // Aqui, antes of first drawing: a troca desmonta e remonta o panel, e
  // fazer isso com a screen ja em uso apagaria o content por one instante.
  if (tab5_panel_touch_gt911()) { tab5_power_dpi_clock((int)settings_panel_clock_mhz()); }   // TDDI: stays o of fabrica

#ifdef TAB5_MOSTRAR_SPLASH
  // SO AGORA: o panel ja tem o clock final e nao vai mais ser desmontado
  // neste boot - ver o aviso grande junto of desenhar_splash() (shell.cpp).
  draw_splash();
#endif

  // Quem esta no Grove? So vale sondar se o trilho estiver enabled - sem energia
  // ninguem se apresenta. E so o Meshtastic vira SERVICO permanente: o GPS
  // continua sendo open e closed pelo app Azimutal, porque ele nao precisa
  // existir com o app closed (ninguem espera notification of satelite).
  // SONDAGEM ADIADA PARA O LOOP, e nao feita aqui.
  //
  // Ela bloqueia ~3 segundos (liga o trilho, espera o modulo bootar, escuta os
  // dois baud). Feita AQUI, antes of first drawing, o aparelho ficava com a
  // screen apagada durante toda a sondagem - parecia travado no boot.
  //
  // Marcando o request, o loop entra, desenha a home e SO ENTAO sonda. A pessoa
  // ve o sistema of pe em alguns frames; o Grove chega alguns segundos depois,
  // que e when ele tem of chegar - nenhum app depende dele para open.
  //
  // Reaproveita exatamente o caminho of troca of modulo: one lugar so que sabe
  // stop servico, sondar e raise o que couber.
  if (settings_grove()) {
    s_grove_resondar = true;
  } else {
    ESP_LOGI(TAG, "Grove sem energia (ajuste desligado) - nao vou sondar");
  }

  for (;;) {
    // MEDIDA (kernel/compositor.hpp, comp_estat): a iteration comeca aqui, no
    // wake. Se ela terminar em frame, o custo inteiro vai on histograma.
    const int64_t iteration_t0 = esp_timer_get_time();
    ETAPA("toque (M5.update, I2C)");
    // Navegacao travada = keyboard (ou captura) lendo o touch por level.
    ajustar_leitura_of_touch(s_nav_travada && (tab5_now_ms() - s_ultimo_touch) < 500);
    M5.update();
    const uint32_t t = tab5_now_ms();

    atender_request_of_clock();

    // PAINEL NOVO (boot, troca of clock): no modo direto as paginas sao a
    // UNICA copia of screen - panel novo nasce sem nada nosso, e o unico jeito
    // of ter a screen of iteration e redesenha-la inteira. (O wake of sleep
    // trata o seu caso la embaixo, antes of acender a luz.)
    if (!tab5_power_is_sleeping() && comp_painel_novo()) { s_dirty_layout = true; }

    // As partes of fundo (kernel/background.h): quem terminou sozinho e limpo, a
    // parada requested pelo console acontece, e o profile efetivo e reaplicado
    // when o conjunto changed. Acordado ou em sleep - o fundo nao dorme.
    ETAPA("partes de fundo");
    background_manter();
    ETAPA("vigia do Grove / abertura com SD");

    // ABERTURA DE APP COM SD PENDENTE: a tarefa of fundo (vTaskMontarSDApp)
    // terminou since o ultimo ciclo? Conclui now, com a screen ja mostrando
    // "abrindo..." ha alguns frames - ver abrir_app() e concluir_abertura_do_sd().
    if (s_app_loading && !s_sd_montando) {
      concluir_abertura_of_sd();
    }
    // O CARTAO VOLTOU com a screen of recusa open: abre o app of verdade.
    if (s_app_recusado && !tab5_sd_lent() && !s_locked) {
      const app_t* ap = s_app_recusado;
      close_app();
      open_app(ap);
    }

    // ---------------------------------------------------------------------
    // GROVE WATCHER: notice a module swap without paying for polling in a loop.
    // ---------------------------------------------------------------------
    //
    // Probing costs 3s of blocked task. Running that periodically is out of
    // the question, and having no alternative is why detection became a BOOT
    // decision - done once and never revisited. Unplugging the GPS and
    // plugging in the C6L changed nothing because nobody was watching.
    //
    // The fix is splitting the two questions. "Who's there?" is expensive
    // and only gets asked when it's worth it. "Is anyone there?" is a GPIO
    // read (see tab5_grove_present) and can run at 0.5Hz with no cost.
    //
    // With the port BUSY you can't just read the pin - it belongs to the
    // UART driver. There, the informant is someone else: the service itself,
    // which knows how long it's been since the radio last spoke. Every state
    // has something watching it.
    if (settings_grove() && !s_grove_resondar && (t - s_grove_olhado) >= VIGIA_MS) {
      s_grove_olhado = t;
      const grove_owner_t dono = tab5_grove_owner();

      if (dono == GROVE_OWNER_MESH) {
        // O radio calou. 60s e folga larga of proposito: o servico pede
        // apresentacao a cada 10s enquanto esta mudo, entao one C6L vivo
        // responde muito antes. E one alarme falso se conserta sozinho - a
        // sondagem seguinte o encontra of novo - enquanto one alarme cedo
        // demais derrubaria o servico of one radio so ocupado.
        // PRAZO PROPRIO, e nao o of no_ar().
        //
        // Apoiada no no_ar(), esta vigia herdava o SILENCIO_MS of servico - que
        // e generoso of proposito para nao chamar of morta uma malha quieta.
        // Quando ele subiu of 30s para 90s, os dois prazos se SOMARAM e a troca
        // of modulo no cabo passou a levar 150s para ser notada. Perguntas
        // diferentes, prazos diferentes: aqui o que importa e quanto time faz
        // que o radio nao fala, cru.
        if (tab5_mesh_silencio_ms() > MUDO_MS) {
          ESP_LOGW(TAG, "radio mudo ha %us - devolvendo o Grove e sondando",
                   (unsigned)(MUDO_MS / 1000));
          s_grove_resondar = true;   // o bloco abaixo para o servico e sonda
        }
      }
      else if (dono == GROVE_OWNER_GPS) {
        // MESMA IDEIA of bloco of Mesh acima, prazo proprio (ver GPS_MUDO_MS):
        // o GPS fala sozinho e continuo, entao silencio aqui significa cabo
        // puxado com muito mais confianca of que significaria para o radio.
        if (tab5_gps_silence_ms() > GPS_MUDO_MS) {
          ESP_LOGW(TAG, "GPS mudo ha %us - devolvendo o Grove e sondando",
                   (unsigned)(GPS_MUDO_MS / 1000));
          s_grove_resondar = true;
        }
      }
      else if (dono == GROVE_OWNER_NONE) {
        const bool ha = tab5_grove_present();

        // Duas leituras iguais antes of acreditar. Um cabo sendo encaixado
        // toca antes of assentar, e agir no first contato dispararia
        // sondagem no middle of movimento of mao.
        if (ha == s_grove_havia) { if (s_grove_iguais < 3) { ++s_grove_iguais; } }
        else                     { s_grove_havia = ha; s_grove_iguais = 1; }

        if (s_grove_iguais == 2) {
          const bool tinha = (tab5_grove_type() != GROVE_NOTHING);
          if (ha && !tinha) {
            ESP_LOGI(TAG, "algo foi plugado no Grove - sondando");
            s_grove_resondar = true;
          } else if (!ha && tinha) {
            // Nao se paga sondagem para concluir "nada": o pino ja disse. E
            // sem isto o tipo velho ficaria no ar, que e o bug of origem.
            ESP_LOGI(TAG, "o Grove esvaziou");
            // So esquecer o tipo. Este ramo so roda com o dono = NINGUEM:
            // nem o Mesh (serial) nem o GPS estao com a UART, entao nao ha
            // servico a stop - e stop servico (ate 1 s esperando a tarefa
            // dele) e justamente o que a regra of core tira of interface (ver
            // vTaskSondagemGrove). Chamar aqui era inofensivo so por acaso.
            tab5_grove_forget();
            s_dirty_layout = true;
          }
        }
      }
    }

    // RE-SONDAGEM DO GROVE, requested por quem trocou o modulo of cabo.
    //
    // Feita EM TAREFA PROPRIA (ver vTaskSondagemGrove, logo
    // acima) - nao mais direto aqui. Chamar tab5_mesh_parar()/
    // tab5_gps_stop()/tab5_grove_detect() nesta tarefa (a mesma que
    // processa touch) prendia o touch por ate ~10s toda vez que alguem
    // trocava o modulo of cabo - observado, user achou que o
    // touch tinha travado of vez.
    //
    // UM CICLO DE ESPERA antes of disparar, ainda assim: o ciclo anterior ja
    // desenhou o botao no estado novo e o app Ajustes ja pode write
    // "probing..." antes of tarefa of fundo comecar a trabalhar of verdade.
    if (s_grove_resondar && s_grove_request_visto) {
      s_grove_resondar = false;
      s_grove_request_visto = false;
      start_sondagem_em_fundo(true);   // troca of modulo - reabre as tentativas
    }


    // NOVA SONDAGEM DO GROVE, when a primeira nao achou nada.
    //
    // A sondagem of boot acontece uma vez e decide a sessao inteira. Isso a
    // torna refem of INSTANTE: o C6L e one ESP32-C6 running o Meshtastic
    // complete, e ele leva alguns segundos para responder. Boot of Tab5 mais
    // rapido que o boot dele - o que acontece depois of tirar o cabo, por
    // exemplo - e o servico simplesmente nao subia mais ate alguem reiniciar.
    //
    // Duas tentativas extras, aos 20s e aos 50s, e so enquanto NADA DE
    // CONCLUSIVO foi encontrado - tambem em tarefa of fundo (ver
    // vTaskSondagemGrove), pelo mesmo reason of bloco acima: nao prender o
    // touch.
    //
    // GROVE_UNKNOWN ENTRA AQUI TAMBEM, e a condicao so tinha GROVE_NOTHING
    // achado on vivo, com log: o boot ligou o 5V, esperou os
    // 4s of C6L raise, a sonda a 115200 nao viu nada e a of 9600 viu 2 bytes
    // of ruido (nem NMEA nem START1 of Meshtastic) - "desconhecido", nao
    // "nada". Com a condicao antiga isso NUNCA tentava of novo sozinho: o
    // servico so subia depois of arrancar e reencaixar o cabo in mao. Um C6L
    // que ainda esta terminando of bootar (ou one instante ruim of energia,
    // ver o BOD que apareceu no mesmo log) pode muito bem return ruido em
    // vez of silencio OU of uma resposta valida - e merece a mesma segunda
    // chance que "nada" ja tinha.
    //
    // Nunca sonda com a porta ocupada: o dono (Mesh ou GPS, os dois servicos
    // que hoje disputam o Grove) esta usando a UART, e uma sondagem por top
    // dela quebraria a leitura.
    if (s_sondas_extra < 2 && settings_grove()
     && (tab5_grove_type() == GROVE_NOTHING || tab5_grove_type() == GROVE_UNKNOWN)
     && tab5_grove_owner() == GROVE_OWNER_NONE
     && t > s_grove_since + (uint32_t)(20000 + s_sondas_extra * 30000)) {
      ++s_sondas_extra;
      ESP_LOGI(TAG, "Grove inconclusivo na primeira sonda (%s) - tentando de novo (%d/2)",
               tab5_grove_type() == GROVE_NOTHING ? "nada" : "desconhecido",
               s_sondas_extra);
      start_sondagem_em_fundo(false);   // tentativa automatica - NAO reabre a contagem
    }

    // Uma leitura of INA226 por segundo, ACORDADO OU EM REPOUSO. Esta tarefa e
    // a unica que pode ir on I2C, entao e ela quem alimenta o cache que o
    // registro of energia consome. Sem isto, o registro so teria dado enquanto
    // a barra of status estivesse sendo desenhada - ou seja, nada em sleep.
    if ((t - s_ultima_measurement) >= 1000) {
      s_ultima_measurement = t;
      ETAPA("medidas (I2C: energia, sensores, IMU, hora)");
      tab5_power_measure();
      // Mesmo tique: os dois alimentam a mesma barra, e uma cadencia so evita
      // que ela mude por metades em instantes diferentes.
      tab5_sensors_measure();
      // IMU, 1x/s - so para o CACHE que outros apps (nao o app_imu, que le
      // direto e mais rapido - ver o aviso grande em tab5_imu.hpp sobre por
      // que as cadencias sao diferentes of proposito) podem consultar sem
      // touch no barramento I2C interno.
      // EM REPOUSO, NAO: ninguem le este cache com a screen apagada (quem
      // segue o IMU - kernel/orientation - mede sozinho a cada 200 ms, e so com
      // app open). Era uma transacao I2C por segundo, todo segundo, para
      // nada; acordado, ela continua servindo o console.
      if (!tab5_power_is_sleeping()) { tab5_imu_measure(); }
      // Hora: GPS/Mesh como fonte, RTC (I2C interno) como destino - mesma
      // regra of so a interface touch o barramento. So escreve of fato a
      // cada ~10min (ver RESSINCRONIZAR_MS), a chamada 1x/s e so para
      // perceber uma fonte nova assim que ela aparecer.
      tab5_time_sync();
      vigiar_mesh_internet(t);
    }

    // Teclado fisico: a cada TECLADO_MS. Quando nao ha key e UMA transacao
    // I2C (so o contador of eventos), barato o bastante para o laco acordado.
    //
    // A TRADUCAO E DO DRIVER (hal/tab5_keyboard): ele conhece o keyboard - o
    // modo HID, a tabela das 70 teclas, as teclas mortas dos acentos - e
    // entrega o evento pronto. Aqui so se entrega on app. Ate 4 teclas por
    // iteration: digitando rapido, mais of uma cabe no intervalo, e uma por iteration
    // atrasaria o text atras dos fingers.
    if (!tab5_power_is_sleeping() && (t - s_ultimo_keyboard) >= keyboard_ms()) {
      s_ultimo_keyboard = t;
      ETAPA("teclado (I2C)");
      const bool keyboard_antes = tab5_keyboard_present();
      tab5_key_t ev;
      for (int k = 0; k < 4 && tab5_keyboard_read(&ev); ++k) {
        s_ultimo_touch = t;          // digitar conta como uso: nao apague a screen
        if (s_locked) {
          // TELA BLOQUEADA: o keyboard nao fala com o app que esta atras - so
          // o Enter, que desbloqueia (o par of deslize para top). Antes desta
          // guarda o que se digitava no bloqueio ia stop in conversa.
          if (ev.code == TAB5_KEY_ENTER) {
            ESP_LOGI(TAG, "desbloqueada pelo teclado (Enter)");
            desbloquear_screen();
            tab5_keyboard_discard();   // o resto of rajada nao vai para o app
            break;
          }
          continue;
        }
        if (s_panel_notices) {
          // CENTRAL DE AVISOS ABERTA: o app esta escondido e nao ouve. Esc
          // fecha a central; o resto e engolido (nao vai digitar as cegas).
          if (ev.code == TAB5_KEY_ESC) { panel_notices_open(false); }
          continue;
        }
        if (app_vivo() && s_app->on_type) {
          s_app->on_type(ev.text, ev.code, ev.mod);
          system_request_redraw();
        } else {
          ESP_LOGI(TAG, "tecla: \"%s\" cod=%u%s%s%s (nenhum app escutando)",
                   ev.text, (unsigned)ev.code,
                   (ev.mod & TAB5_KEY_CTRL)  ? " +CTRL"  : "",
                   (ev.mod & TAB5_KEY_SHIFT) ? " +SHIFT" : "",
                   (ev.mod & TAB5_KEY_ALT)   ? " +ALT"   : "");
        }
      }
      // ENCAIXAR/TIRAR MUDA O LAYOUT mesmo sem key: o keyboard virtual
      // open precisa exit when aparece o fisico, e a dica of barra of
      // escrita muda nos dois sentidos. Se a screen ja estava deitada nao ha
      // giro para provocar esta pass por acaso.
      if (keyboard_antes != tab5_keyboard_present()) { s_dirty_layout = true; }
    }

    // Em sleep, detecta o touch por NIVEL (ha finger in screen?) e nao por borda.
    // O wasClicked() dispara in SOLTURA, e o loop em sleep roda a cada 200ms:
    // one touch rapido comeca e termina dentro of mesmo intervalo, o M5.update()
    // nunca ve a transicao, e o touch se perde. Era por isso que as vezes
    // precisava touch duas ou tres vezes para wake.
    ETAPA("toque: decidir");
    const bool tocando = (M5.Touch.getCount() > 0);
    if (tocando) { s_finger_ms = t; }
    const auto touch = M5.Touch.getDetail();
    if (!tab5_power_is_sleeping()) { ETAPA("toque: vigia"); vigiar_touch(t, tocando); lupa_touch(t, tocando); validar_isr_tddi(tocando); }

    // TECLA EM REPOUSO: a fila of keyboard e consultada a cada iteration of sleep
    // (o pulso of G50 acorda a iteration in hora; sem ele, o prazo of ~1 s). Uma
    // transacao I2C por iteration, e so com keyboard encaixado.
    bool key_acorda = false;
    if (tab5_power_is_sleeping() && tab5_keyboard_present()) {
      key_acorda = tab5_keyboard_pending();
    }

    // O request of wake so vale para o sleep em que chegou: acordado, ele
    // e descartado - sobrando, acordaria o PROXIMO sleep logo on entrar.
    const bool request_wake = s_request_wake;
    s_request_wake = false;
    if (tab5_power_is_sleeping()) {
      const bool sim_acorda = s_sim_x >= 0 || s_sim_open || s_sim_close || request_wake;
      if (tocando || key_acorda || sim_acorda) {
        s_sim_x = -1;   // o request que acordou nao vira click, como o finger
        s_ultimo_touch = t;
        // O que acordou nao conta como uso: o finger nao vira click
        // (s_dedo_do_despertar), e a key nao vira letra (descartada).
        if (tocando) { s_finger_of_despertar = true; }
        if (key_acorda) { tab5_keyboard_discard(); }
        ESP_LOGI(TAG, "acordando (%s): app=%s", tocando ? "toque" : "teclado", s_app ? s_app->name : "home");
        // DESLIGA A ISR AQUI, antes of mais nada - ver o aviso grande junto
        // of s_toque_isr_ok. Acordado, one arraste pulsaria este pino a cada
        // 5-20ms e a queue nunca ficaria vazia se a interrupcao seguisse
        // enabled.
        s_isr_acorda = false;
        ETAPA("repouso: acordando");
        if (tab5_power_resume()) {
          // RELIGA O QUE O APP SOLTOU EM ao_repouso() (ex.: a camera) - so
          // depois of panel ja estar of pe of novo, para nao reabrir one
          // consumidor of DMA antes of canal of panel estar reconstruido.
          // Ver o aviso grande em kernel/app.h junto of ao_repouso/ao_retomar.
          if (app_vivo() && s_app->on_resume) {
            s_app->on_resume();
          }
          // O resume() devolve o panel on ar com a LUZ AINDA APAGADA e as
          // paginas pretas - e no modo direto elas eram a unica copia of screen.
          // Redesenho COMPLETO direto in pagina of tras, e so depois of troca
          // CONFIRMADA a luz sobe: a primeira coisa que o olho ve ja e o
          // content certo. comp_painel_novo() e consumido aqui para o topo
          // of loop nao request one segundo redraw complete.
          const uint32_t t0 = tab5_now_ms();
          lockscreen_on_enable_screen();
          comp_painel_novo();
          draw_barra_status(virtual_screen(), true);
          draw_content(true, true);
          comp_apresentar();
          comp_esperar_troca();
          ESP_LOGI(TAG, "acordando: redesenho levou %lu ms",
                   (unsigned long)(tab5_now_ms() - t0));

          // A rampa of brilho bloqueia esta tarefa; a animacao of screen of
          // bloqueio avanca one frame a cada degrau para nao ficar congelada.
          if (s_locked) { tab5_power_ramp_hook(animar_in_rampa); }
          tab5_power_wake_screen();
          tab5_power_ramp_hook(nullptr);

          // Marcado AQUI, depois of everything pronto. A carencia of time virou
          // rede secundaria (a bandeira acima e a principal), e now ela conta
          // of momento em que a screen became utilizavel - nao of when o
          // despertar comecou.
          s_acordou_em = tab5_now_ms();

          // REDE DE SEGURANCA, AGORA SO QUANDO ELA FAZ FALTA.
          //
          // Ela nasceu para a screen esbranquicada: naquele drawing o panel era
          // RESSINCRONIZADO e o first frame depois disso podia exit ruim,
          // entao one segundo redraw 20ms depois consertava.
          //
          // Hoje o sleep DESMONTA o panel e o resume cria one novo. Com
          // double buffer, o que foi para a screen e one frame inteiro pintado of
          // zero e publicado of uma vez - nao ha "first frame duvidoso"
          // para corrigir. O que sobrou foi o CUSTO: one segundo redraw
          // complete (in home sao os oito icones, 578KB) disparado in pagina
          // VISIVEL exatamente enquanto a luz sobe em rampa. A rajada mais mal
          // colocada que existia, bem no instante em que o olho esta olhando.
          //
          // Sem double buffer a historia e outra: ai o drawing foi feito in
          // propria screen, aos pedacos, e uma segunda pass continua valendo.
          // (A "rede of seguranca" of segundo redraw morreu com o
          // compositor: o frame que acende e sempre inteiro.)
        } else {
          // resume() ja loga o reason. Aqui so registramos que a screen seguiu
          // apagada of proposito, para o log count a historia inteira.
          ESP_LOGE(TAG, "acordando: retomar() falhou - tela segue apagada");
        }
      }

      // Beep of notification + disable o I2S ocioso, TAMBEM em sleep -
      // achado, ver o aviso grande em
      // atualizar_beep_e_ociosidade_do_som() mais acima no arquivo. Mesmo
      // passo of 2s que a barra usa acordada (s_ultima_barra e
      // compartilhada entre os dois ramos: sao mutuamente exclusivos, nunca
      // correm no mesmo ciclo, entao nao ha disputa por ela).
      if ((t - s_ultima_barra) >= BARRA_MS) {
        s_ultima_barra = t;
        atualizar_beep_e_ociosidade_of_sound(t);
      }
    } else {
      if (tocando) { s_ultimo_touch = t; }

      // Toque por NIVEL, para quem pediu: entregue a cada ciclo (20 ms), com o
      // finger ainda in screen. E o que o wasClicked() nao of - ele so fala in
      // soltura, e one jogo nao pode wait o finger exit para virar a cobra.
      // A borda of subida stays a cargo of app; aqui so contamos o que ha now.
      //
      // A carencia of 500 ms depois of wake vale igual: o finger que acendeu a
      // screen nao pode virar jogada.
      if (app_vivo() && s_app->on_touch_level
       && !system_app_covered() && !s_finger_of_despertar && (t - s_acordou_em) > 500) {
        const area_t a = area_util();
        // MESMA CORRECAO E MESMA FAIXA DO CLIQUE (shell.cpp, tratar_toque):
        // TOQUE_DY aplicado aqui tambem, e com a navigation travada a faixa of
        // bottom e of app - o ponto nela e entregue in ultima line of area.
        const int nav_y0 = M5.Display.height() - ALTURA_NAV;
        auto y_of_app = [&](int y_cru, bool* vale) {
          int y = y_cru - TOQUE_DY;
          *vale = y >= ALTURA_BARRA && (y < nav_y0 || s_nav_travada);
          if (y >= nav_y0) { y = nav_y0 - 1; }
          return y;
        };
        bool dentro = false;
        const int ty = y_of_app(touch.y, &dentro);
        dentro = dentro && tocando;
        // SO OS DEDOS NA TELA AGORA. O getCount() of M5Unified conta o finger
        // que ja subiu por mais DUAS amostras (estados touch_end e depois
        // none), e o getDetail(i) indexa o relatorio cru of ultima leitura -
        // alem of numero of pontos dele, as entries sao velhas. Lido pelo
        // getCount, o keyboard via a key "ainda apertada" ~40 ms depois of o
        // finger exit: toda letra atrasava duas voltas, e o finger seguinte que
        // descia nessa window com o mesmo id (o GT911 reaproveita o menor
        // livre) parecia o mesmo finger escorregando - a key anterior sumia.
        // Aqui: as 5 vagas, so as pressionadas, cada id uma vez.
        s_n_touches = 0;
        for (int i = 0; i < (int)m5::Touch_Class::TOUCH_MAX_POINTS && s_n_touches < SISTEMA_TOQUES_MAX; ++i) {
          const auto& dd = M5.Touch.getDetail(i);
          if (!dd.isPressed()) { continue; }
          bool repetido = false;
          for (int k = 0; k < s_n_touches; ++k) { if (s_touches[k].id == dd.id) { repetido = true; break; } }
          if (repetido) { continue; }
          bool vale = false;
          const int y = y_of_app(dd.y, &vale);
          if (!vale) { continue; }
          s_touches[s_n_touches].id = dd.id;
          s_touches[s_n_touches].x  = (int16_t)(dd.x - a.x);
          s_touches[s_n_touches].y  = (int16_t)(y - a.y + s_rol.current);
          ++s_n_touches;
        }
        // ROLAGEM SOMADA AQUI TAMBEM. O ao_tocar() ja fazia isso e este nao
        // fazia - a assimetria nao aparecia porque nenhum app que rola usava
        // touch por level. Deixar assim seria plantar one bug para o first
        // que usasse: os dois retornos of touch TEM que falar o mesmo idioma
        // of coordenadas, senao o app acerta num e erra no outro.
        s_app->on_touch_level(dentro ? touch.x - a.x : -1,
                              dentro ? ty - a.y + s_rol.current : -1,
                              dentro);
      }

      // ARRASTE: acumula enquanto o finger esta in area util. Toda a
      // matematica of arraste/distance/sinal mora now em
      // kernel/scroll.hpp - ver o aviso grande la sobre por que isto
      // deixou of ser so deste arquivo.
      {
        const area_t ar = area_util();
        s_rol.max = limit_of_app(ar);
        const bool in_area = tocando
                          && touch.y >= ALTURA_BARRA
                          && touch.y <  (M5.Display.height() - ALTURA_NAV);
        // NAO marca s_dirty_layout aqui. Rolagem pura tem one caminho proprio,
        // mais barato (ver o bloco of repintura no loop principal): desloca
        // o pixel ja drawn em vez of erase a area inteira e redesenhar
        // everything of novo a cada sample of touch.
        scroll_drag(&s_rol, touch.y, in_area && !system_app_covered() && s_rol.max > 0);
      }

      // O CLIQUE DO SISTEMA - ver TOQUE_TOLERANCIA_PX no topo of arquivo.
      if (touch.isPressed()) {
        if (!s_finger_in_screen) {
          s_finger_in_screen = true;
          s_touch_x0 = touch.x; s_touch_y0 = touch.y;
          s_touch_desvio = 0;
          s_touch_since = t;
        }
        const int dx = abs(touch.x - s_touch_x0), dy = abs(touch.y - s_touch_y0);
        if (dx > s_touch_desvio) { s_touch_desvio = dx; }
        if (dy > s_touch_desvio) { s_touch_desvio = dy; }
        s_touch_x = touch.x; s_touch_y = touch.y;
        // SEGURAR UM ICONE DA HOME levanta ele para move (shell.cpp). Com one
        // icon levantado o touch e dele: a soltura nao vira click nem gesto.
        if (!s_app && !s_app_loading && !system_app_covered() && !s_finger_of_despertar && (t - s_acordou_em) > 500
         && home_segurar(s_touch_x, s_touch_y, t, t - s_touch_since, s_touch_desvio)) {
          s_touch_consumido = true;
        }
      } else if (s_finger_in_screen) {
        s_finger_in_screen = false;
        s_ultimo_touch = t;
        home_soltar();   // grava a order, se one icon foi movido
        // ARRASTE NAO E CLIQUE. Sem esta guarda, terminar uma rolagem apertaria
        // o que estivesse embaixo of finger - e o finger termina em top of algo,
        // sempre. rolagem_foi_arraste() e chamada em TODA soltura: ela tambem
        // zera a measurement para o next touch.
        const bool rolou   = scroll_foi_arraste(&s_rol);
        const bool arrasto = rolou || s_touch_desvio > TOQUE_TOLERANCIA_PX;
        // Duas guardas, e a primeira e a que resolve: e o mesmo finger que
        // acordou a screen? A segunda pega o touch acidental logo depois.
        const bool valido = !arrasto && !s_finger_of_despertar && (t - s_acordou_em) > 500
                         && !s_touch_consumido;
        if (diag_touch_enabled()) {
          ESP_LOGI(TAG, "toque: %lu ms, desvio %d px -> %s", (unsigned long)(t - s_touch_since),
                   s_touch_desvio, valido ? "CLIQUE" : rolou ? "rolagem" : arrasto ? "arrasto"
                                         : s_touch_consumido ? "consumido (nivel)" : "despertar/carencia");
        }
        // GESTO (arraste lateral in home, deslize para top no bloqueio). Um
        // arraste vertical of rolagem of app nao chega aqui porque o app so
        // ve gesto com a home open; o finger que acordou a screen nunca conta.
        if (arrasto && !s_finger_of_despertar && (t - s_acordou_em) > 500 && !s_touch_consumido) {
          tratar_gesto(s_touch_x - s_touch_x0, s_touch_y - s_touch_y0);
        }
        if (valido) {
          ETAPA("tratando toque (abrir/fechar app, ao_tocar)");
          tratar_touch(s_touch_x, s_touch_y);
        }
      }

      // BANCADA: o request of console, in tarefa certa.
      // Com a navigation travada, switch of app por fora tambem e recusado:
      // a trava e of app open, nao so of botao INICIO.
      // COM O APP AINDA "ABRINDO..." (montando o cartao) os pedidos ESPERAM a
      // next iteration - a mesma regra of touch (tratar_toque engole). Atender
      // now chamaria o ao_fechar() of one app cujo ao_abrir() nunca rodou, e
      // depois concluir_abertura_do_sd() abriria one app que ja exited of screen.
      if (s_sim_close && !s_app_loading) {
        s_sim_close = false; s_ultimo_touch = t;
        if (s_app && s_nav_travada) { ESP_LOGW(TAG, "fechar recusado: '%s' travou a navegacao", s_app->name); }
        else if (s_app)             { close_app(); }
      }
      if (const app_t* ap = (s_app_loading ? nullptr : s_sim_open)) {
        s_sim_open = nullptr; s_ultimo_touch = t;
        if (s_app && s_nav_travada) {
          ESP_LOGW(TAG, "abrir '%s' recusado: '%s' travou a navegacao", ap->name, s_app->name);
        } else {
          if (s_app) { close_app(); }
          open_app(ap);
        }
      }
      if (s_sim_x >= 0 && !s_finger_in_screen) {
        const int sx = s_sim_x, sy = s_sim_y;
        s_sim_x = -1;
        s_ultimo_touch = t;
        ESP_LOGI(TAG, "toque simulado em %d,%d", sx, sy);
        tratar_touch(sx, sy);
      }

      // Fim of touch: a marca of consumido nao passa para o next. Tambem
      // limpa a marcada sem touch of reconhecedor em progress (o level viu one
      // finger que o click nao acompanhava).
      if (!s_finger_in_screen) { s_touch_consumido = false; }

      // O finger of despertar so deixa of count when SAI of screen. Depois of
      // bloco of click, of proposito: in pass of soltura o reconhecedor
      // ainda precisa ver a bandeira of pe.
      if (!tocando) { s_finger_of_despertar = false; }
    }

    if (!tab5_power_is_sleeping()) {
      // A tentativa frustrada tem carencia of 5s. Sem isso, one sleep que
      // falha no middle desfaz everything, devolve false, e 20ms depois a condicao of
      // ociosidade continua verdadeira e ele tenta OUTRA VEZ - one loop of
      // erase e acender, que e o que faz as barras piscarem.
      const bool pode_tentar = (s_sleep_falhou == 0) || ((t - s_sleep_falhou) > 5000);

      // Pedido of screen acesa acabou of exit: o prazo recomeca now.
      static uint32_t acordado_antes = 0;
      const uint32_t acordado = s_acordado.load();
      if (acordado_antes && !acordado) { s_ultimo_touch = t; }
      acordado_antes = acordado;
      if ((t - s_ultimo_touch) > ocioso_ms() && pode_tentar && !acordado) {
        // Diz o time CONFIGURADO junto: e a unica prova, no log, of que o
        // ajuste of Ajustes esta mesmo chegando aqui.
        ESP_LOGI(TAG, "repouso apos %u s sem toque (ajuste de Ajustes)",
                 (unsigned)settings_screen_timeout_s());
        // TESTADO: 20MHz quebrou a UART of Mesh (o servico voltou
        // a mostrar "sem modulo" mesmo com o C6L plugado e respondendo antes).
        // De iteration a 40. A ideia seguinte - piso bottom SO when tab5_grove_owner()
        // for GROVE_OWNER_NONE, alto when Mesh/GPS estiver of pe - ainda
        // precisa of teste isolado (ver conversa) antes of entrar
        // aqui: falta saber ate onde o I2C interno (touch/INA226, que rodam
        // TODO sleep, com Grove vazio ou nao) tolera lower.
        // AVISA O APP ANTES DE DESMONTAR A TELA - ver o aviso grande em
        // kernel/app.h junto of ao_repouso/ao_retomar (o crash
        // of camera+sleep). Chamado aqui, ainda com o panel of pe, para o
        // app release hardware (DMA/etc) que nao pode conviver com o panel
        // sendo destruido e recriado por tab5_power_sleep() logo abaixo.
        ETAPA("repouso: entrando");
        // O fundo of screen of bloqueio (~1,6 MB in PSRAM) nao precisa dormir
        // junto: o despertar sorteia e pinta outro.
        if (s_locked) { bq_anim_release(); }
        bq_parede_release();   // o papel of parede pronto of home tambem (~1,6 MB)
        if (app_vivo() && s_app->on_sleep) {
          s_app->on_sleep();
        }
        int piso = (s_app && s_app->piso_sleep_mhz > 0) ? s_app->piso_sleep_mhz : 40;
        if (background_piso_sleep() > piso) { piso = background_piso_sleep(); }   // o fundo segue running
        if (!tab5_power_sleep(piso)) {
          s_sleep_falhou = t;
          s_dirty_layout = true;      // a screen voltou: redesenha o que havia
          // tab5_power_sleep() FALHOU - o panel NAO foi desmontado, entao
          // o app stays of pe com a screen acesa e o hardware que ele acabou of
          // release em ao_repouso() (lines acima) tem que voltar AGORA, senao
          // stays disabled ate o next sleep ter sucesso.
          if (app_vivo() && s_app->on_resume) {
            s_app->on_resume();
          }
        } else {
          s_sleep_falhou = 0;
          // LIGA A ISR AQUI - so now, com a screen of fato apagada, e seguro
          // deixar o touch wake a queue sem atrapalhar PERIODO_UI/OCIOSO
          // (ver o aviso grande junto of s_toque_isr_ok). reset a notification evita
          // que uma sobra of evento of ANTES of dormir cause one wake
          // espurio one ciclo depois of entrar aqui.
          if (s_touch_isr_ok) {
            ulTaskNotifyTakeIndexed(UI_AVISO_IDX, pdTRUE, 0);   // zera o que sobrou
            s_isr_acorda = true;
          }
        }
      } else {
        // O RELOGIO DO APP e o ritmo of frame (kernel/frame_pacing): o periodo
        // request, em varreduras inteiras of panel.
        const uint32_t per = periodo_of_app();
        if (per && pacing_falta_ms() == 0) { s_dirty_data = true; }

        // AVISOS: o que os servicos disseram que changed since a ultima iteration.
        // So o que o app declarou querer chega a ele; o resto e descartado
        // (a home atualiza a line of servicos in hora, por conta propria).
        //
        // ENCOBERTO (bloqueio ou central of notices), NADA E ENTREGUE: os
        // notices ficam guardados ate o app voltar a ficar visivel. O app atras of screen of bloqueio NAO esta visivel, e
        // o ao_aviso of quem mostra conversa (ESP-NOW) marca everything como lido
        // - era o aviso que sumia of barra sem ninguem open o app: chegava
        // com a screen apagada, o despertar bloqueava, e o app "lia" por tras.
        // A central open envelheceu (one aviso chegou ou foi lido): of novo.
        if (panel_notices_changed()) { s_dirty_layout = true; }
        const uint32_t av = system_app_covered() ? 0 : s_notices.exchange(0, std::memory_order_relaxed);
        if (av) {
          if (app_vivo()) {
            const uint32_t dele = av & s_app->notices;
            if (dele) {
              if (s_app->on_aviso) { s_app->on_aviso(dele); }
              else                 { s_dirty_data = true; }
            }
          } else if (!s_app) {
            atualizar_line_servicos();
          }
        }

        // GIRO DA TELA (kernel/orientation), so para app que segue o IMU. Gira
        // ENTRE quadros, aqui, antes of drawing: a pass complete que vem
        // logo abaixo ja exit in orientation nova. Nunca com finger in screen (nem
        // logo depois dele): one arraste viraria touch em outro lugar, e a
        // soltura cairia num botao que nao estava ali when o finger desceu.
        ETAPA("orientacao (IMU, I2C)");
        const bool pode_rotate = app_vivo() && !s_locked
                             && !tocando && !s_finger_in_screen && (t - s_finger_ms) > 300;
        if (orientation_vigiar(t, pode_rotate)) {
          scroll_reset(&s_rol);            // a rolagem era measurement in outra height
          virtual_screen_invalidate_everything();
          if (s_app->on_rotate) { s_app->on_rotate(); }
          s_dirty_layout = true;             // barra, navigation e app: everything of novo
        }

        ETAPA("desenho (app / home)");
        const int64_t drawing_t0 = esp_timer_get_time();
        const bool drawing_complete = s_dirty_layout;
        comp_marcar_desenho();
        uint32_t fase_barra = 0, fase_content = 0;
        if (s_dirty_layout) {
          const int64_t f0 = esp_timer_get_time();
          draw_barra_status(virtual_screen(), true);
          const int64_t f1 = esp_timer_get_time();
          draw_content(true, true);
          fase_barra    = (uint32_t)(f1 - f0);
          fase_content = (uint32_t)(esp_timer_get_time() - f1);
          // A pagina visivel now reflete exatamente s_rol.current - qualquer
          // rolagem pending of antes deste redraw complete ja exited junto.
          scroll_synchronize(&s_rol);
        } else if (scroll_pending(&s_rol)) {
          // ROLAGEM: a position changed, o VALOR of nada changed. Em vez of
          // caminho of top (limpar ~1,65MB + switch of pagina), deslocamos
          // os pixels JA CORRETOS que estao in screen (tab5_screen_scroll - memcpy
          // em faixas, sem erase nada) e so entao chamamos o app para
          // draw por top - pulando a rajada of erase e a troca of
          // pagina, mas NAO o "app redesenha everything".
          //
          // completo_para_apps=true (mesmo sem limpar_area) e proposital:
          // a faixa que o offset acabou of expor NUNCA foi pintada
          // NESTA position of rolagem antes - se cada app so olhasse o
          // dirty-tracking por field (pintura_mudou_i/f/s), estrutura
          // estatica que so desenha when "e pass complete" (titulo,
          // ajuda, botao - ver app_config) nunca preencheria aquela faixa.
          // Foi exatamente o bug visto on vivo: rolar para bottom parecia bom
          // (a faixa nova e fina, passava despercebida), voltar para top
          // expunha of uma vez o rastro of faixas nunca pintadas.
          const area_t a = area_util();
          const scroll_pass_t p = scroll_scroll(&s_rol, a, TFT_BLACK);
          if (p.moved) {
            draw_content(false, true, &p.exposta);
          } else {
            // Arraste rapido demais para o intervalo entre chamadas (delta
            // maior que a propria area util): nao ha o que reaproveitar, cai
            // para o redraw complete normal no next ciclo.
            s_dirty_layout = true;
          }
        } else if (s_dirty_data && !s_dirty_layout) {
          draw_content(false, false);
        }

        // A barra so e repintada when algo NELA changed - a porcentagem, o
        // estado of carga ou o app. Repintar of dois em dois segundos a toa era
        // outra fonte of piscada, so que numa faixa menor.
        if ((t - s_ultima_barra) >= BARRA_MS) {
          s_ultima_barra = t;
          if (barra_changed()) {
            virtual_screen().startWrite();
            draw_barra_status(virtual_screen(), false);
            virtual_screen().endWrite();
          }

          // Beep of notification + disable o I2S ocioso - ver o aviso grande
          // em atualizar_beep_e_ociosidade_do_som(), mais acima no arquivo.
          atualizar_beep_e_ociosidade_of_sound(t);

          // A line of servicos of home, no mesmo tique e pela mesma regra -
          // ver atualizar_linha_servicos() (kernel/shell.cpp) para o "so
          // redesenha se o text changed".
          atualizar_line_servicos();
        }
        if (comp_tem_sujeira()) {
          const uint32_t us = (uint32_t)(esp_timer_get_time() - drawing_t0);
          const uint32_t cpu = comp_contar_desenho(us, drawing_complete);
          // QUEM PESA. A pass complete media 90-130 ms - a media nao diz
          // of quem. A tabela por app (e por fase) exit no 'screen' of console.
          if (drawing_complete && fase_content) {
            const uint32_t limpar = s_fase_limpar_us;
            anotar_pass(s_locked ? "bloqueio" : (s_app ? s_app->name : "home"), cpu,
                           fase_barra, limpar, fase_content > limpar ? fase_content - limpar : 0);
          }
        }
      }
    }

    // O request of sondagem "amadurece" aqui, depois of drawing: no next
    // ciclo ele sera atendido, com a screen ja mostrando o estado current.
    if (s_grove_resondar) { s_grove_request_visto = true; }

    // Periodo adaptativo: rapido durante o uso, mais espacado when parado.
    //
    // EM REPOUSO (Etapa 2): em vez of one numero fixo (PERIODO_ZZZ),
    // o teto vira "quanto falta para o next prazo REAL que este mesmo loop
    // ja respeita hoje" - a amostragem of sensores (1000ms fixo, ver o bloco
    // s_ultima_medida acima), a barra/beep (BARRA_MS) e a vigia of Grove
    // (VIGIA_MS). Nao inventa nenhum prazo novo, so para of wake entre eles
    // a toa. O prazo of Grove SO entra in conta when ele of fato avanca
    // sozinho (settings_grove() enabled e none re-sondagem em andamento) -
    // achado in revisao deste drawing: sem essa guarda, com o Grove disabled
    // (configuracao normal) ou uma re-sondagem em progress, s_grove_olhado para
    // of andar, a conta "quanto falta" ficaria presa em ZERO para sempre, e o
    // loop giraria sem stop exatamente no estado que deveria dormir.
    uint32_t periodo;
    if (tab5_power_is_sleeping()) {
      if (s_touch_isr_ok) {
        const uint32_t decorrido_measurement = t - s_ultima_measurement;
        uint32_t prazo = (decorrido_measurement < 1000) ? (1000 - decorrido_measurement) : 0;

        const uint32_t decorrido_barra = t - s_ultima_barra;
        const uint32_t prazo_barra = (decorrido_barra < BARRA_MS) ? (BARRA_MS - decorrido_barra) : 0;
        if (prazo_barra < prazo) { prazo = prazo_barra; }

        if (settings_grove() && !s_grove_resondar) {
          const uint32_t decorrido_grove = t - s_grove_olhado;
          const uint32_t prazo_grove = (decorrido_grove < VIGIA_MS) ? (VIGIA_MS - decorrido_grove) : 0;
          if (prazo_grove < prazo) { prazo = prazo_grove; }
        }
        periodo = prazo;
      } else {
        periodo = PERIODO_ZZZ;   // ISR nunca subiu - o mesmo comportamento of sempre
      }
    }
    else {
      // ACORDADA: RAPIDA SO QUANDO HA MOTIVO. Ate aqui o loop
      // rodava a cada 20-25ms sempre que a screen estava acesa - 40 a 50
      // voltas por segundo, cada uma lendo o touch por I2C, com o aparelho
      // parado in mesa. Agora:
      //   - finger in screen (ou ha menos of 500ms) ou work pending: 20ms,
      //     com a interrupcao of touch DESLIGADA (num arraste ela pulsaria a
      //     cada 5-20ms e so atrapalharia);
      //   - parada: a interrupcao LIGA e o loop dorme ate o next prazo
      //     real. Um touch, one aviso of servico ou one request of redraw of
      //     outra tarefa acordam in hora (a espera in notification, la embaixo).
      const bool finger = (t - s_ultimo_touch) < 500;
      const bool pending = s_dirty_layout || s_dirty_data || scroll_pending(&s_rol)
                         || comp_tem_sujeira() || s_app_loading || s_sd_montando
                         || s_grove_resondar || s_probing_em_fundo;
      if (finger || pending || !s_touch_isr_ok) {
        s_isr_acorda = false;
        periodo = PERIODO_UI;
      } else {
        uint32_t prazo = 1000;
        auto menor = [&](uint32_t intervalo, uint32_t since) {
          const uint32_t dec = t - since;
          const uint32_t falta = (dec < intervalo) ? (intervalo - dec) : 0;
          if (falta < prazo) { prazo = falta; }
        };
        menor(1000, s_ultima_measurement);                    // sensores, 1Hz
        menor(BARRA_MS, s_ultima_barra);                 // barra of status
        if (settings_grove() && !s_grove_resondar) { menor(VIGIA_MS, s_grove_olhado); }
        menor(ocioso_ms(), s_ultimo_touch);              // hora of erase a screen
        // (o clock of app entra abaixo, depois of piso of PERIODO_UI)
        if (s_speaker_ocioso_since) { menor(SPEAKER_OCIOSO_MS, s_speaker_ocioso_since); }
        if (tab5_keyboard_present()) { menor(keyboard_ms(), s_ultimo_keyboard); }   // keyboard fisico e consultado
        { const uint32_t falta = orientation_prazo_ms(t); if (falta < prazo) { prazo = falta; } }   // vigia of giro (so com app que segue o IMU)
        if (prazo < PERIODO_UI) { prazo = PERIODO_UI; }
        s_isr_acorda = s_touch_isr_ok;
        periodo = prazo;
      }
      // O RELOGIO DO APP fura o piso of PERIODO_UI: o next frame cai
      // logo depois of uma varredura, e esse momento pode estar a menos of
      // 20 ms (e com o finger in screen tambem - drag nao pode lock uma
      // animacao).
      if (periodo_of_app()) {
        uint32_t falta = pacing_falta_ms();
        if (falta < 1) { falta = 1; }
        if (falta < periodo) { periodo = falta; }
      }
    }

    // UM FRAME POR VOLTA (COMPOSITOR.md, regra 4). Tudo que esta iteration
    // desenhou ja esta in pagina of tras (modo direto), e o rastreador anotou
    // onde. Aqui a troca e requested - sem wait: quem espera e o next
    // frame, on open. Volta sem drawing = nada a fazer.
    bool apresentou = false;
    if (!tab5_power_is_sleeping() && comp_tem_sujeira()) {
      ETAPA("compositor (troca de pagina)");
      apresentou = comp_apresentar();
      if (apresentou) { comp_contar_volta((uint32_t)(esp_timer_get_time() - iteration_t0)); }
    }

    // PROVA DE VIDA. Uma iteration complete of loop terminou - a screen foi tratada,
    // os apps tiveram a vez deles, nada travou. E o sinal que o watchdog
    // inscrito la em top espera; sem ele a cada iteration, os 5s of prazo vencem
    // e o aparelho reinicia mesmo funcionando bem.
    esp_task_wdt_reset();
    s_voltas   = s_voltas + 1;   // escritor unico: esta tarefa
    s_iteration_ms = tab5_now_ms();
    ETAPA("esperando evento");

    // A espera in notification (ulTaskNotifyTake) no lugar of vTaskDelay() of sempre - acorda no MESMO
    // prazo calculado acima OU antes, se algo chegar in queue (touch of
    // verdade em sleep, ou sistema_acordar_interface() of one servico). O
    // CONTEUDO e o reason of return sao ignorados of proposito: as
    // proximas lines of PROXIMO ciclo ja conferem M5.Touch e todas as
    // bandeiras sozinhas, exatamente como sempre fizeram.
    if (apresentou && periodo == PERIODO_UI) {
      // NO RITMO DO PAINEL. Com o finger in screen (ou work pending) a
      // iteration dormia 20 ms DEPOIS of terminar: periodo real = 20 ms + o
      // drawing, ~30 q/s contra one panel of ~63 Hz. Os dois ritmos nao
      // batem, e of tempos em tempos one frame stays in screen dois periodos -
      // a rolagem trepida mesmo sem rasgo nenhum.
      //
      // Aqui a iteration que apresentou espera a TROCA (o panel comecou a varrer
      // o frame novo) e recomeca in hora: touch lido logo depois of troca,
      // frame novo pronto para a seguinte. A espera nao e a mais - o next
      // frame teria of espera-la of qualquer jeito (comp_quadro_imagem,
      // passo 1). Vale SO no modo rapido: parada, a interface continua
      // dormindo ate o next prazo, como sempre.
      comp_esperar_troca();
      ulTaskNotifyTakeIndexed(UI_AVISO_IDX, pdTRUE, 0);   // consome one aviso que ja tenha chegado
    } else {
      ulTaskNotifyTakeIndexed(UI_AVISO_IDX, pdTRUE, pdMS_TO_TICKS(periodo));
    }
    // A cada iteration, nao no passo of 2 s: o pendrive espera esta resposta
    // para enable o USB (ver sistema_som_ceder_pinos).
    sound_atender_reserva();
  }
}

void system_start(void)
{
  //                      body           name  stack param prio handle kernel
  // settings_init() ja foi chamado no app_main, ANTES of M5.begin(), para a
  // screen nascer no brilho certo em vez of piscar no default e corrigir depois.
  pool_start();         // tarefas fixas dos servicos que ligam/desligam e dos apps - ANTES of todos
  tab5_memory_marco("pool de vagas");
  tab5_sd_init();      // cria o mutex of cartao ANTES of haver concorrencia
  tab5_memory_marco("cartao (tarefa)");
  tab5_log_start();     // DEPOIS of SD (usa tab5_sd_mount/open) - guarda os logs no cartao
  tab5_memory_marco("log (tarefa+buffer)");
  tab5_monitor_start(); // mede CPU/memoria o time todo e registra eventos (services/tab5_monitor)
  tab5_memory_marco("monitor (tarefa)");
  tab5_wifi_init();    // cria a queue e a tarefa; NAO liga o radio
  tab5_memory_marco("wifi (tarefa, sem radio)");
  tab5_espnow_start();  // espera o firmware nosso no C6 (peer_data) e liga o ESP-NOW
  tab5_memory_marco("espnow (tarefa)");
  tab5_ble_init();     // cria a tarefa; NAO liga (ver o loop of interface)
  tab5_memory_marco("ble (tarefa, sem radio)");
  console_start();      // commands pelo monitor serial (help)
  tab5_memory_marco("console (tarefa)");

  // ISR DO TOQUE - ver o aviso grande junto of UI_AVISO_IDX,
  // mais acima no arquivo. Precisa existir ANTES of tarefa of interface
  // raise (ela consome a queue since a primeira iteration of loop) e DEPOIS of
  // M5.begin() (main.cpp) - e ele quem configura o GPIO 23 como entrada para
  // o driver of GT911; esta ISR so ACRESCENTA one tipo of interrupcao aquele
  // pino, nunca muda direcao/pull.
  {
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
      // INVALID_STATE = outro modulo ja instalou o servico global of ISR
      // antes of chegarmos aqui (o candidato mais provavel e o bring-up of
      // WiFi/C6 pelo esp_hosted, que faz o mesmo em eh_host_port_gpio.c) -
      // normal e seguro, so precisamos que ALGUEM tenha instalado, nao
      // importa quem.
      // No GT911 o proprio driver ja deixou o pino como entrada. Nos TDDI
      // ninguem configurou: entrada com pull-up (line of interrupcao em
      // dreno open, active em bottom - sem finger ela stays em sleep alto).
      if (!tab5_panel_touch_gt911()) {
        gpio_set_direction(GPIO_NUM_23, GPIO_MODE_INPUT);
        gpio_set_pull_mode(GPIO_NUM_23, GPIO_PULLUP_ONLY);
      }
      if (gpio_set_intr_type(GPIO_NUM_23, GPIO_INTR_ANYEDGE) == ESP_OK &&
          gpio_isr_handler_add(GPIO_NUM_23, isr_touch, nullptr) == ESP_OK) {
        // LIGADA DE VEZ: ela anota todo pulso (s_toque_pulso). Quem decide se
        // ela tambem acorda o loop e s_isr_acorda (sleep / screen parada).
        s_touch_isr_instalada = true;
        s_touch_isr_ok = tab5_panel_touch_gt911();
      }
    }
  }
  if (!s_touch_isr_instalada) {
    ESP_LOGW(TAG, "ISR de toque nao subiu - repouso usa o PERIODO_ZZZ fixo de sempre");
  } else if (!s_touch_isr_ok) {
    ESP_LOGI(TAG, "toque da tela %s: interrupcao so depois de provar o pino (validar_isr_tddi)",
             tab5_panel_name());
  }
  // ISR DO TECLADO (ver isr_teclado). O G50 nao e of mais ninguem: entrada com
  // pull-up, borda of press_start. Se nao raise, o keyboard ainda acorda a screen -
  // so que no next prazo of sleep (ate ~1 s), pela consulta of fila.
  {
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << TAB5_KEYBOARD_INT;
    c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = GPIO_PULLUP_ENABLE;
    c.intr_type = GPIO_INTR_NEGEDGE;
    s_key_isr_ok = gpio_config(&c) == ESP_OK
                  && gpio_isr_handler_add((gpio_num_t)TAB5_KEYBOARD_INT, isr_keyboard, nullptr) == ESP_OK;
    if (!s_key_isr_ok) { ESP_LOGW(TAG, "ISR do teclado (G%d) nao subiu - ele acorda a tela pela consulta de 1 s", TAB5_KEYBOARD_INT); }
  }

  // Handle CAPTURADO, nao NULL - sistema_camera_iniciar()/stop() (ver o
  // aviso grande em app.h) precisam saber se quem as chamou JA E esta
  // tarefa, para nao fazer ponte com elas mesmas (deadlock).
  home_ordem_carregar();   // a order dos icones of user (NVS), antes of first drawing
  // O boot tambem e "a screen ligou" - a mesma regra of despertar. Aqui, antes
  // of tarefa of interface existir: o first drawing ja exit locked.
  lockscreen_on_enable_screen();
  s_clock_feito = xSemaphoreCreateBinary();   // antes of interface: ela of, o console espera
  s_sound_parou     = xSemaphoreCreateBinary();   // idem: ela of, o pendrive espera
  xTaskCreatePinnedToCore(vTaskInterface, "ui", 8192, NULL,   4,  &s_tarefa_interface, 1);
  registro_start();
  tab5_memory_marco("ui + registro (tarefas)");
  ESP_LOGI(TAG, "interface no ar, %d aplicativo(s) registrado(s)", SISTEMA_NUM_APPS);
}


// Ver ETAPA no topo of arquivo. Leitura of outra tarefa, sem trava: palavras
// soltas, e one valor one passo atrasado nao muda o diagnostics.
void system_diag_interface(system_diag_t* d)
{
  const uint32_t now = tab5_now_ms();
  d->etapa          = (const char*)s_etapa;
  d->voltas         = s_voltas;
  d->ms_since_iteration = s_iteration_ms ? now - s_iteration_ms : 0;
  d->ms_since_finger  = s_finger_ms ? now - s_finger_ms : UINT32_MAX;
  d->touch_pulsos   = s_pulsos;
  d->touch_falhas   = s_vig_falhas;
  d->touch_presos   = s_vig_presos;
  d->touch_surdos   = s_vig_surdos;
  d->touch_status   = s_vig_status;
  d->sleep        = tab5_power_is_sleeping();
  d->app            = s_app ? s_app->name : nullptr;
}
