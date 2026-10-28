// O "core" do M5tabOS: agenda de desenho, ciclo de vida dos apps, politica
// de energia.
//
// -------------------------------------------------------------------------
// ETAPA 1 do ARCHITECTURE.md - o MOTOR
// -------------------------------------------------------------------------
// Ate aqui isto vivia dentro de kernel/sistema.cpp, junto da APARENCIA
// (home, barra de status, roteamento de toque, calibracao) - 2457 linhas
// fazendo oito coisas diferentes, compartilhando estaticas sem fronteira.
// Partido em dois, sem mudar uma linha de logica (a unica excecao esta
// documentada em kernel/internal.hpp): este arquivo decide O QUE roda e
// QUANDO; kernel/shell.cpp decide como a tela PARECE. Os dois se falam pelo
// pouco que kernel/internal.hpp expoe - nunca por variavel global crua fora
// dali.
//
// UMA tarefa so manda no display e no I2C, de proposito. O LovyanGFX nao e
// seguro para acesso concorrente e o I2C_Class do M5Unified nao tem mutex
// nenhum - duas tarefas no mesmo barramento leem registrador trocado sem dar
// erro. Por isso os apps NAO desenham das tarefas deles: eles produzem dados
// em queues e a funcao desenhar() e chamada daqui.

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
// O tab5_mesh continua aqui por UM motivo so: quem sobe o servico e o boot,
// dentro da sondagem do Grove (ver vTaskSondagemGrove mais abaixo). Isso e
// ciclo de vida, nao desenho - kernel/shell.cpp (a antiga "proposta de
// kernel/shell" que este comentario ja citava antes da Etapa 1 existir) nao
// precisa conhecer o Mesh, e de fato nao conhece. A dependencia que restou
// e legitima: quem liga um servico por causa do Grove e sempre o core.
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
// ONDE A INTERFACE ESTA - para quando ela trava sem deixar nada no log.
// ---------------------------------------------------------------------------
// Cada passo do loop deixa o nome aqui (ETAPA) e cada volta completa conta.
// O monitor (outra tarefa) le isto 1x/s: loop parado ha mais de 3 s vira
// evento com o NOME do passo em que parou; e o comando 'ui' do console mostra
// na hora, mesmo com a tela congelada. 'dedo' diz se o toque ainda e lido.
static volatile const char* s_etapa   = "boot";
static volatile uint32_t    s_voltas  = 0;
static volatile uint32_t    s_volta_ms = 0;   // quando a ultima volta terminou
static volatile uint32_t    s_dedo_ms  = 0;   // a ultima vez que havia dedo na tela
#define ETAPA(nome) (s_etapa = (nome))

// ---------------------------------------------------------------------------
// O TOQUE DO SISTEMA (toques normais que nao eram reconhecidos)
// ---------------------------------------------------------------------------
// Ate aqui o clique era o wasClicked() do M5Unified, e ele recusa dois casos
// que sao toques normais de um dedo nesta tela:
//   - o dedo andou mais de 8 px entre descer e subir (vira "flick"). A 294 DPI
//     isso e 0,7 mm - o rolar natural da ponta do dedo ja passa disso;
//   - o dedo ficou mais de 500 ms (vira "hold"). Um toque sem pressa some.
// Nos dois casos o toque simplesmente nao acontecia, sem log nenhum.
//
// Agora o reconhecedor e do SISTEMA, no modelo do LVGL (CLICKED): desceu e
// subiu sem arrastar = clique, com qualquer duracao. "Arrastar" = afastou-se
// do ponto de descida mais que TOQUE_TOLERANCIA_PX, ou a rolagem do sistema
// andou (rolagem_foi_arraste). ~2 mm, bem abaixo de qualquer gesto - e o
// MESMO limite das listas (ROLAGEM_ARRASTE_MIN): um limite so no sistema.
#define TOQUE_TOLERANCIA_PX ROLAGEM_ARRASTE_MIN
static bool     s_dedo_na_tela = false;   // o NOSSO estado: desceu e ainda nao subiu
static int      s_toque_x0, s_toque_y0;   // onde desceu
static int      s_toque_x, s_toque_y;     // onde esta (fica o da soltura)
static int      s_toque_desvio = 0;       // maior afastamento da descida, px
static uint32_t s_toque_desde = 0;

// A LEITURA DO GT911 NAO PODE PERDER A SOLTURA. O driver do M5GFX so le o chip
// quando o pino de interrupcao (GPIO 23) esta em nivel baixo; com ele alto,
// devolve o ULTIMO dado lido. Se o aviso da soltura passar entre duas voltas
// do loop, o dado velho diz "dedo na tela" para sempre: nenhum clique, a tela
// nunca entra em repouso, e o proximo toque some junto - a tela parece
// travada ate alguns toques depois. Com o dedo na tela, entao, a leitura e
// SEMPRE pelo I2C (pin_int = -1): o GT911 guarda cada relatorio ate ser lido,
// e o da soltura e lido de certeza. Sem dedo, volta a esperar o pino - a tela
// parada nao gasta I2C (e a ISR do mesmo pino acorda o loop no proximo toque).
//
// E O PULSO NAO PODE SE PERDER (sintoma: a tela entrava em repouso com o
// dedo em uso, com o CreazySnake aberto). O GT911 avisa por PULSO
// no pino, e o driver so olha o NIVEL do pino na hora do M5.update(). Com a
// tela ativa (jogo a 20 ms, ou qualquer coisa pendente) a interrupcao ficava
// desligada, e o loop amostrava o pino a cada 20 ms em fase fixa: se o pulso
// caisse sempre entre duas amostras, NENHUM toque novo era visto - nem pelo
// jogo, nem pela contagem do repouso. Agora a interrupcao fica LIGADA sempre e
// so ANOTA o pulso (s_toque_pulso); a leitura seguinte vai ao I2C de certeza.
// Acordar a queue continua so em repouso/parado (s_isr_acorda) - o motivo de ela
// ter nascido desligada (arraste enchendo a queue) segue valendo.
static volatile bool s_toque_pulso = false;   // a ISR viu um pulso desde a ultima leitura
static volatile bool s_isr_acorda  = false;   // a ISR tambem acorda o loop (repouso / parado)
static volatile uint32_t s_pulsos  = 0;       // pulsos do GT911 desde o boot (vigia do toque)

// ---------------------------------------------------------------------------
// VIGIA DO TOQUE (o toque ficava surdo por um tempo, voltava sozinho, e o
// log nao mostrava nada)
// ---------------------------------------------------------------------------
// O toque podia ficar surdo SEM erro nenhum: o driver do M5GFX nao loga falha
// de I2C, e quando a leitura falha ele devolve o ULTIMO dado lido, calado. Tres
// jeitos de ficar surdo, e a olho nao da para saber qual:
//   1. o I2C do GT911 falhando (barramento interno disputado ou preso);
//   2. o GT911 com um relatorio esperando que ninguem le (o pino INT nao
//      pulsa de novo enquanto o buffer dele nao for limpo);
//   3. pulsos chegando e a leitura voltando sem dedo.
// Uma vez por segundo, com a tela acesa, o vigia le o registrador de estado do
// GT911 DIRETO (0x814E - bit 7 "relatorio pronto", bits 0-3 dedos), cruza com
// o que o sistema viu e com os pulsos da ISR. Anomalia vira LOG (W, que vai
// para o cartao na hora) e a recuperacao e a mais barata que existe: forcar a
// proxima leitura pelo I2C, que limpa o buffer do GT911. Mesma tarefa que le
// o toque - nunca duas leituras dele ao mesmo tempo.
static uint32_t s_vig_falhas = 0, s_vig_presos = 0, s_vig_surdos = 0, s_vig_forcados = 0;
static uint8_t  s_vig_status = 0;

// LUPA. Os avisos de "pulsos sem dedo" vieram so com a tela
// mudando sem parar (Camera, Tarefas) e com o dedo na tela - e o GT911 dizendo
// "zero dedos" com o I2C bom. Suspeita: ruido do painel, e o filtro do GT911
// jogando o toque fora. Para provar, a lupa olha o byte de estado a CADA
// volta por 3 s depois de uma anomalia (so le - quem limpa continua sendo o
// driver) e conta: relatorio com dedo, relatorio vazio, bit 6 ("toque grande"
// - o GT911 marca assim o que ele rejeita como palma/ruido), e sem relatorio.
static uint32_t s_lupa_ate = 0;
static uint32_t s_lupa_com = 0, s_lupa_vazio = 0, s_lupa_grande = 0, s_lupa_nada = 0, s_lupa_falha = 0;

static void lupa_toque(uint32_t t, bool tocando)
{
  if (!s_lupa_ate || !tab5_painel_toque_gt911()) { return; }
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

static void lupa_ligar(uint32_t t)
{
  if (s_lupa_ate) { return; }
  s_lupa_ate = t + 3000;
  s_lupa_com = s_lupa_vazio = s_lupa_grande = s_lupa_nada = s_lupa_falha = 0;
}

static void vigiar_toque(uint32_t t, bool tocando)
{
  // Os registradores lidos aqui sao do GT911. Numa tela TDDI (ST7123/ST7121)
  // o endereco do toque e outro chip, com outro mapa: ler 0x814E la seria
  // escrever um ponteiro de registrador sem sentido no controlador da tela.
  if (!tab5_painel_toque_gt911()) { return; }
  static uint32_t prox = 0, pulsos_ant = 0, dedo_desde = 0, log_falha_ms = 0;
  static bool viu_dedo = false, avisou_dedo_longo = false;
  static int presos_seguidos = 0;

  // CARENCIA DO BOOT. Na subida, o proprio driver do GT911 usa o
  // pino INT como SAIDA (o pulso de wakeup() que escolhe o endereco I2C) e o
  // painel e montado logo em seguida: a ISR contou 282 bordas num segundo sem
  // dedo nenhum, e a lupa confirmou - zero relatorios do GT911. Ruido de
  // partida, nao toque. O vigia so comeca 5 s depois da primeira volta, com a
  // contagem de pulsos zerada nesse instante.
  static bool armado = false;
  if (!armado) {
    if (!prox) { prox = t + 5000; }
    if ((int32_t)(t - prox) < 0) { return; }
    armado = true;
    pulsos_ant = s_pulsos;
    viu_dedo = tocando;
    prox = t + 1000;
    return;
  }

  if (tocando) { viu_dedo = true; if (!dedo_desde) { dedo_desde = t; } }
  else         { dedo_desde = 0; avisou_dedo_longo = false; }
  if ((int32_t)(t - prox) < 0) { return; }
  prox = t + 1000;

  const uint32_t total = s_pulsos;
  const uint32_t pulsos = total - pulsos_ant;
  pulsos_ant = total;
  const bool dedo_na_janela = viu_dedo;
  viu_dedo = tocando;

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
    s_toque_pulso = true;   // tenta pelo caminho do driver tambem
    if (s_vig_falhas == 1 || (t - log_falha_ms) >= 10000) {
      log_falha_ms = t;
      ESP_LOGW("toque", "I2C do GT911 NAO respondeu (%lu falha(s) desde o boot; pino INT=%d, %lu pulso(s)/s)",
               (unsigned long)s_vig_falhas, pino, (unsigned long)pulsos);
    }
    return;
  }

  // 2. Relatorio COM dedo parado no GT911, e o sistema sem dedo - duas sondas
  // seguidas (um toque que acabou de descer entre a leitura e a sonda e normal).
  if ((st & 0x80) && (st & 0x0F) && !tocando) {
    if (++presos_seguidos >= 2) {
      ++s_vig_presos; ++s_vig_forcados;
      s_toque_pulso = true;
      lupa_ligar(t);
      ESP_LOGW("toque", "GT911 com %u dedo(s) esperando e o sistema sem dedo ha %d s "
                    "(status %02X, pino INT=%d, %lu pulso(s)/s) - forcando leitura",
               (unsigned)(st & 0x0F), presos_seguidos, st, pino, (unsigned long)pulsos);
    }
  } else {
    presos_seguidos = 0;
  }

  // 3. Pulsos chegando (o GT911 esta vendo dedo) e nenhum dedo lido o segundo
  // inteiro. Um toque normal pulsa ~10-100x/s; uma soltura, poucas vezes.
  if (pulsos >= 10 && !dedo_na_janela) {
    ++s_vig_surdos; ++s_vig_forcados;
    s_toque_pulso = true;
    lupa_ligar(t);
    ESP_LOGW("toque", "%lu pulso(s) em 1 s e NENHUM dedo lido (status %02X, pino INT=%d) - forcando leitura",
             (unsigned long)pulsos, st, pino);
  }

  // O avesso: dedo "na tela" sem parar - leitura velha repetida (o driver
  // devolve o ultimo dado quando falha). Um aviso por episodio.
  if (dedo_desde && !avisou_dedo_longo && (t - dedo_desde) > 15000) {
    avisou_dedo_longo = true;
    ESP_LOGW("toque", "dedo lido SEM PARAR ha 15 s (status %02X, pino INT=%d, %lu pulso(s)/s) - dedo fantasma?",
             st, pino, (unsigned long)pulsos);
  }
}

// LEITURA CONTINUA ENQUANTO SE DIGITA ('continuo'). O driver do GT911 tem
// dois caminhos: lido ha menos de 128 ms, uma leitura simples; lido ha mais,
// ele DESCARTA o relatorio pendente e espera o seguinte (ate ~24 ms parado,
// com a tarefa de interface presa). Esperando o pino, as pausas entre teclas
// passam de 128 ms quase sempre - e cada tecla nova pagava a espera ao
// descer, justo no relatorio do dedo descendo. Com o teclado aberto e o loop
// no ritmo rapido (toque recente), a leitura vai ao I2C em toda volta e o
// caminho lento nao acontece. Loop dormindo nao entra aqui: com a tela sem
// dedo o GT911 nao gera relatorio, e o caminho lento esperaria os 24 ms
// inteiros a cada despertar.
static void ajustar_leitura_do_toque(bool continuo)
{
  auto* tp = M5.Display.touch();
  if (!tp || !tab5_painel_toque_gt911()) { return; }   // so o driver do GT911 olha o pino
  const bool pulso = s_toque_pulso;
  s_toque_pulso = false;   // antes da leitura: um pulso durante ela vale para a proxima
  const int pino = (s_dedo_na_tela || pulso || continuo) ? -1 : (int)GPIO_NUM_23;
  auto c = tp->config();
  if (c.pin_int != pino) { c.pin_int = pino; tp->config(c); }
}
#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "core";

// ---- geometria -------------------------------------------------------------
// ALTURA_BARRA/ALTURA_NAV moraram aqui ate a Etapa 1; agora vem de
// kernel/internal.hpp porque kernel/shell.cpp tambem precisa delas.

// GEOMETRIA DO BOTAO "INICIO" (a pilula na faixa de navegacao) - moveu para
// kernel/shell.cpp, junto de desenhar_barra_nav() e tratar_toque(), que sao
// quem realmente usa essas constantes agora.

// (REPINTURA_MIN_MS, o freio de 80ms entre repinturas, saiu:
// era limite de BANDA para desenhar na pagina visivel. Com o compositor, tudo
// e desenhado na tela virtual e vai para o painel no ritmo dos frames dele.)
static uint32_t s_ultimo_teclado  = 0;
// Relogio de pixel do painel, em MHz. Ver tab5_power_relogio_dpi() - e a unica
// coisa que reduz de forma PERMANENTE a banda que o painel consome da PSRAM, e
// portanto a margem que sobra para todo o resto desenhar sem piscar.
//
// NAO E MAIS CONSTANTE: virou preferencia do usuario, gravada em
// ajustes_relogio_painel() - o console ('tela relogio') e o app Ajustes gravam ali,
// e o boot aplica o que estiver guardado, em vez de sempre voltar para 60.

// ---- comportamento ---------------------------------------------------------
// Tempo sem toque ate o repouso. Vem do AJUSTE do usuario, nao de constante:
// e preferencia de quem segura o aparelho, igual ao brilho. Ver settings.h para
// o porque dos limites de 15 e 60 s.
static inline uint32_t ocioso_ms(void) { return (uint32_t)ajustes_tela_s() * 1000u; }
static const uint32_t PERIODO_UI  = 20;

// (PERIODO_OCIOSO/OCIOSO_APOS_MS sairam. O loop acordado era
// amostrado a 25ms para nao perder toque rapido - "a cura definitiva e o toque
// por INTERRUPCAO". Ela chegou: parada, a interface dorme com a interrupcao do
// GPIO 23 ligada e acorda no instante do toque. Ver o fim do loop.)

// De quanto em quanto tempo perguntar ao teclado se ha tecla.
//
// Era a cada ciclo (20ms) = 50 transacoes I2C por segundo, sempre. Digitacao
// humana nao passa de ~10 teclas por segundo; 60ms e imperceptivel ao dedo e
// corta a conversa com o teclado a um terco.
static const uint32_t TECLADO_MS = 60;
// Com uma tecla que repete segurada (apagar, seta), le no ritmo da interface:
// a repeticao e de 70 ms, e a 60 ms ela sairia de duas em duas leituras.
static uint32_t teclado_ms(void) { return tab5_teclado_segurando() ? 20 : TECLADO_MS; }
// Periodo do loop em repouso. Foi 200ms e era CEDO DEMAIS para o toque: o
// M5.update() so ve o dedo se ele estiver na tela no instante da amostragem, e
// um toque de 100ms cabe inteiro entre duas amostras de 200ms. Detectar por
// nivel nao resolve se nao ha amostra durante o toque.
//
// A 80ms, um toque humano tipico (150ms ou mais) e visto sempre. HOJE so vale
// quando a ISR do toque nao subiu: com ela, o repouso dorme ate o proximo
// prazo real (~1 s) e o toque acorda o loop pela interrupcao. (O repouso
// baixa a CPU para o piso - 40 MHz, ver tab5_power_repouso.)
static const uint32_t PERIODO_ZZZ = 80;
static const uint32_t BARRA_MS    = 2000;

// ---- estado ----------------------------------------------------------------
const app_t* s_app = nullptr;      // NULL = tela inicial - extern em internal.hpp

// ---------------------------------------------------------------------------
// ROLAGEM POR TOQUE
// ---------------------------------------------------------------------------
// Mora no CORE, e nao em cada app, pela mesma razao que a area util mora:
// e geometria, e geometria repetida em oito lugares vira oito jeitos de errar.
//
// O app declara altura_conteudo e nao pensa mais nisso. Quem desloca o desenho,
// recorta na area, converte as coordenadas do toque e distingue arraste de
// clique e o codigo abaixo - agora delegado a kernel/scroll.hpp, o mecanismo
// COMPARTILHADO com qualquer app que precisar de rolagem propria (o Mesh,
// consolidado - ver o aviso grande em scroll.hpp sobre POR QUE
// isto deixou de ser so deste arquivo).
rolagem_t s_rol;   // zero-init ja e VERTICAL/INICIO - extern em internal.hpp

// Mesma ideia de sistema_travar_navegacao() (shell.cpp), para o mesmo tipo de
// problema: um app com mais de uma "tela" interna (ver app_config, Ajustes/
// galeria de fontes) onde SO UMA parte usa a rolagem generica do sistema. Sem
// isto, a tela que NAO rola (a galeria, que pagina) continuava sendo alvo do
// arraste do core - qualquer dedo na area de conteudo movia s_rol.atual por
// baixo, e o atalho de rolagem (tab5_tela_rolar) deslocava pixels que a
// galeria nunca pediu para mover, estragando o desenho dela.
//
// limite_do_app() (mais abaixo) consulta esta trava antes de tudo.
static bool s_rolagem_travada = false;
void sistema_travar_rolagem(bool t) { s_rolagem_travada = t; }

// DOIS niveis de "sujo", e a distincao entre eles e o que mata o flickering:
//
//   s_sujo_layout - a TELA MUDOU (abriu app, fechou app, acordou do repouso).
//                   Aqui sim vale apagar a area e redesenhar tudo.
//   s_sujo_dados  - so os NUMEROS mudaram. Redesenha por cima, SEM apagar.
//
// Antes havia um flag so, e todo redesenho comecava com um fillRect preto na
// area inteira - 720x1120 pixels apagados e repintados a cada segundo. Isso e
// literalmente uma piscada preta na taxa do redesenho.
// Como os apps escrevem com cor de fundo (setTextColor(frente, fundo)), o texto
// pinta o proprio retangulo e nao precisa de limpeza previa.
bool s_sujo_layout = true;   // extern em internal.hpp - shell.cpp tambem escreve
bool s_sujo_dados  = true;   // extern em internal.hpp

// TELA DE BLOQUEIO. Fica NA FRENTE do que estiver aberto (home ou app): o app
// continua vivo por baixo, so nao desenha nem recebe toque enquanto isto for
// verdadeiro. Ligada ao acordar do repouso (se o ajuste estiver ligado) e
// desligada pelo deslize para cima (tratar_gesto, shell.cpp).
bool s_bloqueado = false;

static uint32_t s_ultimo_toque   = 0;
static uint32_t s_ultimo_desenho = 0;
static uint32_t s_ultima_barra   = 0;
static uint32_t s_acordou_em     = 0;   // instante em que TERMINOU de acordar

// O DEDO QUE ACORDOU A TELA nao pode apertar nada.
//
// Havia so uma carencia de 500ms contada a partir do INICIO do despertar. Mas o
// despertar em si consome esse tempo: remontar o DSI, repintar a tela inteira,
// esperar o frame sair e acender em rampa. Quando isso passa de meio segundo -
// e passa, o proprio log mede -, a carencia vence ANTES de o dedo sair da tela,
// a soltura vira clique, e o aparelho abre o app que estava embaixo do dedo.
//
// Tempo era a medida errada. O que define o problema nao e "quanto demorou", e
// "e o MESMO dedo". Esta bandeira segue a pressao: nasce ao acordar e so morre
// quando o dedo sai da tela. Enquanto ela estiver de pe, nenhum clique passa -
// nao importa se o despertar levou 50ms ou 5s.
static bool s_dedo_do_despertar = false;
static uint32_t s_repouso_falhou = 0;   // instante da ultima tentativa frustrada
static uint32_t s_ultima_medida  = 0;
static int      s_sondas_extra   = 0;   // sondagens do Grove alem da do boot
static uint32_t s_grove_desde    = 0;   // quando a contagem das sondas comecou
static bool     s_grove_resondar = false;  // pedido de re-sondagem pendente
static bool     s_grove_pedido_visto = false;  // ja passou um ciclo desde o pedido
// A sondagem DE VERDADE roda numa tarefa propria - ver
// vTaskSondagemGrove() mais abaixo. Este flag fica true do disparo ate ela
// terminar (pode passar de 10s no pior caso), e existe para (a) nao deixar
// duas sondagens rodarem ao mesmo tempo e (b) sistema_grove_sondando()
// continuar contando a verdade pela duracao inteira, nao so no instante em
// que o pedido foi visto.
static volatile bool s_sondando_em_fundo = false;

// VIGIA DO GROVE. Ver o bloco no loop para o porque de cada numero.
static const uint32_t VIGIA_MS    = 2000;    // de quanto em quanto olhar o pino
static const uint32_t MUDO_MS     = 60000;   // silencio do radio ate desconfiar
// O GPS fala SOZINHO, sem ser perguntado (ver tab5_grove.hpp) - um modulo
// conectado manda sentenca a ~1Hz o tempo todo. Por isso o prazo dele e bem
// mais curto que o do radio: 5s sem nada e sinal forte de cabo puxado, nao
// so uma malha quieta como pode acontecer com o Mesh.
static const uint32_t GPS_MUDO_MS = 5000;
static uint32_t s_grove_olhado   = 0;     // ultima olhada no pino
static bool     s_grove_havia    = false; // ultima leitura de presenca
static int      s_grove_iguais   = 0;     // leituras seguidas iguais

static TaskHandle_t s_tarefa_interface = nullptr;   // capturado na criacao, ver sistema_iniciar()

// ---------------------------------------------------------------------------
// CAMERA DE QUALQUER TAREFA
// ---------------------------------------------------------------------------
// Aqui morava a "ponte da camera": um pedido de fora da interface esperava a
// interface executar tab5_camera_iniciar/parar, porque o SCCB do sensor (driver
// I2C do IDF) e o resto do I2C interno (M5GFX) disputavam o mesmo controlador
// sem trava entre si. O SCCB agora passa pelas transacoes do M5GFX
// (hal/tab5_sccb.cpp), que tem trava por porta - todo o I2C interno e
// serializado, de qualquer tarefa. A ponte saiu; o nome fica para quem chama.
bool sistema_camera_iniciar(int largura, int altura) { return tab5_camera_iniciar(largura, altura); }
void sistema_camera_parar(void) { tab5_camera_parar(); }

// Os dedos da ultima amostra (ver sistema_toques em app.h). So a tarefa de
// interface escreve e so ela le (dentro do ao_tocar_nivel): sem trava.
static sistema_toque_t s_toques[SISTEMA_TOQUES_MAX];
static int             s_n_toques = 0;

// O toque em curso ja foi tratado por nivel (sistema_consumir_toque, app.h).
// Lido e zerado pelo reconhecedor do clique, na soltura.
static bool s_toque_consumido = false;
void sistema_consumir_toque(void) { s_toque_consumido = true; }

int sistema_toques(sistema_toque_t* out, int max)
{
  const int n = s_n_toques < max ? s_n_toques : max;
  for (int i = 0; i < n; ++i) { out[i] = s_toques[i]; }
  return n;
}

// ---------------------------------------------------------------------------
// BLUETOOTH - o botao do usuario manda
// ---------------------------------------------------------------------------
// O boot ligava o Bluetooth se o botao estivesse ligado OU se o Mesh estivesse
// configurado para o Bluetooth - desligar o BT em Ajustes durava ate o
// proximo reinicio. Agora a escolha gravada (ajustes_bluetooth) e a unica
// que liga o radio, e o Mesh pelo Bluetooth sobe e desce junto com ela.
static void bt_mesh_subir(void*)
{
  if (!tab5_mesh_servico_ativo() || tab5_mesh_via() != AJUSTES_MESH_BLUETOOTH) { tab5_mesh_iniciar(); }
}

// ---------------------------------------------------------------------------
// MESH PELA INTERNET QUANDO O NO NAO ESTA
// ---------------------------------------------------------------------------
// Sem o C6L (cabo vazio, ou Bluetooth desligado) e com o Wi-Fi conectado, o
// servico Mesh sobe so pela internet (services/tab5_mesh_mqtt.hpp): as
// mensagens seguem chegando sem o radio ligado. Achar o no depois derruba
// este modo (tab5_mesh_iniciar). Com o Mesh no Bluetooth e o BT ligado, quem
// cobre a falta do no e o proprio servico, que abre a internet enquanto o
// BLE procura.
static void mesh_internet_subir(void*) { tab5_mesh_iniciar_internet(); }
static void mesh_internet_descer(void*)
{
  if (tab5_mesh_servico_ativo() && tab5_mesh_via() == AJUSTES_MESH_INTERNET) { tab5_mesh_parar(); }
}

static void vigiar_mesh_internet(uint32_t t)
{
  static uint32_t s_ultima = 0;
  if (t - s_ultima < 15000) { return; }
  s_ultima = t;
  const bool ativo_net = tab5_mesh_servico_ativo() && tab5_mesh_via() == AJUSTES_MESH_INTERNET;
  // INTERNET ESCOLHIDA em Ajustes: e a via, nao o tapa-buraco - roda mesmo com
  // o C6L no cabo (a prioridade do Grove ja foi exercida no achado; ver
  // vTaskSondagemGrove). So precisa do Wi-Fi.
  if (ajustes_mesh_via() == AJUSTES_MESH_INTERNET) {
    if (!tab5_mesh_servico_ativo() && !s_sondando_em_fundo
     && tab5_wifi_estado() == TAB5_WIFI_CONECTADO) {
      sistema_trabalho_solto(mesh_internet_subir, nullptr);
    }
    return;
  }
  // Internet como tapa-buraco: o Grove com o no a faz ceder, o cabo e o principal.
  if (!ajustes_mesh_internet() || tab5_grove_tipo() == GROVE_MESHTASTIC) {
    if (ativo_net) { sistema_trabalho_solto(mesh_internet_descer, nullptr); }
    return;
  }
  // Sem Wi-Fi o modo so-internet nao tem o que fazer: desce o servico. Duas
  // leituras seguidas (30 s) sem Wi-Fi, para nao derrubar numa piscada da rede.
  static int s_sem_wifi = 0;
  if (ativo_net && tab5_wifi_estado() != TAB5_WIFI_CONECTADO) {
    if (++s_sem_wifi >= 2) { s_sem_wifi = 0; sistema_trabalho_solto(mesh_internet_descer, nullptr); return; }
  } else { s_sem_wifi = 0; }
  const bool no_pelo_bt = ajustes_mesh_via() == AJUSTES_MESH_BLUETOOTH && ajustes_bluetooth();
  if (!tab5_mesh_servico_ativo() && !no_pelo_bt && !s_sondando_em_fundo
   && tab5_grove_tipo() != GROVE_MESHTASTIC
   && tab5_wifi_estado() == TAB5_WIFI_CONECTADO) {
    sistema_trabalho_solto(mesh_internet_subir, nullptr);
  }
}
// BANCADA: toque e abertura de app pedidos pelo console (outra tarefa). A
// interface consome na volta seguinte, como se o dedo tivesse tocado - para
// testar uma tela pela serial sem ninguem no aparelho.
static volatile int s_sim_x = -1, s_sim_y = -1;
static const app_t* volatile s_sim_abrir = nullptr;
static volatile bool s_sim_fechar = false;
void sistema_simular_toque(int x, int y) { s_sim_y = y; s_sim_x = x; }
void sistema_simular_abrir(const app_t* app) { if (app) { s_sim_abrir = app; } else { s_sim_fechar = true; } }

bool sistema_bloqueado(void) { return s_bloqueado; }

// TELA ACESA POR PEDIDO (app.h): um bit por motivo, de qualquer tarefa. A
// interface ve a mascara a cada volta; quando ela zera, o prazo de repouso
// conta DAQUELE instante - soltar depois de uma hora acesa nao pode apagar a
// tela na hora.
static std::atomic<uint32_t> s_acordado{0};
static volatile bool s_pedir_acordar = false;
void sistema_manter_acordado(uint32_t motivo, bool sim)
{
  if (sim) {
    // Pedido novo com a tela ja apagada: impedir o PROXIMO repouso nao basta -
    // o atual segue com o clock no piso e a pessoa sem ver nada. Acorda.
    const uint32_t antes = s_acordado.fetch_or(motivo);
    if (!(antes & motivo) && tab5_power_em_repouso()) { s_pedir_acordar = true; sistema_acordar_interface(); }
  } else {
    s_acordado.fetch_and(~motivo);
  }
}

bool sistema_app_exige_sd(void)
{
  const app_t* a = s_app;
  return a && a != s_app_recusado && a->exige_sd;
}

static void bt_desligar(void*)
{
  // O Mesh sai primeiro: a stack nao pode sumir debaixo de uma conexao aberta.
  if (tab5_mesh_servico_ativo() && tab5_mesh_via() == AJUSTES_MESH_BLUETOOTH) { tab5_mesh_parar(); }
  // E confere: se o enlace ainda esta de pe (parar falhou, ou subiu fora do
  // servico), insiste - derrubar a pilha com ele dentro de um ble_gap_* e o
  // reinicio do aparelho que se via ao desligar o Bluetooth.
  if (mesh_ble_rodando()) { mesh_ble_parar(); }
  if (mesh_ble_rodando()) {
    ESP_LOGE(TAG, "o enlace do Mesh nao saiu - o Bluetooth fica ligado");
    return;
  }
  tab5_ble_desligar();
}

static void bt_aplicar(bool ligar)
{
  if (ligar) {
    tab5_ble_ligar();   // trilho do C6 (I2C): tarefa de interface
    // O iniciar do Mesh le o historico do cartao: no worker.
    if (ajustes_mesh_via() == AJUSTES_MESH_BLUETOOTH) { sistema_trabalho_solto(bt_mesh_subir, nullptr); }
  } else {
    sistema_trabalho_solto(bt_desligar, nullptr);
  }
}

void sistema_bluetooth(bool ligar)
{
  if (ligar != ajustes_bluetooth()) { ajustes_definir_bluetooth(ligar); }
  bt_aplicar(ligar);
}

// Se o cartao foi REALMENTE montado para este app. Sem isto, um app que pediu
// SD mas cuja montagem falhou ainda assim chamava desmontar() ao fechar -
// decrementando uma contagem que nunca foi incrementada. Se o registro de
// energia estivesse com o cartao montado naquele instante, o contador caia a
// zero e o cartao era DESMONTADO POR BAIXO DELE.
static bool s_app_montou_sd = false;

// App cuja montagem de cartao SD esta rodando em tarefa de fundo (ver
// vTaskMontarSDApp mais abaixo). NULL quando nao ha abertura pendente. A
// abertura de verdade - chamar ao_abrir() e liberar a tela para o app - so
// acontece quando esta bandeira volta a NULL.
const app_t* s_app_carregando = nullptr;   // extern em internal.hpp - shell.cpp le em tratar_toque
const app_t* s_app_recusado   = nullptr;   // idem - ver recusar_app()
static volatile bool s_sd_montando  = false;  // true do disparo ate a tarefa terminar
static bool          s_sd_montou_ok = false;  // resultado da ultima montagem de fundo

// OCIOSIDADE DO ALTO-FALANTE - achado, investigando por que a
// trava de PM "i2s_driver" (APB_FREQ_MAX, ver comando "energia travas")
// nunca soltava, mesmo com o aparelho havia muito tempo em repouso.
//
// Causa raiz, achada lendo a biblioteca (managed_components/m5stack__
// m5unified/src/utility/Speaker_Class.cpp): a tarefa interna do alto-falante
// (spk_task) so chama i2s_stop() no ramo ocioso quando M5UNIFIED_I2S_ADC_DAC
// esta definido - isto e, quando o chip tem o DAC analogico interno da
// familia ESP32 classica. O Tab5 usa ESP32-P4 com o codec ES8388 externo via
// I2S "de verdade", nao tem esse DAC, entao esse ramo NUNCA compila aqui - e
// o canal I2S, uma vez aberto pelo primeiro tone()/playWav() da sessao, fica
// ligado (e a trava de clock junto) para sempre, tocando som ou nao.
//
// Nao vale a pena remendar a biblioteca vendorizada: um patch se perde na
// proxima atualizacao do componente. A correcao fica inteira do lado de ca,
// pela API publica: se o alto-falante esta ligado (isRunning()) mas ocioso
// (!isPlaying()) por SPEAKER_OCIOSO_MS seguidos, chamamos M5.Speaker.end() -
// que de fato deleta o canal I2S (i2s_del_channel), soltando a trava. O
// proximo tone()/playWav() reabre sozinho: begin() ja e preguicoso por
// natureza (mesmo mecanismo do "primeiro uso liga o ES8388", ver o aviso em
// settings.h/app_config.cpp), entao nao precisa de nenhum cuidado extra aqui.
//
// SEGURO chamar M5.Speaker.end()/isRunning()/isPlaying() AQUI: e a mesma
// tarefa de interface que ja e a unica a chamar M5.Speaker.tone() (ver o
// bloco "BEEP DE NOTIFICACAO NOVA" mais abaixo) - a regra e "so a tarefa de
// interface fala I2C", e end()/begin() tambem falam com o codec por I2C.
//
// A folga de 4s (bem mais que o passo BARRA_MS de 2s usado para conferir)
// existe para nao religar/desligar o canal entre dois beeps proximos (ex.:
// obturador da camera seguido de uma notificacao).
static const uint32_t SPEAKER_OCIOSO_MS = 4000;
static uint32_t       s_speaker_ocioso_desde = 0;  // 0 = tocando ou desligado


// QUEUE DE EVENTOS - ETAPA 2 do ARCHITECTURE.md, primeira fatia.
//
// Ate aqui o loop principal so sabia "vTaskDelay(periodo) e confira tudo nao
// vezes". Isso significa acordar 12,5x/s (PERIODO_ZZZ=80ms) MESMO COM A TELA
// APAGADA, so para perguntar "aconteceu algo?" - e quase sempre a resposta
// era nao. Medido (comando "energia travas 40"): a trava de PM
// "rtos0" do proprio esp_pm fica presa exatamente porque o core 0 nunca
// fica ocioso por tempo suficiente para ela soltar - ver ARCHITECTURE.md,
// secao 2.2.1.
//
// A mudanca aqui e so em COMO o loop espera, nunca em O QUE ele decide: toda
// bandeira suja, todo pintura_mudou_i/f/s, todo barra_mudou() continua
// EXATAMENTE igual - eles so passam a ser conferidos num ritmo mais esperto
// (o proximo prazo real, ou uma interrupcao de toque de verdade) em vez de
// um numero fixo.
//
// SO O REPOUSO usa o prazo adaptativo (ver o calculo no fim do loop). Acordado,
// o timeout passado a xQueueReceive() e o MESMO PERIODO_UI/PERIODO_OCIOSO de
// sempre - o unico ganho ali e trocar vTaskDelay() por uma espera que TAMBEM
// acordaria antes se algo chegasse na queue, sem mudar a cadencia observavel.
//
// O DESPERTADOR E UMA NOTIFICACAO DE TAREFA, nao mais uma fila. A fila
// carregava um 'evento_t' (toque, servico, tecla) que ninguem lia - o loop
// sempre conferia tudo de novo. Para "acorde a tarefa X" o FreeRTOS tem a
// notificacao direta: mais rapida que a fila, sem RAM de fila, e sem o "fila
// cheia, aviso jogado fora" (ela ACUMULA, e um ulTaskNotifyTake leva todos).
// Quem avisa: a ISR do toque (GPIO 23), a ISR do teclado (G50) e
// sistema_acordar_interface() (notificacoes, servicos, pedidos de outras
// tarefas).
//
// NUM INDICE PROPRIO (1), nao no 0: o indice 0 e o padrao de tudo o que usa
// notificacao por baixo (stream buffers do FreeRTOS, alguns drivers), e um
// aviso nosso nele poderia acordar uma espera que nao e nossa. Com o
// sdkconfig antigo (1 entrada so) cai no 0 - funciona igual ao que era, sem
// a protecao. CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=2 no
// sdkconfig.defaults.
#define UI_AVISO_IDX ((configTASK_NOTIFICATION_ARRAY_ENTRIES) > 1 ? 1 : 0)

static inline void ui_avisar_da_isr(void)
{
  if (!s_tarefa_interface) { return; }
  BaseType_t acordou = pdFALSE;
  vTaskNotifyGiveIndexedFromISR(s_tarefa_interface, UI_AVISO_IDX, &acordou);
  portYIELD_FROM_ISR(acordou);
}

// false = a ISR do toque nao subiu (por algum motivo raro - ver
// sistema_iniciar()); o repouso volta ao PERIODO_ZZZ fixo de sempre, o MESMO
// comportamento que o sistema tinha antes desta fatia. Nunca piora nada, so
// deixa de ganhar o teto adaptativo.
static bool s_toque_isr_ok = false;

// TELA TDDI (ST7123/ST7121): a ISR e instalada, mas so passa a valer depois
// de PROVAR que o pino pulsa - o driver desses toques nem olha o GPIO 23, e
// ninguem testou o pino neles. Ate a prova, s_toque_isr_ok fica false e a
// interface roda no caminho sem interrupcao (20 ms acordada, PERIODO_ZZZ em
// repouso), que funciona com qualquer toque. Prova: 3 bordas durante UM
// toque lido pelo I2C. Ver validar_isr_tddi().
static bool s_toque_isr_instalada = false;

// A INTERRUPCAO NASCE DESLIGADA E SO E LIGADA DENTRO DO REPOUSO - achado na
// revisao deste desenho, antes de escrever a primeira linha: enquanto um
// dedo arrasta na tela, o GT911 pulsa este mesmo pino a cada relatorio novo
// (5-20ms, taxa de atualizacao do proprio controlador). Se a interrupcao
// estivesse ligada o tempo todo alimentando esta queue, e essa MESMA queue
// tambem regesse PERIODO_UI/PERIODO_OCIOSO, a queue nunca ficaria vazia
// durante um arraste - xQueueReceive() retornaria quase instantaneo a cada
// chamada, e o loop deixaria de respeitar os 20/25ms escolhidos a dedo,
// rodando tao rapido quanto o proprio corpo do loop permitisse. Por isso:
// s_isr_acorda liga so no instante em que tab5_power_repouso() tem sucesso (ou
// com a tela parada) e desliga quando a tela volta a ter motivo para rodar a
// 20 ms. a interrupcao em si fica ligada o tempo todo: ela
// ANOTA cada pulso (s_toque_pulso, ver ajustar_leitura_do_toque) - com ela
// desligada, um toque cujo pulso caia entre duas amostras do loop se perdia.
//
// AVISO PARA O FUTURO: isto assume que ninguem reconfigura o GPIO 23 depois
// do boot. E verdade hoje - conferido, Touch_GT911::sleep()/
// wakeup() (que REUTILIZAM este mesmo pino como saida) nunca sao chamados em
// lugar nenhum deste projeto, so M5.Display.sleep()/wakeup() (que so mexe no
// painel). Se um dia alguem adicionar M5.Touch.sleep()/wakeup() para
// economizar energia do controlador de toque, isso vai reconfigurar o pino
// por baixo desta ISR e quebra-la em silencio - o sintoma seria "o repouso
// para de ganhar o teto adaptativo e volta ao PERIODO_ZZZ fixo sem log
// nenhum explicando por que". Se isso acontecer, comece a busca aqui.
// A PROVA DO PINO NUMA TELA TDDI (ver s_toque_isr_instalada). Chamada a cada
// volta acordada; depois de provado, nao faz mais nada.
static void validar_isr_tddi(bool tocando)
{
  static uint32_t base = 0;
  static bool dedo = false;
  if (s_toque_isr_ok || !s_toque_isr_instalada) { return; }
  if (tocando && !dedo) { base = s_pulsos; }
  if (tocando && (s_pulsos - base) >= 3) {
    s_toque_isr_ok = true;
    ESP_LOGI(TAG, "toque da tela %s: o pino INT pulsa - repouso e tela parada por interrupcao",
             tab5_painel_nome());
  }
  dedo = tocando;
}

static void IRAM_ATTR isr_toque(void*)
{
  s_toque_pulso = true;          // sempre: a proxima leitura vai ao I2C
  s_pulsos = s_pulsos + 1;
  if (!s_isr_acorda) { return; } // tela ativa: o loop ja roda a 20 ms, nao acordar a toa
  ui_avisar_da_isr();
}

// TECLADO FISICO ACORDA A TELA. O INT do teclado (G50 do ExtPort1, ativo em
// baixo) desce a cada evento e so sobe quando a fila e lida (o driver limpa o
// INT_STAT a cada leitura) - uma borda por tecla, nunca uma rajada, entao ela
// pode ficar ligada de vez, ao contrario da do toque. So acorda a queue no
// mesmo caso da do toque (s_isr_acorda: repouso ou tela parada); acordada, a
// leitura periodica ja pega a tecla. Sem teclado encaixado o pull-up segura
// o pino em cima e ela nunca dispara.
static bool s_tecla_isr_ok = false;

static void IRAM_ATTR isr_teclado(void*)
{
  if (!s_isr_acorda) { return; }
  ui_avisar_da_isr();
}

// Acorda a interface AGORA, sem esperar o proximo prazo periodico - chamado
// por quem publicou algo que vale a pena perceber na hora (ver
// notification.cpp e a ponte da camera mais abaixo). Seguro chamar de
// QUALQUER tarefa: so poe um evento na queue (nunca bloqueia, 0 de espera) -
// se a queue estiver cheia (o que exigiria 4 avisos sem a interface rodar
// nem uma volta do loop, praticamente impossivel), o evento e descartado sem
// erro, porque o proprio loop ja vai acordar por outro motivo em breve.
// Os avisos pendentes. Atomico porque qualquer tarefa publica e so a
// interface consome - exchange(0) pega tudo de uma vez sem perder o que chegar
// no meio.
static std::atomic<uint32_t> s_avisos{0};

void sistema_avisar(uint32_t avisos)
{
  s_avisos.fetch_or(avisos, std::memory_order_relaxed);
  sistema_acordar_interface();
}

// O periodo do app aberto AGORA (0 = so acorda por aviso ou toque).
static uint32_t periodo_do_app(void)
{
  // Bloqueado: o unico "app" que desenha e a animacao de fundo (~25 Hz) com o
  // relogio por cima (ver desenhar_bloqueio).
  if (s_bloqueado) { return BQ_ANIM_PERIODO_MS; }
  if (!app_vivo()) { return 0; }
  return s_app->periodo_agora ? s_app->periodo_agora() : s_app->periodo_ms;
}

void sistema_acordar_interface(void)
{
  if (!s_tarefa_interface) { return; }
  xTaskNotifyGiveIndexed(s_tarefa_interface, UI_AVISO_IDX);
}

// BEEP DE NOTIFICACAO NOVA + DESLIGAR O I2S OCIOSO,
// estendido para tambem valer EM REPOUSO, e para respeitar
// silenciar POR FONTE (a Trilha muda e o Mesh tocando, por exemplo). A pergunta "houve novidade que merece
// som?" mora inteira em notif_houve_novidade_sonora() (notification.cpp) -
// este arquivo nao sabe, e nao precisa saber, quantas fontes existem, os
// nomes delas, ou quais estao silenciadas. Isto comparava o
// TOTAL agregado contra um cache aqui mesmo (s_notif_total_beep) - tinha que
// mudar porque o total nao diz QUAL fonte subiu, e sem isso nao ha como
// silenciar uma sem silenciar todas.
//
// Isto so rodava com a tela acesa: tocar um som prendia a
// trava de PM do I2S para sempre (ver o aviso grande em s_speaker_ocioso_desde
// acima), o que destruiria o proprio motivo do repouso existir. Com o
// watchdog abaixo (M5.Speaker.end() apos SPEAKER_OCIOSO_MS sem tocar nada), o
// repouso deixou de ser incompativel com o beep: toca, e o proprio watchdog
// desliga de novo poucos segundos depois - mesmo com a tela apagada. E por
// isso que esta funcao virou uma funcao separada em vez de ficar inline no
// bloco de repintura: ela e chamada TAMBEM do ramo em repouso, onde nao ha
// tela para desenhar.
//
// NUNCA desenha nada aqui, de proposito - com o DSI desmontado (repouso),
// qualquer chamada a M5.Display seria, na melhor das hipoteses, inutil.
//
// SEGURO chamar M5.Speaker.tone()/isRunning()/isPlaying()/end() AQUI: esta e
// a tarefa de interface, a unica que pode falar I2C (liga o ES8388 no
// primeiro uso - ver o aviso grande em settings.h/app_config.cpp) tanto
// acordada quanto em repouso (o INA226 e o IMU ja fazem I2C em repouso, ver
// vTaskInterface mais abaixo).
// BIP COM O CODEC FRIO. Medido no aparelho: o primeiro bip depois de o
// alto-falante ter sido desligado (SPEAKER_OCIOSO_MS) saia mudo, mesmo no
// volume maximo; o segundo, dentro da janela de 4 s, tocava. O begin() liga o
// ES8388 e o amplificador, mas o DAC so fica audivel depois da rampa
// "click-free" do codec - um tom de 80-90 ms inteiro cabe dentro dela.
//
// Solucao sem bloquear a interface: com o codec frio, toca um bloco unico
// "silencio + tom" (o silencio cobre o aquecimento). Fica em PSRAM e so e
// alocado no primeiro uso. Dois blocos alternados, para um segundo pedido
// poder trocar o som em curso sem reescrever o que o alto-falante esta lendo.
// SOM_AQUECER_MS e o ajuste: se ainda perder o comeco, aumente.
static const uint32_t SOM_AQUECER_MS = 180;
static const uint32_t SOM_FS         = 8000;   // tons ate ~2 kHz: sobra
static const size_t   SOM_AMOSTRAS   = SOM_FS * 3 / 10;   // 300 ms
static int16_t*       s_som_bloco[2] = {nullptr, nullptr};
static int            s_som_vez      = 0;
static uint32_t       s_som_quente_em = 0;   // quando o codec esta pronto

// O ALTO-FALANTE E O USB DIVIDEM PINOS. O I2S do alto-falante sai no GPIO 26
// (dados) e 27 (BCK) - no P4, os pinos do PHY USB 1. O ESP-IDF supoe que esse
// PHY e sempre do OTG (hal/esp32p4/.../gpio_ll.h: gpio_ll_func_sel e
// gpio_ll_pullup_dis): ao configurar esses pinos ele DESLIGA o pad do OTG
// (usb_pad_enable = 0) e o pull-up do D+. Durante o pendrive o OTG esta no
// USB-C (PHY 0, trocado - ver tab5_flashdrive.hpp), entao um bip no meio da
// sessao tirava o disco do computador: o Mac via o cabo sair (sem reset
// nenhum) e o Tab5 seguia esperando o proximo comando. O fim da tarefa do
// alto-falante (spk_task: pinMode no GPIO 26) faz o mesmo.
//
// Enquanto o pendrive pedir, o alto-falante fica desligado e mudo. Quem
// desliga e esta tarefa (a unica que fala com o codec); o pendrive espera a
// confirmacao antes de subir o USB.
static volatile bool s_som_reservado = false;   // o pendrive pediu os pinos
static volatile bool s_som_parado    = false;   // a interface ja desligou o alto-falante
// Dado pela interface quando o alto-falante parou: quem pediu ESPERA NELE, e
// acorda no instante da confirmacao (era um laco perguntando a cada 10 ms -
// o mesmo padrao que o kernel/service_stop.hpp aposentou nos servicos).
static SemaphoreHandle_t s_som_parou = nullptr;   // criado em sistema_iniciar

static void som_atender_reserva(void)
{
  if (!s_som_reservado || s_som_parado) { return; }
  if (M5.Speaker.isRunning()) { M5.Speaker.end(); }
  s_speaker_ocioso_desde = 0;
  s_som_parado = true;
  if (s_som_parou) { xSemaphoreGive(s_som_parou); }
}

bool sistema_som_ceder_pinos(uint32_t espera_ms)
{
  if (!s_som_parou) { return false; }
  xSemaphoreTake(s_som_parou, 0);   // sobra de um pedido anterior nao vale para este
  s_som_parado = false;
  s_som_reservado = true;
  sistema_acordar_interface();
  return xSemaphoreTake(s_som_parou, pdMS_TO_TICKS(espera_ms)) == pdTRUE || s_som_parado;
}

void sistema_som_devolver_pinos(void)
{
  s_som_reservado = false;
  s_som_parado = false;
}

void sistema_beep(float hz, uint32_t ms)
{
  if (s_som_reservado) { return; }   // pinos com o USB (ver acima)
  const uint32_t t = tab5_agora_ms();
  if (!M5.Speaker.isRunning()) { s_som_quente_em = t + SOM_AQUECER_MS; }
  const int32_t falta = (int32_t)(s_som_quente_em - t);

  if (falta <= 0) {
    M5.Speaker.tone(hz, ms);
    return;
  }

  // Codec ainda subindo. begin() explicito: o silencio tem de comecar a
  // contar do instante em que o codec foi ligado, nao de um begin() preguicoso
  // la dentro do playRaw.
  if (!M5.Speaker.begin()) { M5.Speaker.tone(hz, ms); return; }
  const size_t lead = (size_t)falta * SOM_FS / 1000;
  size_t       tom  = (size_t)ms * SOM_FS / 1000;
  if (lead >= SOM_AMOSTRAS) { M5.Speaker.tone(hz, ms); return; }
  if (lead + tom > SOM_AMOSTRAS) { tom = SOM_AMOSTRAS - lead; }

  s_som_vez ^= 1;
  if (s_som_bloco[s_som_vez] == nullptr) {
    s_som_bloco[s_som_vez] = (int16_t*)heap_caps_malloc(SOM_AMOSTRAS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  }
  int16_t* b = s_som_bloco[s_som_vez];
  if (b == nullptr) { M5.Speaker.tone(hz, ms); return; }

  memset(b, 0, lead * sizeof(int16_t));
  const size_t rampa = SOM_FS / 400;   // 2,5 ms de entrada/saida: sem estalo
  for (size_t i = 0; i < tom; ++i) {
    float g = 1.0f;
    if (i < rampa)            { g = (float)i / (float)rampa; }
    else if (tom - i < rampa) { g = (float)(tom - i) / (float)rampa; }
    b[lead + i] = (int16_t)(30000.0f * g * sinf(2.0f * (float)M_PI * hz * (float)i / (float)SOM_FS));
  }
  M5.Speaker.playRaw(b, lead + tom, SOM_FS, false, 1, -1, true);
}

static void atualizar_beep_e_ociosidade_do_som(uint32_t t)
{
  if (notif_houve_novidade_sonora()) {
    sistema_beep(1568.0f, 90);   // Sol5 - curto e agudo, "notificacao", nao "confirmacao"
  }

  // So entra aqui se algum tone()/playWav() ja rodou nesta sessao
  // (isRunning() == false antes disso, nada a fazer).
  if (M5.Speaker.isRunning()) {
    if (M5.Speaker.isPlaying()) {
      s_speaker_ocioso_desde = 0;
    } else if (s_speaker_ocioso_desde == 0) {
      s_speaker_ocioso_desde = t;
    } else if (t - s_speaker_ocioso_desde >= SPEAKER_OCIOSO_MS) {
      M5.Speaker.end();
      s_speaker_ocioso_desde = 0;
    }
  }
}

// DE FORA DA INTERFACE, ACORDA ELA. Parada, a interface dorme
// ate o proximo prazo (ver o fim do loop) - sem isto, o pedido de uma tarefa
// de app esperaria ate 1s. De DENTRO da interface so marca: acordar a si mesma
// a cada desenho faria o loop girar sem nunca dormir.
static inline void acordar_se_de_fora(void)
{
  if (xTaskGetCurrentTaskHandle() != s_tarefa_interface) { sistema_acordar_interface(); }
}

void sistema_pedir_redesenho(void) { s_sujo_dados = true; acordar_se_de_fora(); }

// Redesenho COMPLETO: o sistema limpa a area e o app repinta tudo (a passada
// chega a ele como completa - pintura_completa). O pedir_redesenho() comum e
// incremental: o app redesenha so o que mudou. Os dois passam pela tela
// virtual e pelo compositor; a diferenca e so quanto o app repinta.
//
// Regra: mudou um valor, use pedir_redesenho(). Mudou a TELA, use este.
void sistema_pedir_redesenho_completo(void) { s_sujo_layout = true; acordar_se_de_fora(); }

// TROCA DO RELOGIO DO PAINEL SO NA TAREFA DE INTERFACE. Trocar o relogio
// destroi e recria o painel - e as paginas junto. No modo direto a interface
// escreve NAS PAGINAS durante toda a passada de desenho: destruir o painel de
// outra tarefa (o console) no meio disso seria escrever em memoria liberada.
// Quem nao e a interface deixa o pedido, acorda a interface e ESPERA NO
// SEMAFORO que ela da ao terminar - nada de perguntar a cada 10 ms.
static std::atomic<int> s_relogio_pedido{0};
static std::atomic<int> s_relogio_resposta{0};   // 1 = ok, -1 = falhou
static SemaphoreHandle_t s_relogio_feito = nullptr;   // criado com a interface (sistema_iniciar)

bool sistema_trocar_relogio_painel(int mhz)
{
  if (sistema_na_interface() || !s_tarefa_interface) { return tab5_power_relogio_dpi(mhz); }
  if (!s_relogio_feito) { return false; }
  xSemaphoreTake(s_relogio_feito, 0);   // sobra de um pedido que desistiu de esperar
  s_relogio_resposta.store(0);
  s_relogio_pedido.store(mhz);
  sistema_acordar_interface();
  if (xSemaphoreTake(s_relogio_feito, pdMS_TO_TICKS(5000)) != pdTRUE) { return false; }
  return s_relogio_resposta.load() == 1;
}

static void atender_pedido_de_relogio(void)
{
  const int mhz = s_relogio_pedido.exchange(0);
  if (mhz == 0) { return; }
  s_relogio_resposta.store(tab5_power_relogio_dpi(mhz) ? 1 : -1);
  xSemaphoreGive(s_relogio_feito);
}

// Ver o aviso em app.h. s_sujo_layout=true garante que o proximo redesenho
// completo (que sincroniza s_rol.no_visor sozinho, no loop principal, via
// rolagem_sincronizar()) trate o topo como o estado atual - nao precisa
// mexer em s_rol.no_visor aqui tambem.
void sistema_zerar_rolagem(void) { rolagem_zerar(&s_rol); s_sujo_layout = true; }

const char* sistema_app_atual(void) { return s_app ? s_app->nome : nullptr; }
bool sistema_na_interface(void) { return s_tarefa_interface && xTaskGetCurrentTaskHandle() == s_tarefa_interface; }

static area_t area_util(void)
{
  area_t a;
  a.x       = 0;
  a.y       = ALTURA_BARRA;
  a.largura = M5.Display.width();
  a.altura  = M5.Display.height() - ALTURA_BARRA - ALTURA_NAV;
  return a;
}

// Publicada em app.h: e a UNICA fonte correta da geometria para os apps (e
// para kernel/shell.cpp, que a usa pela mesma porta publica em vez da
// area_util() privada - ver o aviso em kernel/internal.hpp). Antes os apps
// copiavam as alturas daqui, e a copia envelhecia sozinha.
area_t sistema_area(void) { return area_util(); }

// ---------------------------------------------------------------------------
// Perfil de boot: a uniao do que os apps precisam que EXISTA
// ---------------------------------------------------------------------------
tab5_perfil_t sistema_perfil_de_boot(void)
{
  tab5_perfil_t u;
  u.brilho = 80;

  // So as flags de FASE 1 - as que decidem se o driver chega a ser
  // inicializado. Se UM app precisa do IMU, o IMU existe para todos; nao ha
  // como inicializa-lo depois do M5.begin().
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const tab5_perfil_t& p = SISTEMA_APPS[i]->perfil;
    u.imu          = u.imu          || p.imu;
    u.microfone    = u.microfone    || p.microfone;
    u.alto_falante = u.alto_falante || p.alto_falante;
    u.rtc          = u.rtc          || p.rtc;
  }
  return u;
}

// TODA preferencia salva entra no perfil AQUI, e em nenhum outro lugar.
//
// Nasceu de um bug: o Grove ficava desligado no boot mesmo com a
// preferencia gravada como ligada, e so acendia quando algum app era aberto -
// porque a regra morava no caminho de abrir app e o boot nao passava por ele.
// A tela mostrava "ON" e o hardware estava OFF, que e a pior combinacao.
//
// Regra: o perfil do app diz o que ELE precisa; esta funcao impoe o que o
// USUARIO pediu. O app so pode SOMAR - por isso tudo aqui e "||", nunca "=".
void sistema_impor_preferencias(tab5_perfil_t& p)
{
  p.brilho   = ajustes_brilho();                          // preferencia vence sempre
  // "necessario" = a stack ja subiu neste boot (tab5_wifi_pilha_de_pe, HAL)
  // OU o usuario quer WiFi ligado (ajustes_wifi, servico) - a conta mora
  // AQUI, porque so o kernel pode ler as duas sem inverter
  // a dependencia (ver WRITING_APPS.md secao 8).
  p.radio    = p.radio    || tab5_wifi_pilha_de_pe() || ajustes_wifi()
             || ajustes_bluetooth();   // o C6 e o radio do Bluetooth
  p.grove_5v = p.grove_5v || ajustes_grove();

  // CAMERA VIROU SERVICO (contagem de referencia, ver o aviso
  // grande em tab5_camera.hpp) - MESMA RAZAO do Grove acima: um app aberto
  // AGORA nao pode decidir sozinho se desliga CAM_RST, porque quem esta
  // usando a camera pode ser outra coisa (o comando de console 'foto', por
  // exemplo) que nao passa pelo perfil de app nenhum. tab5_camera_ativa()
  // OR'd aqui garante que reaplicar o perfil de QUALQUER app, a qualquer
  // momento, nunca corta a energia de um consumidor que segue vivo.
  p.camera   = p.camera   || tab5_camera_ativa();

  // PARTES DE FUNDO (kernel/background.h): o que cada uma declarou precisar entra
  // aqui - pelo MESMO motivo da camera acima: quem esta na tela nao pode
  // desligar o WiFi de um envio que roda em fundo.
  fundo_somar_perfil(p);
}

static tab5_perfil_t perfil_da_home(void)
{
  tab5_perfil_t p;                    // nada ligado
  sistema_impor_preferencias(p);      // o que o usuario pediu entra aqui
  return p;
}

// Declarado em app.h. Monta o perfil de QUEM ESTA NA TELA agora - o app aberto
// ou a home - e aplica. E o unico jeito de um ajuste valer imediatamente sem o
// app mexer no hardware por fora, por cima da regra de que ele so pode somar.
// PEDE uma re-sondagem do Grove. Nao bloqueia: o trabalho acontece no proximo
// ciclo do loop de interface, que PARA o dono atual (Mesh ou GPS, os dois sao
// servico agora - ver o bloco de resondagem la embaixo) antes de sondar de
// novo. Isto recusava quando o dono era GPS, porque o
// Azimutal lia a UART direto e uma sondagem por cima quebraria a leitura;
// GPS virou servico com o mesmo desligamento limpo que o Mesh ja tinha, e a
// recusa deixou de fazer sentido.
bool sistema_grove_redetectar(void)
{
  s_grove_resondar = true;
  s_sujo_layout = true;
  return true;
}

// Verdadeiro do pedido ate a tarefa de sondagem terminar de verdade - nao so
// no instante do pedido. Ver s_sondando_em_fundo.
bool sistema_grove_sondando(void) { return s_grove_resondar || s_sondando_em_fundo; }

void sistema_reaplicar_perfil(void)
{
  tab5_perfil_t p = app_vivo() ? s_app->perfil : tab5_perfil_t();
  sistema_impor_preferencias(p);
  tab5_power_aplicar(p);
}

// limpar_area = true SO quando a tela mudou de conteudo (abriu/fechou app).
// Nos redesenhos de dado, o conteudo e pintado por cima do anterior.
// O app precisa saber quando o sistema limpou a area embaixo dele. Sem isso,
// um app que pinta por diferenca (guardando o que ja desenhou) fica com a tela
// EM BRANCO depois do repouso: ele acha que nada mudou, mas tudo foi apagado.
// Foi o que aconteceu com o app Ajustes.
static bool s_area_limpa = false;
bool sistema_area_foi_limpa(void) { return s_area_limpa; }
float sistema_quadro_dt(void) { return ritmo_dt(); }

// NAO E rolagem_limite() do modulo generico (kernel/scroll.hpp), de
// proposito - essa aqui consulta s_rolagem_travada e s_app->altura_conteudo,
// que sao politica de app/nucleo, nao geometria de rolagem pura. Nome
// diferente para nao convidar a confundir as duas.
static int limite_do_app(const area_t& a)
{
  if (s_rolagem_travada) { return 0; }
  if (!s_app || s_app->altura_conteudo <= a.altura) { return 0; }
  return s_app->altura_conteudo - a.altura;
}

// 'limpar_area' manda FISICAMENTE apagar a area (tab5_tela_limpar + barra de
// nav) - custa a rajada grande de PSRAM. 'completo_para_apps' e o que cada
// app VE em pintura_completa(): TRUE diz "redesenhe sua estrutura estatica
// inteira (titulo/ajuda/botao guardados atras de um 'if (s_tudo)'), nao so os
// campos que mudaram de valor".
//
// Os dois eram o MESMO parametro, porque so existiam dois
// casos: repintura completa (os dois true - abrir/fechar app, acordar,
// redesenho pedido) ou incremental pura (os dois false - so um dado mudou,
// nada mexeu de posicao). O deslocamento de rolagem (tab5_tela_rolar,
// ver o loop principal) criou um TERCEIRO caso: a posicao na tela MUDOU (os
// pixels foram deslocados, nao apagados) mas ninguem apagou nada
// fisicamente. Chamando com (false, true): pula a rajada de apagar (os
// pixels que nao mudaram de valor ja estao certos, o deslocamento cuidou
// disso) mas ainda avisa cada app que ele PRECISA redesenhar a propria
// estrutura estatica - porque a faixa recem-exposta pelo deslocamento nunca
// foi pintada NESTA posicao antes, e um app cujo titulo/botao so desenha
// quando s_tudo e verdadeiro (a maioria de Ajustes faz isso) simplesmente
// nunca preencheria aquela faixa. Foi visto ao vivo: rolar para baixo
// parecia bom (a faixa nova exposta e fina, o vazio passava despercebido),
// mas voltar para cima expunha de uma vez o rastro acumulado de faixas nunca
// pintadas - "tudo sumia".
//
// 'recorte_app' - so usado pelo caminho de deslocamento de
// rolagem (ver o loop principal): limita a escrita de pixels do app a UMA
// FAIXA da area util, em vez da area inteira. Correto porque o app e chamado
// com a MESMA 'rolada' de sempre (nao sabe que existe recorte) e o
// LovyanGFX descarta silenciosamente qualquer pixel fora do recorte - os
// pixels fora da faixa ja estao certos (o deslocamento cuidou disso antes de
// chamar aqui), entao nao ha diferenca visual em pula-los, so MUITO menos
// bytes escritos na PSRAM. nullptr (o padrao) usa a area util inteira, igual
// a antes desta data.
// MEDIDA DA PASSADA COMPLETA, por fase (ver sistema_passadas_completas).
static uint32_t s_fase_limpar_us = 0;   // a ultima limpeza da area (+ nav)

#define PASSADAS_MAX 24
static sistema_passada_t s_passadas[PASSADAS_MAX];
static int s_n_passadas = 0;

static void anotar_passada(const char* nome, uint32_t us, uint32_t barra, uint32_t limpar, uint32_t conteudo)
{
  sistema_passada_t* p = nullptr;
  for (int i = 0; i < s_n_passadas; ++i) { if (s_passadas[i].nome == nome) { p = &s_passadas[i]; break; } }
  if (!p) {
    if (s_n_passadas >= PASSADAS_MAX) { return; }
    p = &s_passadas[s_n_passadas++];
    *p = {};
    p->nome = nome;
  }
  ++p->n;
  p->us += us;
  p->us_barra += barra;
  p->us_limpar += limpar;
  p->us_conteudo += conteudo;
  if (us > p->max_us) { p->max_us = us; }
}

// Leitura do console (outra tarefa) sem trava: copia de numeros de
// diagnostico; um valor torto num instante raro nao engana ninguem.
int sistema_passadas_completas(sistema_passada_t* out, int max)
{
  const int n = s_n_passadas < max ? s_n_passadas : max;
  for (int i = 0; i < n; ++i) { out[i] = s_passadas[i]; }
  return n;
}

// A tela do sistema no lugar de um app recusado (ver recusar_app).
static void desenhar_recusa(lgfx::LovyanGFX& d, const area_t& a)
{
  const int cx = a.x + a.largura / 2;
  int y = a.y + a.altura / 2 - 90;
  d.setTextDatum(middle_center);
  tab5_texto(d, 3);
  d.setTextColor(TFT_ORANGE, TFT_BLACK);
  char l[64];
  snprintf(l, sizeof(l), "%s precisa do cartão SD", s_app->nome);
  d.drawString(l, cx, y);
  tab5_texto(d, 2);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  y += 70;
  d.drawString("O cartão está no computador (modo pendrive).", cx, y);
  y += 44;
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString("Ele volta ao ejetar no computador,", cx, y);
  y += 40;
  d.drawString("ou em Ajustes > USB. O app abre sozinho.", cx, y);
  d.setTextDatum(top_left);
  tab5_fonte_corpo(d);
}

static void desenhar_conteudo(bool limpar_area, bool completo_para_apps,
                               const area_t* recorte_app = nullptr)
{
  // A TELA VIRTUAL, nao o M5.Display (COMPOSITOR.md, etapa 3a): nada aqui
  // escreve mais na pagina que o painel varre.
  auto& d = tela_virtual();
  area_t a = area_util();

  // TODO quadro passa pelo ritmo (kernel/frame_pacing): agenda o proximo periodico e
  // da o dt de quem anima. O dono e quem esta desenhando AGORA - trocou, o
  // relogio recomeca.
  static char DONO_BLOQUEIO, DONO_AVISOS, DONO_HOME;   // so os enderecos importam
  const void* dono = s_bloqueado ? (const void*)&DONO_BLOQUEIO
                   : s_painel_avisos ? (const void*)&DONO_AVISOS
                   : s_app ? (const void*)s_app : (const void*)&DONO_HOME;
  ritmo_quadro(dono, periodo_do_app());

  d.startWrite();
  if (limpar_area) {
    // EM FAIXAS, nao num fillRect so. A area util e ~1,6MB de escrita na PSRAM
    // e o painel DSI le da mesma memoria sem parar; as duas rajadas juntas
    // secam o FIFO dele e o frame sai AZUL. Ver tab5_screen.hpp - o driver do
    // IDF descreve o sintoma com essas palavras.
    //
    // Era o que acontecia ao SAIR de um app para a home: a home e quase toda
    // preta, e area preta grande e o preenchimento mais rapido e contiguo que
    // existe. Abrir um app escreve o mesmo tanto, mas espalhado em dezenas de
    // operacoes pequenas - e por isso nunca piscava na entrada.
    const int64_t limpar_t0 = esp_timer_get_time();
    tab5_tela_limpar(a.x, a.y, a.largura, a.altura, TFT_BLACK);
    // AVISA A CAMADA DE TELA. Daqui ate o fim desta passada, todo pedido de
    // limpar preto dentro desta area e dispensado - e a rajada dobrada que
    // produzia a piscada azul deixa de acontecer, para TODO app, sem que
    // nenhum deles precise saber da regra.
    tab5_tela_marcar_limpo(a.x, a.y, a.largura, a.altura, TFT_BLACK);
    if (s_bloqueado) {
      // Bloqueado nao ha navegacao: a faixa de baixo fica preta.
      tab5_tela_limpar(0, d.height() - ALTURA_NAV, d.width(), ALTURA_NAV, TFT_BLACK);
    } else {
      desenhar_barra_nav(d);
    }
    s_fase_limpar_us = (uint32_t)(esp_timer_get_time() - limpar_t0);
  }
  s_area_limpa = completo_para_apps;
  // s_app E s_app_carregando (nao so um deles): os dois comecam NULL, e
  // "nullptr == nullptr" e verdadeiro - sem o s_app aqui, a home (sem app
  // nenhum aberto) caia nesta condicao e tentava ler s_app->nome com s_app
  // nulo. Foi o crash do boot: Load access fault bem nesta linha.
  if (s_bloqueado) {
    desenhar_bloqueio(d, a, limpar_area);
  } else if (s_painel_avisos) {
    desenhar_painel_avisos(d, a, limpar_area);
  } else if (s_app && s_app == s_app_recusado) {
    desenhar_recusa(d, a);
  } else if (s_app && s_app == s_app_carregando) {
    // MONTANDO O SD AINDA. s_app ja aponta para o app (ver abrir_app()), mas
    // ao_abrir() dele NAO foi chamado - o app pode nao ter estado nenhum para
    // desenhar, e chamar s_app->desenhar() aqui seria confiar em memoria que o
    // proprio app ainda nao teve chance de preparar. Uma tela de espera simples
    // ate concluir_abertura_do_sd() liberar o app de verdade.
    d.setTextDatum(middle_center);
    tab5_texto(d, 2);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    char msg[48];
    snprintf(msg, sizeof(msg), "abrindo %s...", s_app->nome);
    d.drawString(msg, a.x + a.largura / 2, a.y + a.altura / 2);
  } else if (s_app) {
    const int max = limite_do_app(a);
    if (s_app->desenhar && max > 0) {
      // RECORTE PRIMEIRO. Com o conteudo deslocado, o app desenharia por cima
      // da barra de status e da navegacao sem perceber - ele nao sabe que
      // existe rolagem. O recorte e o que torna essa ignorancia segura.
      //
      // recorte_app, quando vem preenchido, ESTREITA esse recorte para so a
      // faixa recem-exposta (ver o aviso grande no topo da funcao) - o app
      // continua recebendo a MESMA 'rolada' de sempre, sem saber da
      // diferenca.
      const area_t& clip = recorte_app ? *recorte_app : a;
      d.setClipRect(clip.x, clip.y, clip.largura, clip.altura);
      area_t rolada = a;
      rolada.y     -= s_rol.atual;
      rolada.altura = s_app->altura_conteudo;
      s_app->desenhar(d, rolada);
      d.clearClipRect();
    } else if (s_app->desenhar) {
      s_app->desenhar(d, a);
    }
  } else if (limpar_area) {
    // ICONE NAO MUDA. A home e estatica: oito icones e oito rotulos, sempre os
    // mesmos. O que se mexe nela - bateria, temperatura, a linha de servicos -
    // e desenhado por fora, com caminho proprio e comparacao antes de pintar.
    //
    // Redesenhar tudo numa passada de DADOS era repintar 578KB para chegar ao
    // mesmo desenho. Numa transicao isso e a diferenca entre caber e nao caber
    // na banda que sobra do painel; e a passada de dados acontece na pagina
    // VISIVEL, sem a protecao da troca de pagina.
    //
    // A condicao e exatamente a certa: so ha o que redesenhar aqui quando a
    // area foi APAGADA - e ai o desenho realmente sumiu.
    desenhar_home(d, a);
  }
  d.endWrite();

  s_area_limpa = false;
  // SO LIMPA s_sujo_layout QUANDO FOI ESTA PASSADA QUE O ATENDEU
  // (limpar_area==true, ou seja: o loop principal chamou aqui exatamente
  // porque s_sujo_layout estava true). Esta linha zerava
  // INCONDICIONALMENTE - e s_app->desenhar() (chamada acima, dentro da
  // passada INCREMENTAL tambem, limpar_area==false) pode legitimamente
  // pedir um repaint completo NO MEIO da propria passada de dados (ex.:
  // app Camera chamando sistema_travar_navegacao(false) de dentro do
  // desenhar() dela, ao notar que uma captura terminou). Zerar aqui sem
  // condicao apagava esse pedido antes que o loop principal chegasse a ve-lo
  // - a barra de navegacao nunca era redesenhada destravada, e o INICIO
  // ficava escuro para sempre (defeito observado, "captura concluida" aparecia
  // no log e o botao continuava escuro do mesmo jeito).
  //
  // NAO E SO DA CAMERA: qualquer app que chame sistema_pedir_redesenho_completo()/
  // sistema_travar_navegacao()/sistema_zerar_rolagem() de dentro do proprio
  // desenhar() cairia na mesma armadilha - o conserto e aqui, nao em cada app.
  if (limpar_area) { s_sujo_layout = false; }
  s_sujo_dados  = false;
  s_ultimo_desenho = tab5_agora_ms();

  // A MARCA MORRE COM A PASSADA. Ela afirma "este retangulo esta desta cor
  // AGORA"; depois que o app desenhou por cima, deixa de ser verdade. Deixa-la
  // viva ate a proxima passada faria a limpeza seguinte ser dispensada sobre
  // conteudo real - e o app ficaria com rastro por uma otimizacao do sistema.
  tab5_tela_esquecer_limpo();
}

// Um quadro da tela de bloqueio durante a rampa de brilho do despertar.
static void animar_na_rampa(void)
{
  if (!s_bloqueado) { return; }
  desenhar_conteudo(false, false);
  comp_apresentar();
}

// ---------------------------------------------------------------------------
// Ciclo de vida dos apps
// ---------------------------------------------------------------------------
// Declarada aqui e definida mais abaixo (ver o bloco "MONTAGEM (E DESMONTAGEM)
// DE SD EM TAREFA PROPRIA"): fechar_app() precisa dela antes de abrir_app()
// ser lido, e a definicao mora perto da irma vTaskMontarSDApp por clareza.
static void vTaskDesmontarSDApp(void*);

// NAO 'static' - declarada em kernel/internal.hpp, chamada por
// kernel/shell.cpp (tratar_toque, ao fechar o app pelo botao INICIO).
void fechar_app(void)
{
  if (!s_app) { return; }
  ESP_LOGI(TAG, "fechando '%s'", s_app->nome);

  // RECUSADO: o app nunca abriu (nem ao_abrir, nem perfil, nem cartao) - so
  // a tela de motivo do sistema sai.
  if (s_app == s_app_recusado) {
    s_app_recusado = nullptr;
    s_app = nullptr;
    rolagem_zerar(&s_rol);
    s_sujo_layout = true;
    return;
  }

  const bool devolver_sd = s_app_montou_sd;

  // O TRABALHO DO APP TERMINA ANTES DO ao_fechar(): a queue e descartada e o
  // que estiver executando e esperado - so entao o app pode liberar os
  // buffers em que o worker escreve (kernel/work.hpp).
  // FECHAR E TELA PARADA: tudo daqui ate o fim roda na tarefa de interface.
  // Passou de 150 ms, o log diz quem segurou - o trabalho ou o ao_fechar().
  const int64_t t0 = esp_timer_get_time();
  trabalho_encerrar_do_app();
  const int64_t t1 = esp_timer_get_time();
  if (s_app->ao_fechar) { s_app->ao_fechar(); }
  tela_virtual_soltar_superficie();   // a camera ja parou no ao_fechar()
  const int64_t t2 = esp_timer_get_time();
  if (t2 - t0 > 150000) {
    ESP_LOGW(TAG, "fechar '%s' segurou a interface %lld ms (trabalho %lld, ao_fechar %lld)",
             s_app->nome, (t2 - t0) / 1000, (t1 - t0) / 1000, (t2 - t1) / 1000);
  }
  s_nav_travada     = false;  // rede de seguranca: app esquecido nao prende ninguem
  sistema_manter_acordado(SISTEMA_ACORDADO_APP, false);   // idem: o pedido era do app
  s_rolagem_travada = false;  // idem, para o proximo app nao nascer sem rolar
  rolagem_zerar(&s_rol);
  s_app = nullptr;

  // A home e sempre em pe. Restaurar aqui, e nao no proximo abrir, para que a
  // tela inicial nunca apareca deitada por um frame. Vale tambem para o app
  // que seguia o IMU e abriu em pe mas fechou deitado.
  orientacao_ao_fechar();

  // Desmonta DEPOIS do ao_fechar(): o app pode querer fechar arquivos ali.
  // So devolve se ESTE app tiver de fato tomado.
  //
  // EM TAREFA DE FUNDO, fire-and-forget: nada daqui em diante espera a
  // desmontagem terminar, e o mutex + contagem de referencia dentro de
  // tab5_sd.cpp (ver trava()/destrava()/s_refs) ja protegem contra a corrida
  // obvia - abrir outro app com SD antes desta tarefa rodar so incrementa a
  // referencia sobre o cartao que ainda esta montado, em vez de montar de novo.
  if (devolver_sd) {
    pool_criar(nullptr, vTaskDesmontarSDApp, nullptr, "sd_desmonta", 4096, 1);
    s_app_montou_sd = false;
  }
  registro_app_atual(nullptr);

  tab5_power_aplicar(perfil_da_home());
  s_sujo_layout = true;
}

// ---------------------------------------------------------------------------
// MONTAGEM (E DESMONTAGEM) DE SD EM TAREFA PROPRIA
// ---------------------------------------------------------------------------
// Tab5_sd_montar()/desmontar() rodavam DIRETO na tarefa de
// interface, dentro de abrir_app()/fechar_app() - a MESMA tarefa que chama
// M5.update() e processa toque. montar_de_verdade() (tab5_sd.cpp) liga o LDO
// da IO do cartao, negocia o host SDMMC e monta o FAT: nada catastrofico como
// os ~10s do Grove, mas a mesma classe de erro - hardware de verdade demorando
// dezenas a centenas de ms dentro da tarefa que precisa continuar lendo o
// dedo. Um cartao lento para responder bastava para o mesmo sintoma do Grove
// aparecer em escala menor.
//
// A DESMONTAGEM e simples: fecha o app, some da tela, ninguem mais espera por
// ela. Vai para uma tarefa solta (fire-and-forget) sem coordenacao nenhuma - o
// mutex e a contagem de referencia dentro de tab5_sd.cpp (ver trava()/
// destrava()/s_refs) ja garantem que montar() e desmontar() concorrentes de
// tarefas diferentes nao se atropelam, entao nao ha corrida a resolver aqui.
//
// A MONTAGEM e mais delicada: ao_abrir() de apps como Arquivos e Mesh presume
// que o cartao ja esta pronto (listam pasta, leem historico) na primeira
// chamada. Nao da para simplesmente disparar e seguir chamando ao_abrir()
// como se nada tivesse mudado - a abertura do app precisa esperar o resultado
// da montagem antes de chamar ao_abrir(). Por isso abrir_app() trava a tela no
// app (s_app = app, tela mostra "abrindo...") mas SO libera o app de verdade -
// chamando ao_abrir() - quando a tarefa de fundo termina. Ver
// concluir_abertura_do_sd() e a checagem no loop da interface.
static void vTaskMontarSDApp(void*)
{
  s_sd_montou_ok = tab5_sd_montar();
  s_sd_montando  = false;   // POR ULTIMO - so agora o loop pode concluir a abertura
}

static void vTaskDesmontarSDApp(void*)
{
  tab5_sd_desmontar();
}

// ---------------------------------------------------------------------------
// APP RECUSADO PELO SISTEMA
// ---------------------------------------------------------------------------
// Um app que EXIGE o cartao (app_t.exige_sd: Arquivos, Camera) nao abre com
// ele emprestado ao computador (modo pendrive) - e a regra e do sistema, nao
// de cada app. No lugar do app aparece uma tela do sistema dizendo por que;
// INICIO volta normalmente. Se o cartao voltar com ela na tela, o app abre
// sozinho (loop da interface).
//
// s_app aponta para o app (a barra mostra o nome, INICIO funciona), mas
// nenhuma funcao dele e chamada - app_vivo() e falso.
static void recusar_app(const app_t* app)
{
  ESP_LOGW(TAG, "'%s' nao abre: o cartao esta emprestado ao computador", app->nome);
  s_app = app;
  s_app_recusado = app;
  rolagem_zerar(&s_rol);
  orientacao_ao_fechar();                 // a tela de motivo e em pe
  tab5_power_aplicar(perfil_da_home());   // nada do perfil do app fica ligado
  s_sujo_layout = true;
}

// Segunda metade de abrir_app() quando o app usa SD: chamada pelo loop da
// interface assim que a montagem de fundo termina (ok ou nao). Espelha
// exatamente o que abrir_app() fazia embutido, so que depois da espera.
static void concluir_abertura_do_sd(void)
{
  const app_t* app = s_app_carregando;
  s_app_carregando = nullptr;

  s_app_montou_sd = s_sd_montou_ok;
  // CORRIDA COM O PENDRIVE: o cartao foi emprestado entre a checagem do
  // abrir_app() e a montagem. Mesma regra: quem exige o cartao nao abre.
  if (!s_app_montou_sd && app->exige_sd && tab5_sd_emprestado()) {
    recusar_app(app);
    return;
  }
  if (!s_app_montou_sd) {
    ESP_LOGW(TAG, "'%s' pediu cartao SD e ele NAO montou - abrindo assim mesmo, "
                  "mas o app nao vai conseguir gravar", app->nome);
  }

  if (app->ao_abrir && !app->ao_abrir()) {
    ESP_LOGE(TAG, "'%s' recusou abrir", app->nome);
    if (s_app_montou_sd) {
      pool_criar(nullptr, vTaskDesmontarSDApp, nullptr, "sd_desmonta", 4096, 1);
      s_app_montou_sd = false;
    }
    s_app = nullptr;      // volta para a home: a abertura falhou de verdade
    tab5_power_aplicar(perfil_da_home());
    s_sujo_layout = true;
    return;
  }

  registro_app_atual(app->nome);
  s_sujo_layout = true;   // troca a tela de "abrindo..." pelo conteudo de verdade
}

// NAO 'static' - declarada em kernel/internal.hpp, chamada por
// kernel/shell.cpp (tratar_toque, ao tocar um icone da home).
bool abrir_app(const app_t* app)
{
  if (s_app) { fechar_app(); }
  ESP_LOGI(TAG, "abrindo '%s'", app->nome);
  // A home sai da tela: o papel de parede pronto dela (PSRAM) vai junto.
  bq_parede_soltar();

  // O CARTAO ESTA NO COMPUTADOR e este app nao funciona sem ele: o sistema
  // nao abre (ver recusar_app). Antes de qualquer outra coisa - nem perfil,
  // nem orientacao, nem montagem.
  if (app->exige_sd && tab5_sd_emprestado()) {
    recusar_app(app);
    return true;
  }

  // A ordem importa: liga o que o app precisa ANTES de ele iniciar.
  //
  // E o BRILHO NAO VEM DO APP. O perfil do app diz que hardware ele precisa;
  // quao clara a tela fica e preferencia de quem segura o aparelho. Copiamos o
  // perfil e sobrescrevemos - assim o app_t continua declarando o campo (o
  // tab5_power e autocontido e usa ele), mas quem manda e o ajuste.
  // O MESMO VALE PARA O RADIO, e esta linha nasceu de um bug caro.
  //
  // Todo app declara radio=false (o padrao), entao abrir QUALQUER app cortava o
  // WLAN_PWR_EN e matava o ESP32-C6 com o WiFi rodando. O log nao
  // deixa duvida:
  //
  //   sistema: abrindo 'Ajustes'
  //   tab5pwr: trilho do C6 (WLAN_PWR_EN) = 0
  //   sdmmc_send_cmd returned 0x108        <- 1ms depois
  //   eh_sdio: Unrecoverable host sdio state  -> reinicio da placa
  //
  // A regra geral: o perfil do app diz o que ELE precisa ligado, nunca o que os
  // outros podem desligar. Recurso de SERVICO do sistema - brilho, radio - e
  // decidido aqui, e o app so pode SOMAR, nunca subtrair.
  // Os DOIS caminhos precisam da regra, e por isso ela mora tambem em
  // perfil_da_home(): fechar um app tambem reaplica perfil, e um WiFi que
  // sobrevive a abrir o app Ajustes mas morre ao fechar seria pior que o bug antigo -
  // falharia so as vezes.
  // Orientacao ANTES do perfil e de qualquer desenho: girar muda largura e
  // altura, e tudo que for calculado a partir delas precisa ver o valor novo.
  // Quais rotacoes existem e por que: kernel/orientation.cpp.
  orientacao_ao_abrir(app);

  tab5_perfil_t perfil = app->perfil;
  sistema_impor_preferencias(perfil);
  tab5_power_aplicar(perfil);

  // Cartao montado antes do ao_abrir(), pelo mesmo motivo de sempre: o app
  // pode querer ler configuracao ou abrir arquivo de log logo na abertura.
  //
  // A MONTAGEM EM SI NAO RODA MAIS AQUI (ver o bloco grande acima,
  // "MONTAGEM (E DESMONTAGEM) DE SD EM TAREFA PROPRIA"). Travamos a tela
  // neste app agora - por isso s_app ja e atribuido nesta funcao, antes de
  // ao_abrir() ter sido chamado - e devolvemos true: a home some, a barra de
  // navegacao mostra "INICIO" (a checagem de app aberto so olha s_app) e
  // desenhar_conteudo() mostra "abrindo..." enquanto s_app_carregando apontar
  // para este app. O toque fica engolido nesse meio-tempo (ver tratar_toque,
  // kernel/shell.cpp), por poucos frames na pratica - a mesma folga que o
  // cartao levava para montar, so que agora fora da tarefa que le o dedo.
  //
  // Falhar em montar NAO impede o app de abrir - ver concluir_abertura_do_sd().
  // Sem cartao, o Azimutal ainda desenha o mapa; ele so nao grava.
  s_app_montou_sd = false;
  if (app->usa_sd) {
    s_app_carregando = app;
    s_sd_montando    = true;
    pool_criar(nullptr, vTaskMontarSDApp, nullptr, "sd_monta", 4096, 1);

    s_app = app;
    rolagem_zerar(&s_rol);
    s_sujo_layout = true;
    return true;   // a abertura conclui em concluir_abertura_do_sd(), no loop
  }

  if (app->ao_abrir && !app->ao_abrir()) {
    ESP_LOGE(TAG, "'%s' recusou abrir", app->nome);
    tab5_power_aplicar(perfil_da_home());
    return false;
  }

  s_app = app;
  rolagem_zerar(&s_rol);   // todo app comeca no topo
  registro_app_atual(app->nome);
  s_sujo_layout = true;
  return true;
}

// ---------------------------------------------------------------------------
// SONDAGEM DO GROVE, EM TAREFA PROPRIA
// ---------------------------------------------------------------------------
// Parar os servicos e sondar de novo rodava DIRETO na tarefa
// de interface - a MESMA que chama M5.update() e processa toque. O preco:
// tab5_mesh_parar()/tab5_gps_parar() esperam ate 1s cada pela tarefa do
// servico confirmar saida, e tab5_grove_detectar() bloqueia mais uns 4-8s
// sondando os dois protocolos. Toda vez que alguem pluga um modulo no
// Grove, isso e quase 10 SEGUNDOS de toque surdo - observado em
// usuario plugou o GPS, o toque parou de responder, e so voltou
// bem depois (o que coincidiu com o repouso ligando, mas a causa real era
// so a sondagem ainda nao ter terminado).
//
// A correcao e tirar esse trabalho da tarefa de interface. Esta tarefa nova
// roda em paralelo - PRIORIDADE 1, a mais baixa do sistema, porque e
// trabalho de fundo que nao pode disputar CPU nem com a interface (4) nem
// com os servicos (3). Enquanto ela para servico e sonda, a tarefa de
// interface continua livre, e o toque nunca mais fica refem disto.
//
// IDEMPOTENTE POR CONSTRUCAO: tab5_mesh_parar()/tab5_gps_parar() ja
// devolvem true sem fazer nada quando o servico correspondente ja esta
// parado - por isso os DOIS pontos do loop que precisam de uma sondagem
// (a re-sondagem pedida e as tentativas extras quando a primeira nao achou
// nada) podem chamar a MESMA funcao, sem precisar saber se algo estava
// mesmo de pe.
// true so quando o disparo e uma RE-sondagem de verdade (modulo trocado no
// cabo) - e nao uma das tentativas automaticas de achar algo num Grove que
// continua vazio. So esse caso reabre a contagem de tentativas; senao a
// tentativa automatica reiniciaria a propria contagem toda vez que
// encontrasse o cabo ainda vazio, e as "duas tentativas e para" nunca
// parariam de verdade.
static bool s_prox_sondagem_reinicia_contagem = false;

static void vTaskSondagemGrove(void*)
{
  const bool reiniciar = s_prox_sondagem_reinicia_contagem;
  const bool mesh_ok = tab5_mesh_soltar_grove();   // via Bluetooth o no nao esta no cabo
  const bool gps_ok  = tab5_gps_parar();
  if (!mesh_ok || !gps_ok) {
    ESP_LOGE(TAG, "nao consegui liberar o Grove - sondagem cancelada");
  } else {
    if (reiniciar) {
      s_sondas_extra = 0;          // reabre as tentativas automaticas
      s_grove_desde  = tab5_agora_ms(); // ... a partir de AGORA, nao do boot
    }
    if (ajustes_grove()) {
      const grove_tipo_t achado = tab5_grove_detectar();
      if (achado == GROVE_MESHTASTIC) {
        // O GROVE TEM PRIORIDADE. Achar o C6L no cabo leva o Mesh para ele,
        // de qualquer via (Bluetooth ou internet). A troca acontece SO aqui,
        // no achado - boot, cabo plugado, re-sondagem -, e nao a cada volta:
        // depois dela a escolha manual em Ajustes vale ate o proximo achado.
        // tab5_mesh_iniciar() para a via anterior antes de subir a serial.
        if (ajustes_mesh_via() != AJUSTES_MESH_SERIAL) {
          ESP_LOGI(TAG, "Meshtastic no Grove: o cabo tem prioridade - Mesh vai para ele");
          ajustes_definir_mesh_via(AJUSTES_MESH_SERIAL);
        }
        tab5_mesh_iniciar();
      }
      else if (achado == GROVE_GPS)        { tab5_gps_iniciar(); }
    } else {
      ESP_LOGI(TAG, "Grove desligado no ajuste - parei o servico e nao sondei");
    }
  }
  // Zera a vigia: a sondagem acabou de estabelecer a verdade, e a leitura
  // seguinte do pino tem que ser comparada com ELA, nao com o que se
  // acreditava antes.
  s_grove_havia    = (tab5_grove_tipo() != GROVE_NADA);
  s_grove_iguais   = 0;
  s_sujo_layout    = true;
  s_sondando_em_fundo = false;   // POR ULTIMO - so agora a proxima pode comecar
}

// Dispara a sondagem em segundo plano, se ja nao houver uma correndo - dois
// disparos ao mesmo tempo sondariam o mesmo cabo duas vezes por nada.
// 'reiniciar_contagem' ver o aviso em s_prox_sondagem_reinicia_contagem.
static void iniciar_sondagem_em_fundo(bool reiniciar_contagem)
{
  if (s_sondando_em_fundo) { return; }
  s_sondando_em_fundo = true;
  s_prox_sondagem_reinicia_contagem = reiniciar_contagem;
  pool_criar(nullptr, vTaskSondagemGrove, nullptr, "grove_sonda", 4096, 1);
}

// ---------------------------------------------------------------------------
// Tarefa de interface
// ---------------------------------------------------------------------------
static void vTaskInterface(void *pvParameters)
{
  (void)pvParameters;
  s_ultimo_toque = tab5_agora_ms();

  // NO WATCHDOG DE VERDADE, e nao so o da tarefa ociosa.
  //
  // O watchdog do sistema (ver sdkconfig, ESP_TASK_WDT) por padrao so
  // acompanha as duas tarefas IDLE - ele pega uma tarefa que trava o CORE
  // inteiro (nunca devolve CPU, nem para o ocioso rodar), mas NAO pega um
  // DEADLOCK de verdade: esta tarefa bloqueada para sempre num mutex ou
  // semaphore que ninguem mais vai devolver, enquanto o resto do sistema
  // continua rodando normalmente por fora. Sem se inscrever, um travamento
  // assim deixaria a tela congelada para sempre, sem NENHUM sinal disso no
  // log - o aparelho pareceria "vivo" (CPU ociosa disponivel) e estaria
  // surdo e mudo por dentro.
  //
  // O maior periodo que este loop espera e ~1 s (o prazo adaptativo do
  // repouso e da tela parada, ver o fim do loop) - bem dentro dos 5s de prazo
  // do watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S). Se um dia o loop nao voltar aqui dentro desse prazo, o
  // watchdog reinicia o aparelho - pior que congelado, melhor que travado
  // para sempre e invisivel.
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    ESP_LOGW(TAG, "nao consegui inscrever a interface no watchdog");
  }

  // O WiFi so sobe aqui, e nao no app_main, por uma razao concreta: ligar o
  // trilho do C6 e uma escrita no expansor I2C, e esta tarefa e a dona do I2C.
  // Alem disso, subindo depois da interface, a tela ja esta desenhada quando o
  // radio comeca a negociar - o usuario ve "conectando" em vez de esperar.
  if (ajustes_wifi()) { tab5_wifi_ligar(); }

  // BLUETOOTH. Mesma razao do WiFi para morar aqui: ligar pede o
  // trilho do C6 (I2C). So o botao do usuario liga (ver sistema_bluetooth);
  // com o Mesh no Bluetooth o servico dele sobe junto - ele nao depende do
  // Grove, entao nao espera sondagem nenhuma.
  if (ajustes_bluetooth()) { bt_aplicar(true); }

  // Teclado do ExtPort1. Aqui, e nao no app_main, porque e I2C - e esta tarefa
  // e a dona do barramento. Sem teclado encaixado ele so avisa e segue.
  //
  // A VONTADE DO USUARIO E EMPURRADA, nunca lida por dentro do HAL - por isso
  // o habilitar() vem ANTES do iniciar(). Ver o aviso em tab5_keyboard.cpp.
  tab5_teclado_habilitar(ajustes_teclado());
  tab5_teclado_iniciar();

  // Sensor de temperatura do P4. Nao e I2C (e um ADC interno), mas nasce aqui
  // junto dos outros para o boot ter um lugar so onde os sensores sobem.
  tab5_sensores_iniciar();

  // PRIORIDADE DA TELA NO BARRAMENTO. Antes de qualquer app desenhar: e a
  // arbitragem que decide se a piscada azul pode ou nao acontecer, e ela nao
  // depende de nada que os apps facam.
  tab5_barramento_priorizar_tela(true);

  // A TELA VIRTUAL (COMPOSITOR.md, etapa 3a). Antes do primeiro desenho: a
  // partir daqui, home, barras e apps desenham nela, e so o compositor
  // escreve nas paginas do painel.
  tela_virtual_iniciar();

  // O WORKER DO SISTEMA (kernel/work.hpp): o que for lento sai
  // desta tarefa.
  trabalho_iniciar();

  // RELOGIO DO PAINEL. A outra metade da mesma briga: a prioridade decide quem
  // ganha a disputa, isto REDUZ a disputa. O M5GFX programa 80MHz para este
  // painel - 64 frames por segundo e 118,5 MB/s lidos da PSRAM sem parar. A
  // 60MHz sao 48 frames e 88,9 MB/s: um quarto a menos de demanda permanente,
  // e nenhum app precisa saber que isso existe.
  //
  // Aqui, antes do primeiro desenho: a troca desmonta e remonta o painel, e
  // fazer isso com a tela ja em uso apagaria o conteudo por um instante.
  if (tab5_painel_toque_gt911()) { tab5_power_relogio_dpi((int)ajustes_relogio_painel()); }   // TDDI: fica o de fabrica

#ifdef TAB5_MOSTRAR_SPLASH
  // SO AGORA: o painel ja tem o relogio final e nao vai mais ser desmontado
  // neste boot - ver o aviso grande junto de desenhar_splash() (shell.cpp).
  desenhar_splash();
#endif

  // Quem esta no Grove? So vale sondar se o trilho estiver ligado - sem energia
  // ninguem se apresenta. E so o Meshtastic vira SERVICO permanente: o GPS
  // continua sendo aberto e fechado pelo app Azimutal, porque ele nao precisa
  // existir com o app fechado (ninguem espera notificacao de satelite).
  // SONDAGEM ADIADA PARA O LOOP, e nao feita aqui.
  //
  // Ela bloqueia ~3 segundos (liga o trilho, espera o modulo bootar, escuta os
  // dois baud). Feita AQUI, antes do primeiro desenho, o aparelho ficava com a
  // tela apagada durante toda a sondagem - parecia travado no boot.
  //
  // Marcando o pedido, o loop entra, desenha a home e SO ENTAO sonda. A pessoa
  // ve o sistema de pe em alguns frames; o Grove chega alguns segundos depois,
  // que e quando ele tem de chegar - nenhum app depende dele para abrir.
  //
  // Reaproveita exatamente o caminho da troca de modulo: um lugar so que sabe
  // parar servico, sondar e subir o que couber.
  if (ajustes_grove()) {
    s_grove_resondar = true;
  } else {
    ESP_LOGI(TAG, "Grove sem energia (ajuste desligado) - nao vou sondar");
  }

  for (;;) {
    // MEDIDA (kernel/compositor.hpp, comp_estat): a volta comeca aqui, no
    // acordar. Se ela terminar em quadro, o custo inteiro vai ao histograma.
    const int64_t volta_t0 = esp_timer_get_time();
    ETAPA("toque (M5.update, I2C)");
    // Navegacao travada = teclado (ou captura) lendo o toque por nivel.
    ajustar_leitura_do_toque(s_nav_travada && (tab5_agora_ms() - s_ultimo_toque) < 500);
    M5.update();
    const uint32_t t = tab5_agora_ms();

    atender_pedido_de_relogio();

    // PAINEL NOVO (boot, troca de relogio): no modo direto as paginas sao a
    // UNICA copia da tela - painel novo nasce sem nada nosso, e o unico jeito
    // de ter a tela de volta e redesenha-la inteira. (O acordar do repouso
    // trata o seu caso la embaixo, antes de acender a luz.)
    if (!tab5_power_em_repouso() && comp_painel_novo()) { s_sujo_layout = true; }

    // As partes de fundo (kernel/background.h): quem terminou sozinho e limpo, a
    // parada pedida pelo console acontece, e o perfil efetivo e reaplicado
    // quando o conjunto mudou. Acordado ou em repouso - o fundo nao dorme.
    ETAPA("partes de fundo");
    fundo_manter();
    ETAPA("vigia do Grove / abertura com SD");

    // ABERTURA DE APP COM SD PENDENTE: a tarefa de fundo (vTaskMontarSDApp)
    // terminou desde o ultimo ciclo? Conclui agora, com a tela ja mostrando
    // "abrindo..." ha alguns frames - ver abrir_app() e concluir_abertura_do_sd().
    if (s_app_carregando && !s_sd_montando) {
      concluir_abertura_do_sd();
    }
    // O CARTAO VOLTOU com a tela de recusa aberta: abre o app de verdade.
    if (s_app_recusado && !tab5_sd_emprestado() && !s_bloqueado) {
      const app_t* ap = s_app_recusado;
      fechar_app();
      abrir_app(ap);
    }

    // ---------------------------------------------------------------------
    // VIGIA DO GROVE: perceber a troca de modulo sem pagar sondagem em loop.
    // ---------------------------------------------------------------------
    //
    // A sondagem custa 3s de tarefa bloqueada. Rodar isso periodicamente esta
    // fora de questao, e foi por nao ter alternativa que a deteccao virou
    // decisao de BOOT - feita uma vez e nunca mais revista. Tirar o GPS e por o
    // C6L nao mudava nada porque ninguem estava olhando.
    //
    // A saida e separar as duas perguntas. "Quem esta ai?" e cara e so se
    // pergunta quando vale. "Tem alguem ai?" e uma leitura de GPIO (ver o
    // tab5_grove_presenca) e pode rodar a 0,5Hz sem doer.
    //
    // Com a porta OCUPADA nao da para ler o pino - ele e do driver da UART. Aí
    // o informante e outro: o proprio servico, que sabe ha quanto tempo o radio
    // nao fala. Cada estado tem quem o observe.
    if (ajustes_grove() && !s_grove_resondar && (t - s_grove_olhado) >= VIGIA_MS) {
      s_grove_olhado = t;
      const grove_dono_t dono = tab5_grove_dono();

      if (dono == GROVE_DONO_MESH) {
        // O radio calou. 60s e folga larga de proposito: o servico pede
        // apresentacao a cada 10s enquanto esta mudo, entao um C6L vivo
        // responde muito antes. E um alarme falso se conserta sozinho - a
        // sondagem seguinte o encontra de novo - enquanto um alarme cedo
        // demais derrubaria o servico de um radio so ocupado.
        // PRAZO PROPRIO, e nao o do no_ar().
        //
        // Apoiada no no_ar(), esta vigia herdava o SILENCIO_MS do servico - que
        // e generoso de proposito para nao chamar de morta uma malha quieta.
        // Quando ele subiu de 30s para 90s, os dois prazos se SOMARAM e a troca
        // de modulo no cabo passou a levar 150s para ser notada. Perguntas
        // diferentes, prazos diferentes: aqui o que importa e quanto tempo faz
        // que o radio nao fala, cru.
        if (tab5_mesh_silencio_ms() > MUDO_MS) {
          ESP_LOGW(TAG, "radio mudo ha %us - devolvendo o Grove e sondando",
                   (unsigned)(MUDO_MS / 1000));
          s_grove_resondar = true;   // o bloco abaixo para o servico e sonda
        }
      }
      else if (dono == GROVE_DONO_GPS) {
        // MESMA IDEIA do bloco do Mesh acima, prazo proprio (ver GPS_MUDO_MS):
        // o GPS fala sozinho e continuo, entao silencio aqui significa cabo
        // puxado com muito mais confianca do que significaria para o radio.
        if (tab5_gps_silencio_ms() > GPS_MUDO_MS) {
          ESP_LOGW(TAG, "GPS mudo ha %us - devolvendo o Grove e sondando",
                   (unsigned)(GPS_MUDO_MS / 1000));
          s_grove_resondar = true;
        }
      }
      else if (dono == GROVE_DONO_NINGUEM) {
        const bool ha = tab5_grove_presenca();

        // Duas leituras iguais antes de acreditar. Um cabo sendo encaixado
        // toca antes de assentar, e agir no primeiro contato dispararia
        // sondagem no meio do movimento da mao.
        if (ha == s_grove_havia) { if (s_grove_iguais < 3) { ++s_grove_iguais; } }
        else                     { s_grove_havia = ha; s_grove_iguais = 1; }

        if (s_grove_iguais == 2) {
          const bool tinha = (tab5_grove_tipo() != GROVE_NADA);
          if (ha && !tinha) {
            ESP_LOGI(TAG, "algo foi plugado no Grove - sondando");
            s_grove_resondar = true;
          } else if (!ha && tinha) {
            // Nao se paga sondagem para concluir "nada": o pino ja disse. E
            // sem isto o tipo velho ficaria no ar, que e o bug de origem.
            ESP_LOGI(TAG, "o Grove esvaziou");
            // So esquecer o tipo. Este ramo so roda com o dono = NINGUEM:
            // nem o Mesh (serial) nem o GPS estao com a UART, entao nao ha
            // servico a parar - e parar servico (ate 1 s esperando a tarefa
            // dele) e justamente o que a regra do core tira da interface (ver
            // vTaskSondagemGrove). Chamar aqui era inofensivo so por acaso.
            tab5_grove_esquecer();
            s_sujo_layout = true;
          }
        }
      }
    }

    // RE-SONDAGEM DO GROVE, pedida por quem trocou o modulo do cabo.
    //
    // Feita EM TAREFA PROPRIA (ver vTaskSondagemGrove, logo
    // acima) - nao mais direto aqui. Chamar tab5_mesh_parar()/
    // tab5_gps_parar()/tab5_grove_detectar() nesta tarefa (a mesma que
    // processa toque) prendia o toque por ate ~10s toda vez que alguem
    // trocava o modulo do cabo - observado, usuario achou que o
    // toque tinha travado de vez.
    //
    // UM CICLO DE ESPERA antes de disparar, ainda assim: o ciclo anterior ja
    // desenhou o botao no estado novo e o app Ajustes ja pode escrever
    // "sondando..." antes da tarefa de fundo comecar a trabalhar de verdade.
    if (s_grove_resondar && s_grove_pedido_visto) {
      s_grove_resondar = false;
      s_grove_pedido_visto = false;
      iniciar_sondagem_em_fundo(true);   // troca de modulo - reabre as tentativas
    }


    // NOVA SONDAGEM DO GROVE, quando a primeira nao achou nada.
    //
    // A sondagem do boot acontece uma vez e decide a sessao inteira. Isso a
    // torna refem do INSTANTE: o C6L e um ESP32-C6 rodando o Meshtastic
    // completo, e ele leva alguns segundos para responder. Boot do Tab5 mais
    // rapido que o boot dele - o que acontece depois de tirar o cabo, por
    // exemplo - e o servico simplesmente nao subia mais ate alguem reiniciar.
    //
    // Duas tentativas extras, aos 20s e aos 50s, e so enquanto NADA DE
    // CONCLUSIVO foi encontrado - tambem em tarefa de fundo (ver
    // vTaskSondagemGrove), pelo mesmo motivo do bloco acima: nao prender o
    // toque.
    //
    // GROVE_DESCONHECIDO ENTRA AQUI TAMBEM, e a condicao so tinha GROVE_NADA
    // achado ao vivo, com log: o boot ligou o 5V, esperou os
    // 4s do C6L subir, a sonda a 115200 nao viu nada e a de 9600 viu 2 bytes
    // de ruido (nem NMEA nem START1 do Meshtastic) - "desconhecido", nao
    // "nada". Com a condicao antiga isso NUNCA tentava de novo sozinho: o
    // servico so subia depois de arrancar e reencaixar o cabo na mao. Um C6L
    // que ainda esta terminando de bootar (ou um instante ruim de energia,
    // ver o BOD que apareceu no mesmo log) pode muito bem devolver ruido em
    // vez de silencio OU de uma resposta valida - e merece a mesma segunda
    // chance que "nada" ja tinha.
    //
    // Nunca sonda com a porta ocupada: o dono (Mesh ou GPS, os dois servicos
    // que hoje disputam o Grove) esta usando a UART, e uma sondagem por cima
    // dela quebraria a leitura.
    if (s_sondas_extra < 2 && ajustes_grove()
     && (tab5_grove_tipo() == GROVE_NADA || tab5_grove_tipo() == GROVE_DESCONHECIDO)
     && tab5_grove_dono() == GROVE_DONO_NINGUEM
     && t > s_grove_desde + (uint32_t)(20000 + s_sondas_extra * 30000)) {
      ++s_sondas_extra;
      ESP_LOGI(TAG, "Grove inconclusivo na primeira sonda (%s) - tentando de novo (%d/2)",
               tab5_grove_tipo() == GROVE_NADA ? "nada" : "desconhecido",
               s_sondas_extra);
      iniciar_sondagem_em_fundo(false);   // tentativa automatica - NAO reabre a contagem
    }

    // Uma leitura do INA226 por segundo, ACORDADO OU EM REPOUSO. Esta tarefa e
    // a unica que pode ir ao I2C, entao e ela quem alimenta o cache que o
    // registro de energia consome. Sem isto, o registro so teria dado enquanto
    // a barra de status estivesse sendo desenhada - ou seja, nada em repouso.
    if ((t - s_ultima_medida) >= 1000) {
      s_ultima_medida = t;
      ETAPA("medidas (I2C: energia, sensores, IMU, hora)");
      tab5_power_medir();
      // Mesmo tique: os dois alimentam a mesma barra, e uma cadencia so evita
      // que ela mude por metades em instantes diferentes.
      tab5_sensores_medir();
      // IMU, 1x/s - so para o CACHE que outros apps (nao o app_imu, que le
      // direto e mais rapido - ver o aviso grande em tab5_imu.hpp sobre por
      // que as cadencias sao diferentes de proposito) podem consultar sem
      // tocar no barramento I2C interno.
      // EM REPOUSO, NAO: ninguem le este cache com a tela apagada (quem
      // segue o IMU - kernel/orientation - mede sozinho a cada 200 ms, e so com
      // app aberto). Era uma transacao I2C por segundo, todo segundo, para
      // nada; acordado, ela continua servindo o console.
      if (!tab5_power_em_repouso()) { tab5_imu_medir(); }
      // Hora: GPS/Mesh como fonte, RTC (I2C interno) como destino - mesma
      // regra de so a interface tocar o barramento. So escreve de fato a
      // cada ~10min (ver RESSINCRONIZAR_MS), a chamada 1x/s e so para
      // perceber uma fonte nova assim que ela aparecer.
      tab5_hora_sincronizar();
      vigiar_mesh_internet(t);
    }

    // Teclado fisico: a cada TECLADO_MS. Quando nao ha tecla e UMA transacao
    // I2C (so o contador de eventos), barato o bastante para o laco acordado.
    //
    // A TRADUCAO E DO DRIVER (hal/tab5_keyboard): ele conhece o teclado - o
    // modo HID, a tabela das 70 teclas, as teclas mortas dos acentos - e
    // entrega o evento pronto. Aqui so se entrega ao app. Ate 4 teclas por
    // volta: digitando rapido, mais de uma cabe no intervalo, e uma por volta
    // atrasaria o texto atras dos dedos.
    if (!tab5_power_em_repouso() && (t - s_ultimo_teclado) >= teclado_ms()) {
      s_ultimo_teclado = t;
      ETAPA("teclado (I2C)");
      const bool teclado_antes = tab5_teclado_presente();
      tab5_tecla_t ev;
      for (int k = 0; k < 4 && tab5_teclado_ler(&ev); ++k) {
        s_ultimo_toque = t;          // digitar conta como uso: nao apague a tela
        if (s_bloqueado) {
          // TELA BLOQUEADA: o teclado nao fala com o app que esta atras - so
          // o Enter, que desbloqueia (o par do deslize para cima). Antes desta
          // guarda o que se digitava no bloqueio ia parar na conversa.
          if (ev.codigo == TAB5_TECLA_ENTER) {
            ESP_LOGI(TAG, "desbloqueada pelo teclado (Enter)");
            desbloquear_tela();
            tab5_teclado_descartar();   // o resto da rajada nao vai para o app
            break;
          }
          continue;
        }
        if (s_painel_avisos) {
          // CENTRAL DE AVISOS ABERTA: o app esta escondido e nao ouve. Esc
          // fecha a central; o resto e engolido (nao vai digitar as cegas).
          if (ev.codigo == TAB5_TECLA_ESC) { painel_avisos_abrir(false); }
          continue;
        }
        if (app_vivo() && s_app->ao_teclar) {
          s_app->ao_teclar(ev.texto, ev.codigo, ev.mod);
          sistema_pedir_redesenho();
        } else {
          ESP_LOGI(TAG, "tecla: \"%s\" cod=%u%s%s%s (nenhum app escutando)",
                   ev.texto, (unsigned)ev.codigo,
                   (ev.mod & TAB5_TECLA_CTRL)  ? " +CTRL"  : "",
                   (ev.mod & TAB5_TECLA_SHIFT) ? " +SHIFT" : "",
                   (ev.mod & TAB5_TECLA_ALT)   ? " +ALT"   : "");
        }
      }
      // ENCAIXAR/TIRAR MUDA O LAYOUT mesmo sem tecla: o teclado virtual
      // aberto precisa sair quando aparece o fisico, e a dica da barra de
      // escrita muda nos dois sentidos. Se a tela ja estava deitada nao ha
      // giro para provocar esta passada por acaso.
      if (teclado_antes != tab5_teclado_presente()) { s_sujo_layout = true; }
    }

    // Em repouso, detecta o toque por NIVEL (ha dedo na tela?) e nao por borda.
    // O wasClicked() dispara na SOLTURA, e o loop em repouso roda a cada 200ms:
    // um toque rapido comeca e termina dentro do mesmo intervalo, o M5.update()
    // nunca ve a transicao, e o toque se perde. Era por isso que as vezes
    // precisava tocar duas ou tres vezes para acordar.
    ETAPA("toque: decidir");
    const bool tocando = (M5.Touch.getCount() > 0);
    if (tocando) { s_dedo_ms = t; }
    const auto toque = M5.Touch.getDetail();
    if (!tab5_power_em_repouso()) { ETAPA("toque: vigia"); vigiar_toque(t, tocando); lupa_toque(t, tocando); validar_isr_tddi(tocando); }

    // TECLA EM REPOUSO: a fila do teclado e consultada a cada volta do repouso
    // (o pulso do G50 acorda a volta na hora; sem ele, o prazo de ~1 s). Uma
    // transacao I2C por volta, e so com teclado encaixado.
    bool tecla_acorda = false;
    if (tab5_power_em_repouso() && tab5_teclado_presente()) {
      tecla_acorda = tab5_teclado_pendente();
    }

    // O pedido de acordar so vale para o repouso em que chegou: acordado, ele
    // e descartado - sobrando, acordaria o PROXIMO repouso logo ao entrar.
    const bool pedido_acordar = s_pedir_acordar;
    s_pedir_acordar = false;
    if (tab5_power_em_repouso()) {
      const bool sim_acorda = s_sim_x >= 0 || s_sim_abrir || s_sim_fechar || pedido_acordar;
      if (tocando || tecla_acorda || sim_acorda) {
        s_sim_x = -1;   // o pedido que acordou nao vira clique, como o dedo
        s_ultimo_toque = t;
        // O que acordou nao conta como uso: o dedo nao vira clique
        // (s_dedo_do_despertar), e a tecla nao vira letra (descartada).
        if (tocando) { s_dedo_do_despertar = true; }
        if (tecla_acorda) { tab5_teclado_descartar(); }
        ESP_LOGI(TAG, "acordando (%s): app=%s", tocando ? "toque" : "teclado", s_app ? s_app->nome : "home");
        // DESLIGA A ISR AQUI, antes de mais nada - ver o aviso grande junto
        // de s_toque_isr_ok. Acordado, um arraste pulsaria este pino a cada
        // 5-20ms e a queue nunca ficaria vazia se a interrupcao seguisse
        // ligada.
        s_isr_acorda = false;
        ETAPA("repouso: acordando");
        if (tab5_power_retomar()) {
          // RELIGA O QUE O APP SOLTOU EM ao_repouso() (ex.: a camera) - so
          // depois do painel ja estar de pe de novo, para nao reabrir um
          // consumidor de DMA antes do canal do painel estar reconstruido.
          // Ver o aviso grande em kernel/app.h junto de ao_repouso/ao_retomar.
          if (app_vivo() && s_app->ao_retomar) {
            s_app->ao_retomar();
          }
          // O retomar() devolve o painel ao ar com a LUZ AINDA APAGADA e as
          // paginas pretas - e no modo direto elas eram a unica copia da tela.
          // Redesenho COMPLETO direto na pagina de tras, e so depois da troca
          // CONFIRMADA a luz sobe: a primeira coisa que o olho ve ja e o
          // conteudo certo. comp_painel_novo() e consumido aqui para o topo
          // do loop nao pedir um segundo redesenho completo.
          const uint32_t t0 = tab5_agora_ms();
          bloqueio_ao_ligar_tela();
          comp_painel_novo();
          desenhar_barra_status(tela_virtual(), true);
          desenhar_conteudo(true, true);
          comp_apresentar();
          comp_esperar_troca();
          ESP_LOGI(TAG, "acordando: redesenho levou %lu ms",
                   (unsigned long)(tab5_agora_ms() - t0));

          // A rampa de brilho bloqueia esta tarefa; a animacao da tela de
          // bloqueio avanca um quadro a cada degrau para nao ficar congelada.
          if (s_bloqueado) { tab5_power_gancho_rampa(animar_na_rampa); }
          tab5_power_acender();
          tab5_power_gancho_rampa(nullptr);

          // Marcado AQUI, depois de tudo pronto. A carencia de tempo virou
          // rede secundaria (a bandeira acima e a principal), e agora ela conta
          // do momento em que a tela ficou utilizavel - nao de quando o
          // despertar comecou.
          s_acordou_em = tab5_agora_ms();

          // REDE DE SEGURANCA, AGORA SO QUANDO ELA FAZ FALTA.
          //
          // Ela nasceu para a tela esbranquicada: naquele desenho o painel era
          // RESSINCRONIZADO e o primeiro frame depois disso podia sair ruim,
          // entao um segundo redesenho 20ms depois consertava.
          //
          // Hoje o repouso DESMONTA o painel e o retomar cria um novo. Com
          // double buffer, o que foi para a tela e um frame inteiro pintado do
          // zero e publicado de uma vez - nao ha "primeiro frame duvidoso"
          // para corrigir. O que sobrou foi o CUSTO: um segundo redesenho
          // completo (na home sao os oito icones, 578KB) disparado na pagina
          // VISIVEL exatamente enquanto a luz sobe em rampa. A rajada mais mal
          // colocada que existia, bem no instante em que o olho esta olhando.
          //
          // Sem double buffer a historia e outra: ai o desenho foi feito na
          // propria tela, aos pedacos, e uma segunda passada continua valendo.
          // (A "rede de seguranca" do segundo redesenho morreu com o
          // compositor: o frame que acende e sempre inteiro.)
        } else {
          // retomar() ja loga o motivo. Aqui so registramos que a tela seguiu
          // apagada de proposito, para o log contar a historia inteira.
          ESP_LOGE(TAG, "acordando: retomar() falhou - tela segue apagada");
        }
      }

      // Beep de notificacao + desligar o I2S ocioso, TAMBEM em repouso -
      // achado, ver o aviso grande em
      // atualizar_beep_e_ociosidade_do_som() mais acima no arquivo. Mesmo
      // passo de 2s que a barra usa acordada (s_ultima_barra e
      // compartilhada entre os dois ramos: sao mutuamente exclusivos, nunca
      // correm no mesmo ciclo, entao nao ha disputa por ela).
      if ((t - s_ultima_barra) >= BARRA_MS) {
        s_ultima_barra = t;
        atualizar_beep_e_ociosidade_do_som(t);
      }
    } else {
      if (tocando) { s_ultimo_toque = t; }

      // Toque por NIVEL, para quem pediu: entregue a cada ciclo (20 ms), com o
      // dedo ainda na tela. E o que o wasClicked() nao da - ele so fala na
      // soltura, e um jogo nao pode esperar o dedo sair para virar a cobra.
      // A borda de subida fica a cargo do app; aqui so contamos o que ha agora.
      //
      // A carencia de 500 ms depois de acordar vale igual: o dedo que acendeu a
      // tela nao pode virar jogada.
      if (app_vivo() && s_app->ao_tocar_nivel
       && !sistema_app_encoberto() && !s_dedo_do_despertar && (t - s_acordou_em) > 500) {
        const area_t a = area_util();
        // MESMA CORRECAO E MESMA FAIXA DO CLIQUE (shell.cpp, tratar_toque):
        // TOQUE_DY aplicado aqui tambem, e com a navegacao travada a faixa de
        // baixo e do app - o ponto nela e entregue na ultima linha da area.
        const int nav_y0 = M5.Display.height() - ALTURA_NAV;
        auto y_do_app = [&](int y_cru, bool* vale) {
          int y = y_cru - TOQUE_DY;
          *vale = y >= ALTURA_BARRA && (y < nav_y0 || s_nav_travada);
          if (y >= nav_y0) { y = nav_y0 - 1; }
          return y;
        };
        bool dentro = false;
        const int ty = y_do_app(toque.y, &dentro);
        dentro = dentro && tocando;
        // SO OS DEDOS NA TELA AGORA. O getCount() do M5Unified conta o dedo
        // que ja subiu por mais DUAS amostras (estados touch_end e depois
        // none), e o getDetail(i) indexa o relatorio cru da ultima leitura -
        // alem do numero de pontos dele, as entradas sao velhas. Lido pelo
        // getCount, o teclado via a tecla "ainda apertada" ~40 ms depois de o
        // dedo sair: toda letra atrasava duas voltas, e o dedo seguinte que
        // descia nessa janela com o mesmo id (o GT911 reaproveita o menor
        // livre) parecia o mesmo dedo escorregando - a tecla anterior sumia.
        // Aqui: as 5 vagas, so as pressionadas, cada id uma vez.
        s_n_toques = 0;
        for (int i = 0; i < (int)m5::Touch_Class::TOUCH_MAX_POINTS && s_n_toques < SISTEMA_TOQUES_MAX; ++i) {
          const auto& dd = M5.Touch.getDetail(i);
          if (!dd.isPressed()) { continue; }
          bool repetido = false;
          for (int k = 0; k < s_n_toques; ++k) { if (s_toques[k].id == dd.id) { repetido = true; break; } }
          if (repetido) { continue; }
          bool vale = false;
          const int y = y_do_app(dd.y, &vale);
          if (!vale) { continue; }
          s_toques[s_n_toques].id = dd.id;
          s_toques[s_n_toques].x  = (int16_t)(dd.x - a.x);
          s_toques[s_n_toques].y  = (int16_t)(y - a.y + s_rol.atual);
          ++s_n_toques;
        }
        // ROLAGEM SOMADA AQUI TAMBEM. O ao_tocar() ja fazia isso e este nao
        // fazia - a assimetria nao aparecia porque nenhum app que rola usava
        // toque por nivel. Deixar assim seria plantar um bug para o primeiro
        // que usasse: os dois retornos de toque TEM que falar o mesmo idioma
        // de coordenadas, senao o app acerta num e erra no outro.
        s_app->ao_tocar_nivel(dentro ? toque.x - a.x : -1,
                              dentro ? ty - a.y + s_rol.atual : -1,
                              dentro);
      }

      // ARRASTE: acumula enquanto o dedo esta na area util. Toda a
      // matematica de arraste/afastamento/sinal mora agora em
      // kernel/scroll.hpp - ver o aviso grande la sobre por que isto
      // deixou de ser so deste arquivo.
      {
        const area_t ar = area_util();
        s_rol.max = limite_do_app(ar);
        const bool na_area = tocando
                          && toque.y >= ALTURA_BARRA
                          && toque.y <  (M5.Display.height() - ALTURA_NAV);
        // NAO marca s_sujo_layout aqui. Rolagem pura tem um caminho proprio,
        // mais barato (ver o bloco de repintura no loop principal): desloca
        // o pixel ja desenhado em vez de apagar a area inteira e redesenhar
        // tudo de novo a cada amostra de toque.
        rolagem_arrastar(&s_rol, toque.y, na_area && !sistema_app_encoberto() && s_rol.max > 0);
      }

      // O CLIQUE DO SISTEMA - ver TOQUE_TOLERANCIA_PX no topo do arquivo.
      if (toque.isPressed()) {
        if (!s_dedo_na_tela) {
          s_dedo_na_tela = true;
          s_toque_x0 = toque.x; s_toque_y0 = toque.y;
          s_toque_desvio = 0;
          s_toque_desde = t;
        }
        const int dx = abs(toque.x - s_toque_x0), dy = abs(toque.y - s_toque_y0);
        if (dx > s_toque_desvio) { s_toque_desvio = dx; }
        if (dy > s_toque_desvio) { s_toque_desvio = dy; }
        s_toque_x = toque.x; s_toque_y = toque.y;
        // SEGURAR UM ICONE DA HOME levanta ele para mover (shell.cpp). Com um
        // icone levantado o toque e dele: a soltura nao vira clique nem gesto.
        if (!s_app && !s_app_carregando && !sistema_app_encoberto() && !s_dedo_do_despertar && (t - s_acordou_em) > 500
         && home_segurar(s_toque_x, s_toque_y, t, t - s_toque_desde, s_toque_desvio)) {
          s_toque_consumido = true;
        }
      } else if (s_dedo_na_tela) {
        s_dedo_na_tela = false;
        s_ultimo_toque = t;
        home_soltar();   // grava a ordem, se um icone foi movido
        // ARRASTE NAO E CLIQUE. Sem esta guarda, terminar uma rolagem apertaria
        // o que estivesse embaixo do dedo - e o dedo termina em cima de algo,
        // sempre. rolagem_foi_arraste() e chamada em TODA soltura: ela tambem
        // zera a medida para o proximo toque.
        const bool rolou   = rolagem_foi_arraste(&s_rol);
        const bool arrasto = rolou || s_toque_desvio > TOQUE_TOLERANCIA_PX;
        // Duas guardas, e a primeira e a que resolve: e o mesmo dedo que
        // acordou a tela? A segunda pega o toque acidental logo depois.
        const bool valido = !arrasto && !s_dedo_do_despertar && (t - s_acordou_em) > 500
                         && !s_toque_consumido;
        if (diag_toque_ligado()) {
          ESP_LOGI(TAG, "toque: %lu ms, desvio %d px -> %s", (unsigned long)(t - s_toque_desde),
                   s_toque_desvio, valido ? "CLIQUE" : rolou ? "rolagem" : arrasto ? "arrasto"
                                         : s_toque_consumido ? "consumido (nivel)" : "despertar/carencia");
        }
        // GESTO (arraste lateral na home, deslize para cima no bloqueio). Um
        // arraste vertical de rolagem de app nao chega aqui porque o app so
        // ve gesto com a home aberta; o dedo que acordou a tela nunca conta.
        if (arrasto && !s_dedo_do_despertar && (t - s_acordou_em) > 500 && !s_toque_consumido) {
          tratar_gesto(s_toque_x - s_toque_x0, s_toque_y - s_toque_y0);
        }
        if (valido) {
          ETAPA("tratando toque (abrir/fechar app, ao_tocar)");
          tratar_toque(s_toque_x, s_toque_y);
        }
      }

      // BANCADA: o pedido do console, na tarefa certa.
      // Com a navegacao travada, trocar de app por fora tambem e recusado:
      // a trava e do app aberto, nao so do botao INICIO.
      // COM O APP AINDA "ABRINDO..." (montando o cartao) os pedidos ESPERAM a
      // proxima volta - a mesma regra do toque (tratar_toque engole). Atender
      // agora chamaria o ao_fechar() de um app cujo ao_abrir() nunca rodou, e
      // depois concluir_abertura_do_sd() abriria um app que ja saiu da tela.
      if (s_sim_fechar && !s_app_carregando) {
        s_sim_fechar = false; s_ultimo_toque = t;
        if (s_app && s_nav_travada) { ESP_LOGW(TAG, "fechar recusado: '%s' travou a navegacao", s_app->nome); }
        else if (s_app)             { fechar_app(); }
      }
      if (const app_t* ap = (s_app_carregando ? nullptr : s_sim_abrir)) {
        s_sim_abrir = nullptr; s_ultimo_toque = t;
        if (s_app && s_nav_travada) {
          ESP_LOGW(TAG, "abrir '%s' recusado: '%s' travou a navegacao", ap->nome, s_app->nome);
        } else {
          if (s_app) { fechar_app(); }
          abrir_app(ap);
        }
      }
      if (s_sim_x >= 0 && !s_dedo_na_tela) {
        const int sx = s_sim_x, sy = s_sim_y;
        s_sim_x = -1;
        s_ultimo_toque = t;
        ESP_LOGI(TAG, "toque simulado em %d,%d", sx, sy);
        tratar_toque(sx, sy);
      }

      // Fim do toque: a marca de consumido nao passa para o proximo. Tambem
      // limpa a marcada sem toque do reconhecedor em curso (o nivel viu um
      // dedo que o clique nao acompanhava).
      if (!s_dedo_na_tela) { s_toque_consumido = false; }

      // O dedo do despertar so deixa de contar quando SAI da tela. Depois do
      // bloco do clique, de proposito: na passada da soltura o reconhecedor
      // ainda precisa ver a bandeira de pe.
      if (!tocando) { s_dedo_do_despertar = false; }
    }

    if (!tab5_power_em_repouso()) {
      // A tentativa frustrada tem carencia de 5s. Sem isso, um repouso que
      // falha no meio desfaz tudo, devolve false, e 20ms depois a condicao de
      // ociosidade continua verdadeira e ele tenta OUTRA VEZ - um loop de
      // apagar e acender, que e o que faz as barras piscarem.
      const bool pode_tentar = (s_repouso_falhou == 0) || ((t - s_repouso_falhou) > 5000);

      // Pedido de tela acesa acabou de sair: o prazo recomeca agora.
      static uint32_t acordado_antes = 0;
      const uint32_t acordado = s_acordado.load();
      if (acordado_antes && !acordado) { s_ultimo_toque = t; }
      acordado_antes = acordado;
      if ((t - s_ultimo_toque) > ocioso_ms() && pode_tentar && !acordado) {
        // Diz o tempo CONFIGURADO junto: e a unica prova, no log, de que o
        // ajuste de Ajustes esta mesmo chegando aqui.
        ESP_LOGI(TAG, "repouso apos %u s sem toque (ajuste de Ajustes)",
                 (unsigned)ajustes_tela_s());
        // TESTADO: 20MHz quebrou a UART do Mesh (o servico voltou
        // a mostrar "sem modulo" mesmo com o C6L plugado e respondendo antes).
        // De volta a 40. A ideia seguinte - piso baixo SO quando tab5_grove_dono()
        // for GROVE_DONO_NINGUEM, alto quando Mesh/GPS estiver de pe - ainda
        // precisa de teste isolado (ver conversa) antes de entrar
        // aqui: falta saber ate onde o I2C interno (touch/INA226, que rodam
        // TODO repouso, com Grove vazio ou nao) tolera descer.
        // AVISA O APP ANTES DE DESMONTAR A TELA - ver o aviso grande em
        // kernel/app.h junto de ao_repouso/ao_retomar (o crash
        // da camera+repouso). Chamado aqui, ainda com o painel de pe, para o
        // app soltar hardware (DMA/etc) que nao pode conviver com o painel
        // sendo destruido e recriado por tab5_power_repouso() logo abaixo.
        ETAPA("repouso: entrando");
        // O fundo da tela de bloqueio (~1,6 MB na PSRAM) nao precisa dormir
        // junto: o despertar sorteia e pinta outro.
        if (s_bloqueado) { bq_anim_soltar(); }
        bq_parede_soltar();   // o papel de parede pronto da home tambem (~1,6 MB)
        if (app_vivo() && s_app->ao_repouso) {
          s_app->ao_repouso();
        }
        int piso = (s_app && s_app->piso_repouso_mhz > 0) ? s_app->piso_repouso_mhz : 40;
        if (fundo_piso_repouso() > piso) { piso = fundo_piso_repouso(); }   // o fundo segue rodando
        if (!tab5_power_repouso(piso)) {
          s_repouso_falhou = t;
          s_sujo_layout = true;      // a tela voltou: redesenha o que havia
          // tab5_power_repouso() FALHOU - o painel NAO foi desmontado, entao
          // o app fica de pe com a tela acesa e o hardware que ele acabou de
          // soltar em ao_repouso() (linhas acima) tem que voltar AGORA, senao
          // fica desligado ate o proximo repouso ter sucesso.
          if (app_vivo() && s_app->ao_retomar) {
            s_app->ao_retomar();
          }
        } else {
          s_repouso_falhou = 0;
          // LIGA A ISR AQUI - so agora, com a tela de fato apagada, e seguro
          // deixar o toque acordar a queue sem atrapalhar PERIODO_UI/OCIOSO
          // (ver o aviso grande junto de s_toque_isr_ok). zerar a notificacao evita
          // que uma sobra de evento de ANTES de dormir cause um acordar
          // espurio um ciclo depois de entrar aqui.
          if (s_toque_isr_ok) {
            ulTaskNotifyTakeIndexed(UI_AVISO_IDX, pdTRUE, 0);   // zera o que sobrou
            s_isr_acorda = true;
          }
        }
      } else {
        // O RELOGIO DO APP e o ritmo de quadro (kernel/frame_pacing): o periodo
        // pedido, em varreduras inteiras do painel.
        const uint32_t per = periodo_do_app();
        if (per && ritmo_falta_ms() == 0) { s_sujo_dados = true; }

        // AVISOS: o que os servicos disseram que mudou desde a ultima volta.
        // So o que o app declarou querer chega a ele; o resto e descartado
        // (a home atualiza a linha de servicos na hora, por conta propria).
        //
        // ENCOBERTO (bloqueio ou central de avisos), NADA E ENTREGUE: os
        // avisos ficam guardados ate o app voltar a ficar visivel. O app atras da tela de bloqueio NAO esta visivel, e
        // o ao_aviso de quem mostra conversa (ESP-NOW) marca tudo como lido
        // - era o aviso que sumia da barra sem ninguem abrir o app: chegava
        // com a tela apagada, o despertar bloqueava, e o app "lia" por tras.
        // A central aberta envelheceu (um aviso chegou ou foi lido): de novo.
        if (painel_avisos_mudou()) { s_sujo_layout = true; }
        const uint32_t av = sistema_app_encoberto() ? 0 : s_avisos.exchange(0, std::memory_order_relaxed);
        if (av) {
          if (app_vivo()) {
            const uint32_t dele = av & s_app->avisos;
            if (dele) {
              if (s_app->ao_aviso) { s_app->ao_aviso(dele); }
              else                 { s_sujo_dados = true; }
            }
          } else if (!s_app) {
            atualizar_linha_servicos();
          }
        }

        // GIRO DA TELA (kernel/orientation), so para app que segue o IMU. Gira
        // ENTRE quadros, aqui, antes do desenho: a passada completa que vem
        // logo abaixo ja sai na orientacao nova. Nunca com dedo na tela (nem
        // logo depois dele): um arraste viraria toque em outro lugar, e a
        // soltura cairia num botao que nao estava ali quando o dedo desceu.
        ETAPA("orientacao (IMU, I2C)");
        const bool pode_girar = app_vivo() && !s_bloqueado
                             && !tocando && !s_dedo_na_tela && (t - s_dedo_ms) > 300;
        if (orientacao_vigiar(t, pode_girar)) {
          rolagem_zerar(&s_rol);            // a rolagem era medida na outra altura
          tela_virtual_invalidar_tudo();
          if (s_app->ao_girar) { s_app->ao_girar(); }
          s_sujo_layout = true;             // barra, navegacao e app: tudo de novo
        }

        ETAPA("desenho (app / home)");
        const int64_t desenho_t0 = esp_timer_get_time();
        const bool desenho_completo = s_sujo_layout;
        comp_marcar_desenho();
        uint32_t fase_barra = 0, fase_conteudo = 0;
        if (s_sujo_layout) {
          const int64_t f0 = esp_timer_get_time();
          desenhar_barra_status(tela_virtual(), true);
          const int64_t f1 = esp_timer_get_time();
          desenhar_conteudo(true, true);
          fase_barra    = (uint32_t)(f1 - f0);
          fase_conteudo = (uint32_t)(esp_timer_get_time() - f1);
          // A pagina visivel agora reflete exatamente s_rol.atual - qualquer
          // rolagem pendente de antes deste redesenho completo ja saiu junto.
          rolagem_sincronizar(&s_rol);
        } else if (rolagem_pendente(&s_rol)) {
          // ROLAGEM: a posicao mudou, o VALOR de nada mudou. Em vez do
          // caminho de cima (limpar ~1,65MB + trocar de pagina), deslocamos
          // os pixels JA CORRETOS que estao na tela (tab5_tela_rolar - memcpy
          // em faixas, sem apagar nada) e so entao chamamos o app para
          // desenhar por cima - pulando a rajada de apagar e a troca de
          // pagina, mas NAO o "app redesenha tudo".
          //
          // completo_para_apps=true (mesmo sem limpar_area) e proposital:
          // a faixa que o deslocamento acabou de expor NUNCA foi pintada
          // NESTA posicao de rolagem antes - se cada app so olhasse o
          // dirty-tracking por campo (pintura_mudou_i/f/s), estrutura
          // estatica que so desenha quando "e passada completa" (titulo,
          // ajuda, botao - ver app_config) nunca preencheria aquela faixa.
          // Foi exatamente o bug visto ao vivo: rolar para baixo parecia bom
          // (a faixa nova e fina, passava despercebida), voltar para cima
          // expunha de uma vez o rastro de faixas nunca pintadas.
          const area_t a = area_util();
          const rolagem_passada_t p = rolagem_deslocar(&s_rol, a, TFT_BLACK);
          if (p.deslocou) {
            desenhar_conteudo(false, true, &p.exposta);
          } else {
            // Arraste rapido demais para o intervalo entre chamadas (delta
            // maior que a propria area util): nao ha o que reaproveitar, cai
            // para o redesenho completo normal no proximo ciclo.
            s_sujo_layout = true;
          }
        } else if (s_sujo_dados && !s_sujo_layout) {
          desenhar_conteudo(false, false);
        }

        // A barra so e repintada quando algo NELA mudou - a porcentagem, o
        // estado de carga ou o app. Repintar de dois em dois segundos a toa era
        // outra fonte de piscada, so que numa faixa menor.
        if ((t - s_ultima_barra) >= BARRA_MS) {
          s_ultima_barra = t;
          if (barra_mudou()) {
            tela_virtual().startWrite();
            desenhar_barra_status(tela_virtual(), false);
            tela_virtual().endWrite();
          }

          // Beep de notificacao + desligar o I2S ocioso - ver o aviso grande
          // em atualizar_beep_e_ociosidade_do_som(), mais acima no arquivo.
          atualizar_beep_e_ociosidade_do_som(t);

          // A linha de servicos da home, no mesmo tique e pela mesma regra -
          // ver atualizar_linha_servicos() (kernel/shell.cpp) para o "so
          // redesenha se o texto mudou".
          atualizar_linha_servicos();
        }
        if (comp_tem_sujeira()) {
          const uint32_t us = (uint32_t)(esp_timer_get_time() - desenho_t0);
          const uint32_t cpu = comp_contar_desenho(us, desenho_completo);
          // QUEM PESA. A passada completa media 90-130 ms - a media nao diz
          // de quem. A tabela por app (e por fase) sai no 'tela' do console.
          if (desenho_completo && fase_conteudo) {
            const uint32_t limpar = s_fase_limpar_us;
            anotar_passada(s_bloqueado ? "bloqueio" : (s_app ? s_app->nome : "home"), cpu,
                           fase_barra, limpar, fase_conteudo > limpar ? fase_conteudo - limpar : 0);
          }
        }
      }
    }

    // O pedido de sondagem "amadurece" aqui, depois do desenho: no proximo
    // ciclo ele sera atendido, com a tela ja mostrando o estado atual.
    if (s_grove_resondar) { s_grove_pedido_visto = true; }

    // Periodo adaptativo: rapido durante o uso, mais espacado quando parado.
    //
    // EM REPOUSO (Etapa 2): em vez de um numero fixo (PERIODO_ZZZ),
    // o teto vira "quanto falta para o proximo prazo REAL que este mesmo loop
    // ja respeita hoje" - a amostragem de sensores (1000ms fixo, ver o bloco
    // s_ultima_medida acima), a barra/beep (BARRA_MS) e a vigia do Grove
    // (VIGIA_MS). Nao inventa nenhum prazo novo, so para de acordar entre eles
    // a toa. O prazo do Grove SO entra na conta quando ele de fato avanca
    // sozinho (ajustes_grove() ligado e nenhuma re-sondagem em andamento) -
    // achado na revisao deste desenho: sem essa guarda, com o Grove desligado
    // (configuracao normal) ou uma re-sondagem em curso, s_grove_olhado para
    // de andar, a conta "quanto falta" ficaria presa em ZERO para sempre, e o
    // loop giraria sem parar exatamente no estado que deveria dormir.
    uint32_t periodo;
    if (tab5_power_em_repouso()) {
      if (s_toque_isr_ok) {
        const uint32_t decorrido_medida = t - s_ultima_medida;
        uint32_t prazo = (decorrido_medida < 1000) ? (1000 - decorrido_medida) : 0;

        const uint32_t decorrido_barra = t - s_ultima_barra;
        const uint32_t prazo_barra = (decorrido_barra < BARRA_MS) ? (BARRA_MS - decorrido_barra) : 0;
        if (prazo_barra < prazo) { prazo = prazo_barra; }

        if (ajustes_grove() && !s_grove_resondar) {
          const uint32_t decorrido_grove = t - s_grove_olhado;
          const uint32_t prazo_grove = (decorrido_grove < VIGIA_MS) ? (VIGIA_MS - decorrido_grove) : 0;
          if (prazo_grove < prazo) { prazo = prazo_grove; }
        }
        periodo = prazo;
      } else {
        periodo = PERIODO_ZZZ;   // ISR nunca subiu - o mesmo comportamento de sempre
      }
    }
    else {
      // ACORDADA: RAPIDA SO QUANDO HA MOTIVO. Ate aqui o loop
      // rodava a cada 20-25ms sempre que a tela estava acesa - 40 a 50
      // voltas por segundo, cada uma lendo o toque por I2C, com o aparelho
      // parado na mesa. Agora:
      //   - dedo na tela (ou ha menos de 500ms) ou trabalho pendente: 20ms,
      //     com a interrupcao do toque DESLIGADA (num arraste ela pulsaria a
      //     cada 5-20ms e so atrapalharia);
      //   - parada: a interrupcao LIGA e o loop dorme ate o proximo prazo
      //     real. Um toque, um aviso de servico ou um pedido de redesenho de
      //     outra tarefa acordam na hora (a espera na notificacao, la embaixo).
      const bool dedo = (t - s_ultimo_toque) < 500;
      const bool pendente = s_sujo_layout || s_sujo_dados || rolagem_pendente(&s_rol)
                         || comp_tem_sujeira() || s_app_carregando || s_sd_montando
                         || s_grove_resondar || s_sondando_em_fundo;
      if (dedo || pendente || !s_toque_isr_ok) {
        s_isr_acorda = false;
        periodo = PERIODO_UI;
      } else {
        uint32_t prazo = 1000;
        auto menor = [&](uint32_t intervalo, uint32_t desde) {
          const uint32_t dec = t - desde;
          const uint32_t falta = (dec < intervalo) ? (intervalo - dec) : 0;
          if (falta < prazo) { prazo = falta; }
        };
        menor(1000, s_ultima_medida);                    // sensores, 1Hz
        menor(BARRA_MS, s_ultima_barra);                 // barra de status
        if (ajustes_grove() && !s_grove_resondar) { menor(VIGIA_MS, s_grove_olhado); }
        menor(ocioso_ms(), s_ultimo_toque);              // hora de apagar a tela
        // (o relogio do app entra abaixo, depois do piso de PERIODO_UI)
        if (s_speaker_ocioso_desde) { menor(SPEAKER_OCIOSO_MS, s_speaker_ocioso_desde); }
        if (tab5_teclado_presente()) { menor(teclado_ms(), s_ultimo_teclado); }   // teclado fisico e consultado
        { const uint32_t falta = orientacao_prazo_ms(t); if (falta < prazo) { prazo = falta; } }   // vigia do giro (so com app que segue o IMU)
        if (prazo < PERIODO_UI) { prazo = PERIODO_UI; }
        s_isr_acorda = s_toque_isr_ok;
        periodo = prazo;
      }
      // O RELOGIO DO APP fura o piso de PERIODO_UI: o proximo quadro cai
      // logo depois de uma varredura, e esse momento pode estar a menos de
      // 20 ms (e com o dedo na tela tambem - arrastar nao pode travar uma
      // animacao).
      if (periodo_do_app()) {
        uint32_t falta = ritmo_falta_ms();
        if (falta < 1) { falta = 1; }
        if (falta < periodo) { periodo = falta; }
      }
    }

    // UM FRAME POR VOLTA (COMPOSITOR.md, regra 4). Tudo que esta volta
    // desenhou ja esta na pagina de tras (modo direto), e o rastreador anotou
    // onde. Aqui a troca e pedida - sem esperar: quem espera e o proximo
    // quadro, ao abrir. Volta sem desenho = nada a fazer.
    bool apresentou = false;
    if (!tab5_power_em_repouso() && comp_tem_sujeira()) {
      ETAPA("compositor (troca de pagina)");
      apresentou = comp_apresentar();
      if (apresentou) { comp_contar_volta((uint32_t)(esp_timer_get_time() - volta_t0)); }
    }

    // PROVA DE VIDA. Uma volta completa do loop terminou - a tela foi tratada,
    // os apps tiveram a vez deles, nada travou. E o sinal que o watchdog
    // inscrito la em cima espera; sem ele a cada volta, os 5s do prazo vencem
    // e o aparelho reinicia mesmo funcionando bem.
    esp_task_wdt_reset();
    s_voltas   = s_voltas + 1;   // escritor unico: esta tarefa
    s_volta_ms = tab5_agora_ms();
    ETAPA("esperando evento");

    // A espera na notificacao (ulTaskNotifyTake) no lugar do vTaskDelay() de sempre - acorda no MESMO
    // prazo calculado acima OU antes, se algo chegar na queue (toque de
    // verdade em repouso, ou sistema_acordar_interface() de um servico). O
    // CONTEUDO e o motivo do retorno sao ignorados de proposito: as
    // proximas linhas do PROXIMO ciclo ja conferem M5.Touch e todas as
    // bandeiras sozinhas, exatamente como sempre fizeram.
    if (apresentou && periodo == PERIODO_UI) {
      // NO RITMO DO PAINEL. Com o dedo na tela (ou trabalho pendente) a
      // volta dormia 20 ms DEPOIS de terminar: periodo real = 20 ms + o
      // desenho, ~30 q/s contra um painel de ~63 Hz. Os dois ritmos nao
      // batem, e de tempos em tempos um quadro fica na tela dois periodos -
      // a rolagem trepida mesmo sem rasgo nenhum.
      //
      // Aqui a volta que apresentou espera a TROCA (o painel comecou a varrer
      // o quadro novo) e recomeca na hora: toque lido logo depois da troca,
      // quadro novo pronto para a seguinte. A espera nao e a mais - o proximo
      // quadro teria de espera-la de qualquer jeito (comp_quadro_imagem,
      // passo 1). Vale SO no modo rapido: parada, a interface continua
      // dormindo ate o proximo prazo, como sempre.
      comp_esperar_troca();
      ulTaskNotifyTakeIndexed(UI_AVISO_IDX, pdTRUE, 0);   // consome um aviso que ja tenha chegado
    } else {
      ulTaskNotifyTakeIndexed(UI_AVISO_IDX, pdTRUE, pdMS_TO_TICKS(periodo));
    }
    // A cada volta, nao no passo de 2 s: o pendrive espera esta resposta
    // para ligar o USB (ver sistema_som_ceder_pinos).
    som_atender_reserva();
  }
}

void sistema_iniciar(void)
{
  //                      corpo           nome  stack param prio handle kernel
  // ajustes_iniciar() ja foi chamado no app_main, ANTES do M5.begin(), para a
  // tela nascer no brilho certo em vez de piscar no padrao e corrigir depois.
  pool_iniciar();         // tarefas fixas dos servicos que ligam/desligam e dos apps - ANTES de todos
  tab5_mem_marco("pool de vagas");
  tab5_sd_iniciar();      // cria o mutex do cartao ANTES de haver concorrencia
  tab5_mem_marco("cartao (tarefa)");
  tab5_log_iniciar();     // DEPOIS do SD (usa tab5_sd_montar/abrir) - guarda os logs no cartao
  tab5_mem_marco("log (tarefa+buffer)");
  tab5_monitor_iniciar(); // mede CPU/memoria o tempo todo e registra eventos (services/tab5_monitor)
  tab5_mem_marco("monitor (tarefa)");
  tab5_wifi_iniciar();    // cria a queue e a tarefa; NAO liga o radio
  tab5_mem_marco("wifi (tarefa, sem radio)");
  tab5_espnow_iniciar();  // espera o firmware nosso no C6 (peer_data) e liga o ESP-NOW
  tab5_mem_marco("espnow (tarefa)");
  tab5_ble_iniciar();     // cria a tarefa; NAO liga (ver o loop de interface)
  tab5_mem_marco("ble (tarefa, sem radio)");
  console_iniciar();      // comandos pelo monitor serial (help)
  tab5_mem_marco("console (tarefa)");

  // ISR DO TOQUE - ver o aviso grande junto de UI_AVISO_IDX,
  // mais acima no arquivo. Precisa existir ANTES da tarefa de interface
  // subir (ela consome a queue desde a primeira volta do loop) e DEPOIS do
  // M5.begin() (main.cpp) - e ele quem configura o GPIO 23 como entrada para
  // o driver do GT911; esta ISR so ACRESCENTA um tipo de interrupcao aquele
  // pino, nunca muda direcao/pull.
  {
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
      // INVALID_STATE = outro modulo ja instalou o servico global de ISR
      // antes de chegarmos aqui (o candidato mais provavel e o bring-up do
      // WiFi/C6 pelo esp_hosted, que faz o mesmo em eh_host_port_gpio.c) -
      // normal e seguro, so precisamos que ALGUEM tenha instalado, nao
      // importa quem.
      // No GT911 o proprio driver ja deixou o pino como entrada. Nos TDDI
      // ninguem configurou: entrada com pull-up (linha de interrupcao em
      // dreno aberto, ativa em baixo - sem dedo ela fica em repouso alto).
      if (!tab5_painel_toque_gt911()) {
        gpio_set_direction(GPIO_NUM_23, GPIO_MODE_INPUT);
        gpio_set_pull_mode(GPIO_NUM_23, GPIO_PULLUP_ONLY);
      }
      if (gpio_set_intr_type(GPIO_NUM_23, GPIO_INTR_ANYEDGE) == ESP_OK &&
          gpio_isr_handler_add(GPIO_NUM_23, isr_toque, nullptr) == ESP_OK) {
        // LIGADA DE VEZ: ela anota todo pulso (s_toque_pulso). Quem decide se
        // ela tambem acorda o loop e s_isr_acorda (repouso / tela parada).
        s_toque_isr_instalada = true;
        s_toque_isr_ok = tab5_painel_toque_gt911();
      }
    }
  }
  if (!s_toque_isr_instalada) {
    ESP_LOGW(TAG, "ISR de toque nao subiu - repouso usa o PERIODO_ZZZ fixo de sempre");
  } else if (!s_toque_isr_ok) {
    ESP_LOGI(TAG, "toque da tela %s: interrupcao so depois de provar o pino (validar_isr_tddi)",
             tab5_painel_nome());
  }
  // ISR DO TECLADO (ver isr_teclado). O G50 nao e de mais ninguem: entrada com
  // pull-up, borda de descida. Se nao subir, o teclado ainda acorda a tela -
  // so que no proximo prazo do repouso (ate ~1 s), pela consulta da fila.
  {
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << TAB5_TECLADO_INT;
    c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = GPIO_PULLUP_ENABLE;
    c.intr_type = GPIO_INTR_NEGEDGE;
    s_tecla_isr_ok = gpio_config(&c) == ESP_OK
                  && gpio_isr_handler_add((gpio_num_t)TAB5_TECLADO_INT, isr_teclado, nullptr) == ESP_OK;
    if (!s_tecla_isr_ok) { ESP_LOGW(TAG, "ISR do teclado (G%d) nao subiu - ele acorda a tela pela consulta de 1 s", TAB5_TECLADO_INT); }
  }

  // Handle CAPTURADO, nao NULL - sistema_camera_iniciar()/parar() (ver o
  // aviso grande em app.h) precisam saber se quem as chamou JA E esta
  // tarefa, para nao fazer ponte com elas mesmas (deadlock).
  home_ordem_carregar();   // a ordem dos icones do usuario (NVS), antes do primeiro desenho
  // O boot tambem e "a tela ligou" - a mesma regra do despertar. Aqui, antes
  // da tarefa de interface existir: o primeiro desenho ja sai bloqueado.
  bloqueio_ao_ligar_tela();
  s_relogio_feito = xSemaphoreCreateBinary();   // antes da interface: ela da, o console espera
  s_som_parou     = xSemaphoreCreateBinary();   // idem: ela da, o pendrive espera
  xTaskCreatePinnedToCore(vTaskInterface, "ui", 8192, NULL,   4,  &s_tarefa_interface, 1);
  registro_iniciar();
  tab5_mem_marco("ui + registro (tarefas)");
  ESP_LOGI(TAG, "interface no ar, %d aplicativo(s) registrado(s)", SISTEMA_NUM_APPS);
}


// Ver ETAPA no topo do arquivo. Leitura de outra tarefa, sem trava: palavras
// soltas, e um valor um passo atrasado nao muda o diagnostico.
void sistema_diag_interface(sistema_diag_t* d)
{
  const uint32_t agora = tab5_agora_ms();
  d->etapa          = (const char*)s_etapa;
  d->voltas         = s_voltas;
  d->ms_desde_volta = s_volta_ms ? agora - s_volta_ms : 0;
  d->ms_desde_dedo  = s_dedo_ms ? agora - s_dedo_ms : UINT32_MAX;
  d->toque_pulsos   = s_pulsos;
  d->toque_falhas   = s_vig_falhas;
  d->toque_presos   = s_vig_presos;
  d->toque_surdos   = s_vig_surdos;
  d->toque_status   = s_vig_status;
  d->repouso        = tab5_power_em_repouso();
  d->app            = s_app ? s_app->nome : nullptr;
}
