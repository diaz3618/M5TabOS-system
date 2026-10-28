// O "shell" of M5tabOS: barra of status, screen inicial (launcher), roteamento
// of touch para o app certo, e a calibracao of touch.
//
// -------------------------------------------------------------------------
// ETAPA 1 of ARCHITECTURE.md - metade of SHELL (aparencia)
// -------------------------------------------------------------------------
// Ate aqui isto vivia dentro of kernel/sistema.cpp, junto of MOTOR (agenda
// of drawing, ciclo of vida dos apps, politica of energia) - 2457 lines
// fazendo oito coisas diferentes. Partido em dois, sem mudar uma line of
// logica (a unica excecao esta documentada em kernel/internal.hpp, junto of
// troca area_util() -> sistema_area()): core.cpp e o motor, este arquivo e
// a aparencia.
//
// "shell" no sentido of sistema operacional (Windows Shell, GNOME Shell): a
// camada visual em iteration dos apps. NAO tem relacao com console.cpp (que e one
// shell of LINHA DE COMANDO, outro conceito, outro arquivo).
//
// UMA tarefa so manda no display e no I2C, of proposito - ver o aviso
// identico em core.cpp. Os apps nao desenham das proprias tarefas; a funcao
// draw() of cada one e chamada pelo motor, que mora no outro arquivo.

#include "app.h"
#include "internal.hpp"
#include "lockscreen_anim.hpp"
#include "virtual_screen.hpp"
#include "settings.h"
#include "tab5_ble.hpp"
#include "tab5_mesh_ble.hpp"
#include "tab5_wifi.hpp"
#include "tab5_espnow.hpp"
#include "tab5_screen.hpp"
#include "tab5_grove.hpp"
#include "tab5_sensors.hpp"
#include "notification.h"
#include "tab5_mesh.hpp"
#include "tab5_gps.hpp"
#include "tab5_sd.hpp"
#include "tab5_ui.hpp"
#include "tab5_time.hpp"
#include <time.h>
#include <string.h>
#include <strings.h>   // strcasecmp

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "shell";

// ---------------------------------------------------------------------------
// GEOMETRIA DO BOTAO DA FAIXA (a pilula in faixa of navigation) - NUM SO
// LUGAR, porque desenhar_barra_nav() PINTA neste rectangle
// e tratar_toque() now PRECISA saber o mesmo rectangle para so close o
// app when o touch cai dentro dele (antes, qualquer touch in faixa
// inteira of 96px - a LARGURA TODA of screen - fechava o app; so o Y era
// checado). Duas constantes divergentes aqui voltariam a open esse buraco.
static const int NAV_BTN_L  = 160;               // width of pilula (era 180, junto com a faixa mais baixa)
static const int NAV_BTN_A  = ALTURA_NAV - 32;   // height of pilula
static const int NAV_BTN_FOLGA = 48;            // touch alem of pilula, of cada lado (ver tratar_toque)
static const int NAV_BTN_Y0 = 16;                // offset of topo of faixa ate o topo of pilula

static const int ICONE_LADO   = 190;
static const int ICONE_COLS   = 3;

// ---------------------------------------------------------------------------
// CORRECAO VERTICAL DO TOQUE
// ---------------------------------------------------------------------------
// O panel capacitivo devolve o CENTROIDE of area of contato, e o finger nao
// toca a screen com a ponta que voce ve: ele toca com a polpa, alguns milimetros
// abaixo. O olho mira in borda visivel, o sensor mede o middle of mancha - e a
// leitura exit sistematicamente ABAIXO of onde voce achou que tocou.
//
// O sintoma e sempre o mesmo e foi exatamente o observado: "tenho que touch mais
// para top para acertar o botao". Nao e imprecisao of panel; e a diferenca
// entre onde voce mira e onde voce encosta.
//
// A 294 DPI deste panel, 10px sao ~0,85mm - dentro of faixa que os sistemas
// operacionais aplicam para a mesma coisa. Nao e chute cego: o comando `touch`
// of console mede o desvio real, e este numero existe para ser ajustado com
// aquele dado, nao com impressao.
//
// Fica no SHELL e nao nos apps of proposito: e propriedade of panel e of
// finger, igual para todo mundo. Cada app corrigindo por conta seria oito
// correcoes diferentes para uma causa so. O valor (TOQUE_DY) mora em
// kernel/internal.hpp: o touch por level, em core.cpp, aplica o mesmo.

// Diagnostico of touch, enabled pelo console. Imprime onde o finger caiu, cru e
// ja convertido - a unica forma of switch "parece impreciso" por one numero.
static bool s_diag_touch = false;
bool diag_touch_enabled(void) { return s_diag_touch; }
void system_diag_touch(bool enable)
{
  s_diag_touch = enable;
  ESP_LOGI(TAG, "diagnostico de toque %s (correcao atual: %d px)",
           enable ? "LIGADO" : "desligado", TOQUE_DY);
}

// Travado: drawn apagado in barra of navigation, para o olho saber que
// nao adianta touch. Ver sistema_travar_navegacao() logo abaixo.
//
// NAO 'static' - core.cpp tambem escreve false direto em fechar_app() (rede
// of seguranca: one app closed nao pode deixar a navigation travada para o
// next). Declarada extern em kernel/internal.hpp.
bool s_nav_travada = false;
void system_lock_navigation(bool t)
{
  s_nav_travada = t;
  if (t && s_panel_notices) { panel_notices_open(false); }   // open antes of trava: fecha
  s_dirty_layout = true;
}

// ---------------------------------------------------------------------------
// Icone of battery
// ---------------------------------------------------------------------------

// Tres estados of alimentacao, nao dois. O erro anterior era detectar "loading"
// so pela corrente positiva: a 100% o carregador para, a corrente vai a zero e o
// raio sumia, mesmo com o cabo in tomada.
//
// O discriminador e simples e confiavel: o sistema consome mais of 100mA. Se
// essa corrente NAO esta saindo of battery, ela so pode estar vindo of USB.
typedef enum {
  EN_BATERIA,      // descarregando: corrente claramente negativa
  EN_CARREGANDO,   // corrente entrando in battery
  EN_TOMADA        // alimentado por fora, battery sem switch carga (cheia)
} estado_energia_t;

static estado_energia_t estado_energia(const tab5_measurement_t& m)
{
  if (m.mA < -20.0f) { return EN_BATERIA;    }
  if (m.mA >  20.0f) { return EN_CARREGANDO; }
  return EN_TOMADA;
}

// Raio of carregamento, drawn com contorno para ficar legivel tanto sobre a
// parte cheia quanto sobre a parte vazia of battery.
static void draw_raio(lgfx::LovyanGFX& d, int cx, int cy, uint16_t color)
{
  d.fillTriangle(cx + 2, cy - 10, cx - 7, cy + 2, cx + 1, cy + 2, color);
  d.fillTriangle(cx - 2, cy + 10, cx + 7, cy - 2, cx - 1, cy - 2, color);
}

// Plugue, para when esta in tomada com a battery ja cheia - o mesmo que o
// celular mostra when termina of carregar e voce nao tirou o cabo.
//
// Dimensoes apertadas of proposito: ele desenha DENTRO of battery, que tem 30px
// of height e 3px of borda of cada lado. Sobram 24px, e este plugue ocupa 19.
static void draw_tomada(lgfx::LovyanGFX& d, int cx, int cy, uint16_t color)
{
  d.fillRect(cx - 5, cy - 9, 2, 5, color);           // pino esquerdo
  d.fillRect(cx + 3, cy - 9, 2, 5, color);           // pino direito
  d.fillRoundRect(cx - 7, cy - 4, 14, 9, 2, color);  // body
  d.fillRect(cx - 1, cy + 5, 2, 5, color);           // cabo
}

// Bateria estilo celular: moldura, polo, preenchimento proporcional e o raio
// when esta loading.
static void draw_battery(lgfx::LovyanGFX& d, int x, int y, int pct, estado_energia_t est)
{
  const int L = 58;              // width of body
  const int A = 30;              // height of body
  const int borda = 3;

  const int p = (pct < 0) ? 0 : (pct > 100 ? 100 : pct);

  const uint16_t color = (est != EN_BATERIA) ? TFT_GREEN
                     : (p < 20)            ? TFT_RED
                     : (p < 50)            ? TFT_YELLOW
                                           : TFT_GREENYELLOW;

  // Corpo: interior apagado e depois preenchido ate a fracao of carga.
  // SIS_FUNDO e nao TFT_DARKGREY: este fillRect limpa a area of icon E pinta
  // a parte VAZIA of battery. Com o cinza antigo sobrava one rectangle claro em
  // iteration e dentro dela - one pedaco of barra velha que nao acompanhou a troca.
  d.fillRect(x, y, L + 6, A, SYSTEM_BACKGROUND);                    // limpa a area toda
  d.drawRoundRect(x, y, L, A, 6, TFT_WHITE);
  d.drawRoundRect(x + 1, y + 1, L - 2, A - 2, 5, TFT_WHITE); // 2px of moldura
  d.fillRect(x + L + 1, y + A / 2 - 6, 5, 12, TFT_WHITE);    // polo positivo

  const int util = L - 2 * borda;
  const int cheio = (util * p) / 100;
  if (cheio > 0) {
    d.fillRect(x + borda, y + borda, cheio, A - 2 * borda, color);
  }

  // Simbolo central. Mesma tecnica nos dois casos: contorno escuro deslocado of
  // 1px e figura clara por top, para ficar legivel tanto sobre a parte cheia
  // quanto sobre a parte vazia of battery.
  if (est == EN_CARREGANDO) {
    draw_raio(d, x + L / 2 + 1, y + A / 2 + 1, TFT_BLACK);
    draw_raio(d, x + L / 2,     y + A / 2,     TFT_WHITE);
  } else if (est == EN_TOMADA) {
    draw_tomada(d, x + L / 2 + 1, y + A / 2 + 1, TFT_BLACK);
    draw_tomada(d, x + L / 2,     y + A / 2,     TFT_WHITE);
  }

  // Se o modelo nao souber estimar (pct < 0), marca com one tracinho.
  if (pct < 0) {
    d.drawFastHLine(x + borda + 4, y + A / 2, util - 8, TFT_LIGHTGREY);
  }
}

// ---------------------------------------------------------------------------
// Desenho
// ---------------------------------------------------------------------------
// Icone of WiFi: quatro arcos concentricos, aceso conforme o sinal.
//
// Simbolo em vez of text porque a barra e lida of relance: forma e color chegam
// antes of leitura. O name of rede continua disponivel no app Ajustes, que e
// onde voce vai when quer detalhe.
//
// Faixas of RSSI (dBm), as usuais of WiFi:
//   >= -55 excelente (4) | >= -65 bom (3) | >= -75 ok (2) | >= -85 fraco (1)
// O X FICA AO LADO, E NAO POR CIMA.
//
// A versao anterior riscava o proprio leque com dois tracos grossos. Ficou
// legivel e became horrivel: o X brigava com os arcos pelo mesmo espaco e o
// simbolo virava one borrao. A convencao boa mantem o
// leque intacto e poe o X SOLTO no canto of top, a right.
//
// Para isso o leque anda one pouco para a left e encolhe, abrindo o canto.
// O icon inteiro passa a ocupar ~46px of width em vez of ~40.
// Termometro of ~14x31 (era 12x26, +20%junto of troca of
// fonte), drawn a partir of centro of bulbo. Bulbo, haste e a coluna
// colorida dentro dela - o mesmo drawing serve para qualquer color, entao a
// temperatura pinta o liquido e o vidro stays sempre claro.
//
// ICONE VETORIAL, NAO ACOMPANHA FONTE SOZINHO - por isso os literais abaixo
// sao os originais * 1.2 (arredondados), e nao uma conta em time of execucao:
// e one drawing fixo, chamado of one unico lugar, sem reason para reprocessar
// escala a cada frame.
// A COR DA TEMPERATURA, continua: azul frio -> verde -> amarelo -> laranja ->
// vermelho. Antes eram tres faixas (azul ate 55, laranja, vermelho a 70) e o
// termometro passava a maior parte of dia no mesmo azul - nao contava nada.
// Os degraus seguem o que este chip faz of verdade: ~40 parado, ~50 com a screen
// e o radio ocupados, 55+ trabalhando, 70+ quente.
static uint16_t color_of_temp(float c)
{
  static const struct { float t; uint8_t r, g, b; } P[] = {
    { 35.0f,  80, 160, 255 },   // azul
    { 45.0f,   0, 210, 130 },   // verde
    { 55.0f, 255, 215,   0 },   // amarelo
    { 63.0f, 255, 130,   0 },   // laranja
    { 70.0f, 255,  40,  40 },   // vermelho
  };
  const int n = sizeof(P) / sizeof(P[0]);
  int r = P[0].r, g = P[0].g, b = P[0].b;
  if (c >= P[n - 1].t) { r = P[n - 1].r; g = P[n - 1].g; b = P[n - 1].b; }
  else {
    for (int i = 0; i + 1 < n; ++i) {
      if (c >= P[i].t && c < P[i + 1].t) {
        const float k = (c - P[i].t) / (P[i + 1].t - P[i].t);
        r = P[i].r + (int)((P[i + 1].r - P[i].r) * k);
        g = P[i].g + (int)((P[i + 1].g - P[i].g) * k);
        b = P[i].b + (int)((P[i + 1].b - P[i].b) * k);
        break;
      }
    }
  }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// A COLUNA SOBE COM A TEMPERATURA (20 a 80 C, a faixa of sensor) e o liquido
// toma a color of cor_da_temp(). Sem leitura: vidro vazio, bulbo cinza.
// Ocupa of cy-20 a cy+12.
static void draw_termometro(lgfx::LovyanGFX& d, int cx, int cy, bool tem, float c,
                                uint16_t fundo)
{
  const uint16_t vidro   = TFT_LIGHTGREY;
  const uint16_t liquido = tem ? color_of_temp(c) : (uint16_t)TFT_DARKGREY;

  // Haste: contorno claro com o miolo in color of fundo, para o liquido ter onde
  // aparecer sem o vidro sumir.
  d.fillRoundRect(cx - 5, cy - 20, 10, 24, 5, vidro);
  d.fillRoundRect(cx - 2, cy - 18, 5, 22, 2, fundo);
  if (tem) {
    float f = (c - 20.0f) / 60.0f;
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    const int h = 2 + (int)(f * 20.0f + 0.5f);        // 2..22 px of coluna
    d.fillRect(cx - 2, cy + 4 - h, 5, h, liquido);
  }

  d.fillCircle(cx, cy + 5, 7, vidro);
  d.fillCircle(cx, cy + 5, 5, liquido);               // bulbo
}

static void draw_wifi(lgfx::LovyanGFX& d, int cx, int cy, uint16_t color,
                          int barras, bool cortado)
{
  // Leque deslocado: o centro dele nao e mais o centro of icon.
  cx -= 7;

  // Desenhado como arcos: o ponto in base e mais tres coroas. fillArc cobre of
  // 225 a 315 graus, que e o leque of 90 graus apontando para top.
  // Apagado em cinza ESCURO, e nao no TFT_DARKGREY of antes: com a barra now
  // escura, aquele cinza medio ficava mais claro que o fundo e o leque "vazio"
  // chamava mais atencao que o cheio.
  const uint16_t off = 0x52AA;   /* rgb(82, 86, 82) */
  d.fillCircle(cx, cy + 9, 3, barras >= 1 ? color : off);
  for (int n = 1; n <= 3; ++n)
  {
    const int r = 5 + n * 5;
    d.fillArc(cx, cy + 9, r, r - 2, 225, 315,
              (barras >= n + 1) ? color : off);
  }

  if (cortado)
  {
    // POR CIMA of ponta of leque, comendo one pedaco dele - como no simbolo
    // usual of "sem connection". Nao e decoracao: o X sobreposto diz que aquele leque esta
    // interrompido, enquanto one X on lado dizia so "tem one X aqui tambem".
    //
    // 3px of espessura. A versao anterior tinha 4 mais one contorno preto of 6,
    // e o conjunto ficava mais pesado que o proprio leque que devia negar.
    const int ex = cx + 16;
    const int ey = cy - 4;
    const int b  = 6;
    tab5_line_grossa(d, ex - b, ey - b, ex + b, ey + b, 3, TFT_RED);
    tab5_line_grossa(d, ex + b, ey - b, ex - b, ey + b, 3, TFT_RED);
  }
}

// MODO ESP-NOW. Com o canal fixo, o WiFi released o roteador DE
// PROPOSITO - o radio esta of pe, conversando por ESP-NOW. O leque cinza com X
// dizia "sem connection", que e mentira: ha connection, so que outra. Neste modo a
// barra mostra o simbolo of ESP-NOW (o mesmo of icon of app: ponto of origem
// e tres ondas saindo) - ciano no ar, cinza com o radio parado.
static void draw_espnow(lgfx::LovyanGFX& d, int cx, int cy, bool no_ar)
{
  const uint16_t color = no_ar ? TFT_CYAN : (uint16_t)0x52AA;
  const int ox = cx - 9, oy = cy + 9;   // o ponto of origem, embaixo a left
  d.fillCircle(ox, oy, 3, color);
  for (int n = 1; n <= 3; ++n) {
    const int r = 3 + n * 6;
    d.fillArc(ox, oy, r, r - 2, 270, 360, color);
  }
}

// BLUETOOTH NA BARRA: a runa, a left of WiFi, so enquanto o
// Bluetooth esta enabled. Cor: azul = conectado a one no (Mesh BLE); cinza
// claro = enabled; cinza escuro = subindo; vermelho = falhou.
static int bt_visual(void)
{
  const tab5_ble_state_t e = tab5_ble_state();
  if (e == TAB5_BLE_OFF) { return 0; }
  if (e == TAB5_BLE_FAILED)    { return 3; }
  if (e == TAB5_BLE_STARTING || e == TAB5_BLE_STOPPING) { return 1; }
  return mesh_ble_conectado() ? 4 : 2;
}

static void draw_bt(lgfx::LovyanGFX& d, int cx, int cy, int v)
{
  const uint16_t color = (v == 4) ? (uint16_t)TFT_SKYBLUE : (v == 3) ? (uint16_t)TFT_RED
                     : (v == 2) ? (uint16_t)TFT_LIGHTGREY : (uint16_t)0x52AA;
  const int t = cy - 12, b = cy + 12, dx = 7, q = 6;
  for (int k = 0; k <= 1; ++k) {
    d.drawLine(cx + k, t, cx + k, b, color);
    d.drawLine(cx + k, t, cx + dx + k, t + q, color);
    d.drawLine(cx + dx + k, t + q, cx - dx + k, b - q, color);
    d.drawLine(cx + k, b, cx + dx + k, b - q, color);
    d.drawLine(cx + dx + k, b - q, cx - dx + k, t + q, color);
  }
}

// O que a barra MOSTRA of radio: o estado of WiFi, ou o modo ESP-NOW (100 no
// ar, 101 parado). Um numero so para o drawing e para o barra_mudou - os dois
// tem que concordar, senao switch of modo nao repinta.
static int radio_visual(void)
{
  if (tab5_espnow_canal_fixo()) { return tab5_espnow_pronto() ? 100 : 101; }
  return (int)tab5_wifi_state();
}

static void hora_now(char* out, size_t n);   // mais abaixo, junto of faixa of bottom

// Ultimo estado drawn in barra, para nao repintar when nada changed.
static int  s_barra_temp = -999;  // ultimo grau INTEIRO drawn (so in screen of bloqueio)
static char s_barra_hora[8] = ""; // a hora desenhada no canto (fora of bloqueio)
static bool s_barra_bloq = false; // o canto foi pintado como screen of bloqueio
static int  s_barra_pct  = -999;
static int  s_barra_wifi = -1;    // estado of WiFi com que a barra foi pintada
static int  s_barra_bt   = -1;    // estado of Bluetooth com que a barra foi pintada
static uint32_t s_barra_notif = 0;   // versao of notification ja desenhada
static estado_energia_t s_barra_estado = EN_BATERIA;
static const char* s_barra_name = nullptr;
// A COR DE CADA GRAVIDADE - uma so, para a barra, a central e o bloqueio
// falarem a mesma lingua.
static uint16_t color_of_level(notification_level_t n)
{
  return (n >= NOTIF_URGENTE) ? (uint16_t)TFT_RED
       : (n == NOTIF_ATENCAO) ? (uint16_t)TFT_ORANGE
       : (n == NOTIF_INFO)    ? (uint16_t)TFT_SKYBLUE
                              : (uint16_t)TFT_LIGHTGREY;   // ESTADO: atividade, nao novidade
}

// complete=true pinta o FUNDO of barra antes dos elementos. E preciso sempre
// que a screen tenha sido apagada (abertura, iteration of sleep, troca of app),
// senao sobra o preto of framebuffer e a barra parece ter sumido - foi o que
// aconteceu when o fillRect foi removido achando que era so o flash.
//
// complete=false e a atualizacao periodica: os elementos se sobrescrevem
// sozinhos (text com color of fundo, icones limpando o proprio rectangle) e
// paint o fundo inteiro aqui seria justamente o flash que a gente tirou.
void draw_barra_status(lgfx::LovyanGFX& d, bool complete)
{
  const int L = d.width();
  char buf[32];

  // Le of CACHE. A leitura of verdade acontece uma vez por segundo no loop of
  // tarefa; ir on I2C aqui of novo seria uma transacao a mais por redraw of
  // barra, sem nenhum dado novo.
  tab5_measurement_t m = tab5_power_latest();
  const estado_energia_t est = estado_energia(m);

  // EM FAIXAS. 1280x64 em deitado sao ~164KB escritos of uma vez, e a barra e
  // repintada em toda troca of app - o mesmo tipo of rajada que produzia a
  // piscada, so que menor. tab5_screen_clear respira entre as faixas.
  if (complete) {
    tab5_screen_clear(0, 0, L, ALTURA_BARRA, SYSTEM_BACKGROUND);
    // Os blocos of left, middle e right limpam os proprios retangulos logo
    // abaixo, todos em SIS_FUNDO e todos dentro deste. Marcados, eles somem.
    tab5_screen_mark_clean(0, 0, L, ALTURA_BARRA, SYSTEM_BACKGROUND);
  }

  // Fora of 'complete', NAO apagamos a barra inteira.
  //
  // O fillRect of width total era one flash visivel a cada repintura - discreto
  // enquanto o middle of barra era text, evidente depois que virou one icon com
  // arcos. Cada elemento now se vira: o text escreve com color of fundo (que
  // sobrescreve em vez of limpar) e os dois icones limpam so o proprio rectangle.
  //
  // Preco disso: todo text daqui PRECISA ter width fixa. Um name of app mais
  // curto que o anterior deixaria rabo of antigo in screen.
  // ---- ESQUERDA: a hora, e o app logo em seguida ----------------------
  //
  // Os dois juntos of proposito. Sao as duas coisas que voce le sem procurar -
  // que horas sao e onde estou - e o MEIO stays inteiro para os notices, que e
  // content que aparece e some. A hora ficava in faixa of bottom e a
  // temperatura of P4 aqui; trocaram of lugar: a hora e o que se consulta o
  // time todo, e o canto of top e onde todo celular a poe.
  //
  // NA TELA DE BLOQUEIO, NAO: ela ja tem a hora grande no middle, e a hora
  // repetida aqui em top seria a mesma informacao duas vezes. Ali o canto
  // continua com o P4 e a temperatura, como era.
  //
  // ESQ_FIM e a fronteira, e ela existe porque o bloco of middle ja comeu o name
  // of app uma vez: os dois limpavam retangulos que se cruzavam, e quem
  // desenhasse depois vencia. Agora cada lado sabe onde para.
  static const int ROT_X          = 16;
  static const int GAP_ROT_TERMO  = 14;  // espaco entre "P4" e o termometro
  static const int GAP_TERMO_TEMP = 15;  // espaco entre o termometro (+20%) e o numero
  static const int APP_X          = 168;
  static const int ESQ_FIM        = 340;

  tab5_screen_clear(0, 4, ESQ_FIM, ALTURA_BARRA - 8, SYSTEM_BACKGROUND);
  if (!s_locked) {
    hora_now(buf, sizeof(buf));
    tab5_fonte_body_negrito(d);
    tab5_text(d, 3.3f);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_WHITE, SYSTEM_BACKGROUND);
    d.drawString(buf, ROT_X, ALTURA_BARRA / 2);
    tab5_fonte_body(d);
  } else {
    // Ordem of leitura: "P4", o termometro, o numero. O rotulo diz DE QUEM e a
    // measurement, o simbolo diz o QUE e, o numero diz QUANTO.
    float c = 0.0f;
    const bool tem = tab5_sensors_temp(&c);
    tab5_text(d, 3.0f);   // +20%
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_LIGHTGREY, SYSTEM_BACKGROUND);
    d.drawString("P4", ROT_X, ALTURA_BARRA / 2);
    // Medido, nao fixo: a width of "P4" depende of fonte of momento.
    const int termo_x = ROT_X + (int)d.textWidth("P4") + GAP_ROT_TERMO;
    draw_termometro(d, termo_x, ALTURA_BARRA / 2, tem, c, SYSTEM_BACKGROUND);
    if (tem) { snprintf(buf, sizeof(buf), "%.0f°C", (double)c); }
    else     { snprintf(buf, sizeof(buf), "--"); }
    d.drawString(buf, termo_x + GAP_TERMO_TEMP, ALTURA_BARRA / 2);
  }

  // Nome of app em BRANCO e mais encorpado que o resto: e a resposta para "onde
  // eu estou", que se le antes of qualquer outra coisa in barra. E o unico
  // TITULO que aparece em toda screen of sistema, entao e o unico text of
  // barra que troca para a Orbitron_Light (ver tab5_fonte_titulo em
  // tab5_ui.hpp) - o resto of barra (P4, wifi, battery) sao leituras
  // curtas, nao titulo, e ficam in fonte of body.
  //
  // TAMANHO NATIVO (1) e a base ideal para uma fonte bitmap como a Orbitron -
  // escalar por uma fracao descarta/duplica line of pixel of one jeito
  // irregular e engrossa o traco (foi por isso que "CONFIG" in barra parecia
  // uma fonte DIFERENTE of sample of galeria em Ajustes). Mas 1.0 became
  // grande demais aqui in praticafoi reduzir uns 20%,
  // entao 0.8 (ainda bem mais limpo que uma fracao tipo 0.6, so one pouco
  // aquem of nativo).
  //
  // O negrito duplo-com-deslocamento que existia aqui era remendo para a
  // Font0 nao ter peso - a Orbitron_Light ja e uma fonte "leve" (light) of
  // proposito grafico, entao NAO desenhamos duas vezes: a segunda copia
  // deslocada 1px ficaria borrada, nao "em negrito", numa fonte com traco
  // fino e regular como esta.
  // Roboto negrito nativa (ver tab5_titulo em tab5_ui.hpp)
  // - era a FreeMonoBold a 1,04x, a ultima escala fracionaria of barra.
  tab5_titulo(d, 28);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, SYSTEM_BACKGROUND);
  snprintf(buf, sizeof(buf), "%.14s", s_app ? s_app->name : "Home");
  d.drawString(buf, APP_X, ALTURA_BARRA / 2);
  tab5_fonte_body(d);

  // Estado of WiFi. Tudo daqui exit of cache: tab5_wifi_* nunca vai on C6, quem
  // conversa pelo SDIO e a tarefa of servico, uma vez a cada 5s.
  const tab5_wifi_state_t ew = tab5_wifi_state();

  uint16_t color_wifi = TFT_LIGHTGREY;
  int      barras   = 0;
  bool     cortado  = true;

  if (ew == TAB5_WIFI_CONNECTED)
  {
    color_wifi = TFT_GREENYELLOW;
    cortado  = false;
    const int r = (int)tab5_wifi_rssi();
    // Sem leitura of sinal, mostramos o icon CHEIO em vez of vazio: estamos
    // conectados of fato (temos IP), e draw zero barras diria on olho que a
    // connection esta ruim - trocaria uma informacao ausente por uma errada.
    barras = (r == TAB5_WIFI_RSSI_UNKNOWN) ? 4
           : (r >= -55) ? 4 : (r >= -65) ? 3 : (r >= -75) ? 2 : 1;
  }
  else if (ew == TAB5_WIFI_STARTING || ew == TAB5_WIFI_CONNECTING)
  {
    // Amarelo e SEM o X: esta tentando, ainda nao falhou. Mostrar o X aqui
    // faria a screen dizer "sem connection" durante uma connection em andamento.
    color_wifi = TFT_YELLOW;
    barras   = 0;
    cortado  = false;
  }
  else if (ew == TAB5_WIFI_FAILED || ew == TAB5_WIFI_NO_CREDENTIALS
        || ew == TAB5_WIFI_C6_UNRESPONSIVE)
  {
    // C6_MUDO tem que estar AQUI e em Ajustes: nao esta disabled, esta
    // QUEBRADO, e a diferenca decide se voce vai on Ajustes religar (inutil) ou
    // atras of gravador. Todo estado novo of enum precisa ser revisitado nos
    // dois lugares.
    color_wifi = TFT_RED;
  }

  // ---- DIREITA: aviso, wifi, battery ----------------------------------
  //
  // Tudo agrupado no mesmo canto of proposito. O wifi ficava no MEIO of barra,
  // longe of battery, e os dois sao a mesma pergunta - "como esta o aparelho
  // now". Separados, o olho tinha que varrer a barra inteira para juntar.
  //
  // As posicoes saem of right para a left, ancoradas no icon of
  // battery: assim mexer numa width nao desalinha as outras.
  const int bx      = L - 16 - 64;        // icon of battery
  const int pct_dir = bx - 12;            // borda right of "100%"
  // 82: o suficiente para o braco direito of X (que chega a cx+22) nao encostar
  // no numero of porcentagem, e nao mais que isso - a 96 o icon ficava released
  // no middle of caminho, longe of grupo a que pertence.
  const int wifi_cx = pct_dir - 82;       // centro of icon of wifi

  // O MEIO E SO DOS AVISOS, e ele comeca depois of ESQ_FIM. Antes o rectangle
  // of aviso era ancorado no wifi e crescia 150px para a left, entrando por
  // top of name of app - era isso que comia o name.
  const int bt_v     = bt_visual();
  const int bt_cx    = wifi_cx - 46;
  const int middle_ini = ESQ_FIM + 8;
  const int middle_end = bt_v ? (bt_cx - 18) : (wifi_cx - 30);

  notification_item_t lista[NOTIF_MAX_FONTES];
  const int n_notices = notification_listar(lista, NOTIF_MAX_FONTES);
  const notification_item_t& it = lista[0];
  tab5_screen_clear(middle_ini, 4, middle_end - middle_ini, ALTURA_BARRA - 8, SYSTEM_BACKGROUND);
  if (n_notices > 0) {
    // UM PONTO POR APP A MAIS, in color of gravidade dele, a right of
    // pastilha. Antes as outras viravam so "+N" - dava para saber QUANTAS,
    // nunca se alguma delas era vermelha. Os pontos cabem ate onde o middle
    // deixa (sempre sobrando 80 px para a pastilha); o que nao couber iteration
    // a ser "+N" in frente of text. Tocar a barra abre a central of notices,
    // com uma line inteira por app.
    const int PONTO_P = 22;
    int pontos = n_notices - 1;
    const int cabem = (middle_end - middle_ini - 80) / PONTO_P;
    if (pontos > cabem) { pontos = cabem > 0 ? cabem : 0; }
    const int outras = (n_notices - 1) - pontos;
    for (int k = 0; k < pontos; ++k) {
      d.fillCircle(middle_end - PONTO_P / 2 - k * PONTO_P, ALTURA_BARRA / 2, 8,
                   color_of_level(lista[pontos - k].level));
    }
    const int middle_end_pastilha = middle_end - (pontos ? pontos * PONTO_P + 6 : 0);
    const int middle_cx = (middle_ini + middle_end_pastilha) / 2;
    const notification_level_t level = it.level;
    // A FONTE EM DESTAQUE (notif_destaque): a mais grave, e no empate a mais
    // recente - com o summary DELA ("1 em LongFast"). As outras ativas viram
    // "+N" in frente, que o corte por width (abaixo) nunca come. Antes,
    // varias fontes viravam "N notices" SOMANDO pontos of Trilha com mensagens
    // of Mesh - uma message nova sumia dentro of numero.
    char av[40];
    char summary[NOTIF_RESUMO];
    if (it.summary[0]) { snprintf(summary, sizeof(summary), "%s", it.summary); }
    else              { snprintf(summary, sizeof(summary), "%d %s", it.count, it.name); }
    if (outras > 0) { snprintf(av, sizeof(av), "+%d %s", outras, summary); }
    else            { snprintf(av, sizeof(av), "%s", summary); }
    // PASTILHA, e nao text released. Fundo colorido com o text escuro por top:
    // o aviso vira one objeto que se destaca of barra, em vez of mais one text
    // cinza competindo com os outros dois.
    const uint16_t cn = color_of_level(level);
    const int alt        = ALTURA_BARRA - 20;
    const int disponivel = middle_end_pastilha - middle_ini;

    // O BUG DE VERDADE ESTAVA AQUI: "cabe ou nao desenha nada".
    //
    // Na HOME (retrato, L=720) o middle of barra tem so ~168px - o wifi e a
    // battery ja tomam o resto. "%d no canal" com contagem of tres digitos
    // ("100 no canal", 12 caracteres) mede 172px e NAO cabia; o code antigo
    // simplesmente pulava o fillRoundRect e o drawString inteiros. O estado
    // interno estava certo o time todo - s_nao_lidas subiu, notif_publicar()
    // rodou, notif_versao() changed, a barra ate REDESENHOU o rectangle of middle -
    // so que com nada dentro dele. Por fora, silencio; por dentro, everything certo.
    // E o default of bug que ja apareceu neste sistema outras vezes: one estado
    // real sem representacao visual none.
    //
    // Um canal of malha PARADO semanas acumula tres digitos com facilidade -
    // "no mes passado" e exatamente o time que leva.
    //
    // A CORRECAO: o aviso SEMPRE aparece. Se o text of summary nao cabe
    // inteiro, ele e cortado para caber - errado por excesso of informacao
    // (numero truncado) e nao por ausencia total, que e o unico dos dois erros
    // que pode ser confundido com "nao tem nada para notify".
    // MEDIDO DE VERDADE, nao mais "strlen*12": aquela conta vinha of width
    // fixa of Font0 (6px/caractere) e a Font4 e PROPORCIONAL - one "i" e one "m"
    // nao ocupam o mesmo espaco. d.setTextSize() precisa vir ANTES of
    // d.textWidth(), porque a measurement exit no tamanho ATUAL of text.
    tab5_text(d, 3);   // +50%
    // NEGRITO, para o aviso read of longe. ANTES of medir, nao so antes of draw: a Bold e one
    // pouco mais larga que a Regular, e a conta of corte por caractere (logo
    // abaixo) tem que cortar pelo tamanho que sera DE FATO drawn, senao o
    // text vaza alguns pixels of espaco disponivel.
    tab5_fonte_body_negrito(d);
    // Corta MEDINDO (fonte proporcional) e por caractere UTF-8 - o summary
    // tem acento ("Cartão no computador"), e o corte por byte desenhava lixo.
    tab5_cortar_on_caber(d, av, disponivel - 28);
    const int larg = (int)d.textWidth(av) + 28;

    if (av[0] && larg <= disponivel) {
      d.fillRoundRect(middle_cx - larg / 2, 10, larg, alt, alt / 2, cn);
      d.setTextDatum(middle_center);
      d.setTextColor(TFT_BLACK, cn);
      d.drawString(av, middle_cx, ALTURA_BARRA / 2);
    } else if (disponivel >= 16) {
      // MESMO 1 CARACTERE NAO COUBE: uma bolinha in color certa. Perde o numero,
      // mas nao perde a existencia of aviso - que e a garantia que este bloco
      // inteiro existe para dar.
      d.fillCircle(middle_cx, ALTURA_BARRA / 2, 7, cn);
    }
    tab5_fonte_body(d);   // devolve - ver o aviso of negrito, mais acima
  }

  // 56 of width, e nao 32: o icon cresceu para caber o X on lado. Um
  // rectangle curto deixaria middle X in screen when a connection voltasse.
  tab5_screen_clear(wifi_cx - 26, 4, 60, ALTURA_BARRA - 8, SYSTEM_BACKGROUND);
  if (bt_v) {
    tab5_screen_clear(bt_cx - 14, 4, 28, ALTURA_BARRA - 8, SYSTEM_BACKGROUND);
    draw_bt(d, bt_cx, ALTURA_BARRA / 2, bt_v);
  }
  s_barra_bt = bt_v;

  // A conta of centro, porque "-1" released viraria numero magico em duas semanas:
  // o drawing poe o ponto of base em cy+9 e o arco maior sobe 20px a partir
  // dali, entao o icon ocupa of (cy-11) a (cy+12) - centro em cy+0.5. Para
  // esse centro cair no middle of barra, cy = ALTURA_BARRA/2 - 1.
  // (Antes estava (ALTURA_BARRA-18)/2, que deixava o icon 8px acima of centro.)
  const int rv = radio_visual();
  if (rv >= 100) { draw_espnow(d, wifi_cx, ALTURA_BARRA / 2 - 1, rv == 100); }
  else           { draw_wifi(d, wifi_cx, ALTURA_BARRA / 2 - 1, color_wifi, barras, cortado); }

  // Bateria a right: so o icon e a porcentagem. Tensao, corrente e potencia
  // vivem no app Energia - a barra nao e lugar of numero of diagnostics.
  draw_battery(d, bx, (ALTURA_BARRA - 30) / 2, m.pct, est);

  // TAMANHO PROPRIO, explicito - antes herdava o que sobrava of P4 ou of
  // aviso (dependendo se havia aviso in pass), one acoplamento fragil que
  // ninguem tinha escolhido. 2.5 (o of P4) * 0.85 = 2.1; +35% -> 2.835.
  tab5_text(d, 2.835f);
  d.setTextDatum(middle_right);
  d.setTextColor(TFT_WHITE, SYSTEM_BACKGROUND);
  if (m.pct < 0) { snprintf(buf, sizeof(buf), "--%%"); }
  else           { snprintf(buf, sizeof(buf), "%3d%%", m.pct); }
  d.drawString(buf, pct_dir, ALTURA_BARRA / 2);

  { float c = 0.0f; s_barra_temp = tab5_sensors_temp(&c) ? (int)(c + 0.5f) : -999; }
  hora_now(s_barra_hora, sizeof(s_barra_hora));
  s_barra_bloq   = s_locked;
  s_barra_pct    = m.pct;
  s_barra_wifi   = rv;
  s_barra_notif  = notification_versao();
  s_barra_estado = est;
  s_barra_name   = s_app ? s_app->name : nullptr;
}

// Verdadeiro se algo visivel in barra changed since o ultimo drawing dela.
bool barra_changed(void)
{
  tab5_measurement_t m = tab5_power_latest();     // idem: cache, nao I2C
  const char* name = s_app ? s_app->name : nullptr;
  // GRAU INTEIRO, e nao o float: a decimal muda quase toda leitura e a barra
  // seria repintada 1x por segundo para sempre - justamente o gasto que a
  // funcao existe para evitar.
  // A temperatura so conta in screen of bloqueio (e o unico lugar em que ela
  // esta nesta barra); fora dela, o que vira sozinho e a hora.
  float c = 0.0f;
  const int temp = tab5_sensors_temp(&c) ? (int)(c + 0.5f) : -999;
  char hora[8];
  hora_now(hora, sizeof(hora));

  return (m.pct != s_barra_pct)
      || (s_locked != s_barra_bloq)
      || (s_locked ? (temp != s_barra_temp) : (strcmp(hora, s_barra_hora) != 0))
      || (estado_energia(m) != s_barra_estado)
      || (name != s_barra_name)
      // Sem esta line o icon of WiFi so mudava when a BATERIA mudava:
      // conectar ou cair nao repintava nada, e a barra mentia por minutos.
      || (radio_visual() != s_barra_wifi)
      || (bt_visual() != s_barra_bt)
      // Aviso novo tem que aparecer in hora, nao no next 1% of battery. A
      // VERSAO muda so when algo changed of verdade - one servico que republica
      // o mesmo estado nao faz a barra repintar.
      || (notification_versao() != s_barra_notif);
}

// Ultima line of servicos desenhada. Existe pelo mesmo reason of
// barra_mudou(): a faixa e redesenhada so em repintura COMPLETA, e o content
// dela muda sozinho - o Grove troca of modulo, o cartao entra, o radio cala.
// Sem comparar, a line envelheceria in screen ate alguem open e close one app.
static char s_nav_txt[128] = "";
static char s_nav_temp[16] = "";  // a temperatura que a faixa mostra (com app open)

// A HORA DA BARRA DE CIMA (e of screen of bloqueio). "--:--" enquanto o clock nao foi
// acertado (sem RTC valido nem rede) - melhor que uma hora of 1970.
static void hora_now(char* out, size_t n)
{
  const uint32_t t = tab5_time_epoch();
  if (!t) { snprintf(out, n, "--:--"); return; }
  struct tm lt;
  tab5_local_time(t, &lt);
  snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

// THE P4's TEMPERATURE IN THE BOTTOM STRIP (the clock moved up to the top bar).
//
// The same column layout GROVE and CARD use - small caption on top, value
// below - with the thermometer on the left, over the height of both lines:
//
//   |  P4 · avg 41°
//   o  47°C
//
// The caption says WHOSE reading this is and carries the AVERAGE since boot
// (tab5_sensors_average_temp): a number on its own doesn't say whether 47 is
// high or normal for this device; next to the average, it does. The
// thermometer rises and changes color with the reading (cor_da_temp), so you
// can read the state without reading the number.

// Key for what's drawn: only repaints when the WHOLE degree (or the
// average) changes - the decimal would change on almost every reading.
static void temp_chave(char* out, size_t n)
{
  float c = 0.0f, m = 0.0f;
  const int ci = tab5_sensors_temp(&c) ? (int)(c + 0.5f) : -999;
  const int mi = tab5_sensors_average_temp(&m) ? (int)(m + 0.5f) : -999;
  snprintf(out, n, "%d/%d", ci, mi);
}

static const int TEMP_TERMO_L = 26;   // thermometer (14) plus the gap to the text

// Builds both texts and returns the width of the whole block, for whoever
// wants to center it. Leaves the font on body size.
static int temp_textos(lgfx::LovyanGFX& d, char* rot, size_t nr, char* val, size_t nv,
                       bool* tem, float* c)
{
  float m = 0.0f;
  *tem = tab5_sensors_temp(c);
  if (tab5_sensors_average_temp(&m)) { snprintf(rot, nr, "P4 · avg %.0f°", (double)m); }
  else                              { snprintf(rot, nr, "P4"); }
  if (*tem) { snprintf(val, nv, "%.0f°C", (double)*c); }
  else      { snprintf(val, nv, "--"); }
  tab5_fonte_body(d);
  tab5_text(d, 1.95f);
  const int lr = (int)d.textWidth(rot);
  tab5_text(d, 3.45f);
  const int lv = (int)d.textWidth(val);
  return TEMP_TERMO_L + (lr > lv ? lr : lv);
}

// Desenha o bloco com a borda left em x0. Nao limpa nada: quem chama ja
// limpou a faixa (ou a parte dela que e sua).
static void draw_temp_nav(lgfx::LovyanGFX& d, int x0)
{
  const int y = d.height() - ALTURA_NAV;
  char rot[32], val[16];
  bool tem; float c = 0.0f;
  temp_textos(d, rot, sizeof(rot), val, sizeof(val), &tem, &c);

  // O termometro ocupa of cy-20 a cy+12: cy = middle of faixa + 4 o centra.
  draw_termometro(d, x0 + 7, y + ALTURA_NAV / 2 + 4, tem, c, TFT_BLACK);

  const int tx = x0 + TEMP_TERMO_L;
  d.setTextDatum(top_left);
  tab5_text(d, 1.95f);   // o mesmo par of tamanhos of rubrica/valor of GROVE e CARTAO
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  const int rot_y = y + 8;
  d.drawString(rot, tx, rot_y);
  const int alt_rot = d.fontHeight();
  tab5_text(d, 3.45f);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString(val, tx, rot_y + alt_rot + 4);
  tab5_fonte_body(d);
}

// Com one app open a temperatura stays a ESQUERDA of pilula - so ela e
// repintada when o grau vira, nunca a pilula.
static void draw_temp_nav_app(lgfx::LovyanGFX& d)
{
  const int y = d.height() - ALTURA_NAV;
  int l = d.width() / 2 - NAV_BTN_L / 2 - 24;   // ate perto of pilula, sem encostar
  if (l > 240) { l = 240; }
  tab5_screen_clear(16, y + 1, l, ALTURA_NAV - 1, TFT_BLACK);
  draw_temp_nav(d, 24);
}

// Monta os dois campos. TUDO DE CACHE: none sondagem, none montagem of
// cartao, nada que va on I2C. Esta funcao roda no tique periodico, e o caminho
// of drawing nao e lugar of ir on hardware.
static void montar_servicos(char* esq, size_t te, char* dir, size_t td)
{
  if (!settings_grove()) {
    snprintf(esq, te, "off");
  } else if (tab5_mesh_servico_active()) {
    // Com o servico of pe, o que interessa nao e "tem one C6L ali" e sim se ele
    // esta respondendo e quantos nos alcanca.
    snprintf(esq, te, "Meshtastic - %d nodes, %s",
             tab5_mesh_num_nos(), tab5_mesh_no_ar() ? "online" : "silent");
  } else if (tab5_gps_service_active()) {
    // Mesma ideia of Mesh acima: o que importa e se ha FIX of verdade, nao so
    // "tem one GPS ali". qualidade==0 e "recebo o modulo mas ainda sem fix" -
    // acontece nos primeiros segundos, ou a ceu closed.
    const nmea_position_t p = tab5_gps_position();
    if (p.valid && p.qualidade > 0) {
      snprintf(esq, te, "GPS - fix (%u sats)", (unsigned)p.sats_usados);
    } else {
      snprintf(esq, te, "GPS - no fix");
    }
  } else {
    snprintf(esq, te, "%s", grove_name(tab5_grove_type()));
  }

  snprintf(dir, td, "%s", tab5_sd_description());
}

// DUAS COLUNAS COM RUBRICA, e nao uma line corrida.
//
// A line corrida saia of area em portrait - 90 caracteres nao cabem em 720px -
// e mesmo cabendo era one paragrafo onde deveria haver one panel. Com rubrica
// pequena em top e valor embaixo, o olho acha o field sem read o text todo.
static void draw_servicos(lgfx::LovyanGFX& d, const char* esq, const char* dir)
{
  const int L = d.width();
  const int y = d.height() - ALTURA_NAV;

  // A MAIOR DAS QUE SOBRARAM. 1280x95 sao ~243KB numa
  // rajada, e ela e repintada a cada mudanca of line of servicos.
  tab5_screen_clear(0, y + 1, L, ALTURA_NAV - 1, TFT_BLACK);

  // Teto e divisor: e o que transforma dois textos soltos num panel. O divisor
  // vai so in height of content, sem encostar nas bordas - encostando, ele
  // viraria uma parede e cortaria a faixa em duas telas.
  // TRES COLUNAS: GROVE | P4 | CARTAO. A of middle e estreita e
  // so tem a temperatura; as dos lados dividem o resto.
  const int middle_l = 210;
  const int lado_l = (L - middle_l) / 2;
  d.drawFastHLine(0, y, L, SYSTEM_LINE);
  d.drawFastVLine(lado_l, y + 16, ALTURA_NAV - 32, SYSTEM_LINE);
  d.drawFastVLine(lado_l + middle_l, y + 16, ALTURA_NAV - 32, SYSTEM_LINE);

  {
    char rot[32], val[16];
    bool tem; float c = 0.0f;
    const int bl = temp_textos(d, rot, sizeof(rot), val, sizeof(val), &tem, &c);
    draw_temp_nav(d, L / 2 - bl / 2);
  }

  struct { const char* rot; const char* val; int cx; } col[2] = {
    { "GROVE",  esq, lado_l / 2 },
    { "CARD",   dir, L - lado_l / 2 },
  };

  for (int i = 0; i < 2; ++i) {
    d.setTextDatum(top_center);

    // POSICAO DO ROTULO E DO VALOR MEDIDA DE VERDADE (d.fontHeight()), nao
    // mais "y+22"/"y+44" fixos (cortavam a parte of bottom of text): os offsets antigos foram calculados a
    // olho para o tamanho of ANTES of aumento, e nao acompanharam when o
    // rotulo/valor cresceram (+30%/+15%). Medir a height
    // real of CADA tamanho antes of position o next elimina esse
    // descompasso of vez - cresce o text, cresce o espaco reservado, junto.
    tab5_text(d, 1.95f);  // +30%; +50%
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    const int rot_y = y + 8;   // faixa of 72 px: rotulo + valor + respiro cabem com 8 em top
    d.drawString(col[i].rot, col[i].cx, rot_y);
    const int alt_rot = d.fontHeight();

    tab5_text(d, 3.45f);  // +15%; +50%
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    // Corta no que cabe in METADE of screen, e nao no tamanho of buffer: o text
    // e centrado, entao one valor comprido sangraria para os dois lados. Corta
    // caractere por caractere medindo of verdade (d.textWidth) - a conta por
    // "12px por caractere" era of Font0 (width fixa) e nao vale para a Font4
    // (proporcional).
    char cabe[40];
    snprintf(cabe, sizeof(cabe), "%s", col[i].val);
    tab5_cortar_on_caber(d, cabe, lado_l - 32);   // por caractere: "nós" tem acento
    d.drawString(cabe, col[i].cx, rot_y + alt_rot + 4);
  }
}

// Chamada pelo loop principal (core.cpp) no mesmo tique of barra of status -
// so redesenha se o TEXTO changed. Comparar strings of 90 bytes of dois em dois
// segundos e mais barato que paint a faixa a toa. So faz sentido in HOME
// (s_app==NULL): com one app open a faixa mostra o botao, nao servicos.
void atualizar_line_servicos(void)
{
  if (s_locked) { return; }   // a faixa of bottom e preta enquanto locked
  char temp[16];
  temp_chave(temp, sizeof(temp));
  if (s_app) {
    // Com app open so a temperatura muda sozinha nesta faixa.
    if (strcmp(temp, s_nav_temp) != 0) {
      snprintf(s_nav_temp, sizeof(s_nav_temp), "%s", temp);
      virtual_screen().startWrite();
      draw_temp_nav_app(virtual_screen());
      virtual_screen().endWrite();
    }
    return;
  }
  char e[64], v[64], junto[128];
  montar_servicos(e, sizeof(e), v, sizeof(v));
  snprintf(junto, sizeof(junto), "%s|%s|%s", e, v, temp);
  if (strcmp(junto, s_nav_txt) != 0) {
    snprintf(s_nav_txt, sizeof(s_nav_txt), "%s", junto);
    virtual_screen().startWrite();
    draw_servicos(virtual_screen(), e, v);
    virtual_screen().endWrite();
  }
}

// O QUE O BOTAO DA FAIXA FAZ, e o drawing segue. Hoje ele so tem one work
// - close o app e voltar a screen inicial - mas o simbolo exit daqui, e nao of
// one text cravado no drawing: no dia em que o botao fizer outra coisa (one app
// com niveis voltando one level, por exemplo), muda esta funcao e o tratamento
// of touch, e o drawing acompanha sozinho.
typedef enum {
  NAV_INICIO,   // circulo: a screen inicial (o "home" of Android)
  NAV_VOLTAR,   // triangulo para a left: one passo para tras
} nav_acao_t;

static nav_acao_t nav_acao(void)
{
  return NAV_INICIO;
}

// Os simbolos que todo celular usa, sem text: cabem em qualquer idioma e
// leem antes of qualquer palavra. Traco of 3 px, in tinta of botao.
static void draw_icon_nav(lgfx::LovyanGFX& d, int cx, int cy, nav_acao_t acao,
                               uint16_t tinta, uint16_t fundo)
{
  if (acao == NAV_VOLTAR) {
    d.fillTriangle(cx - 13, cy, cx + 10, cy - 14, cx + 10, cy + 14, tinta);
    d.fillTriangle(cx - 7, cy, cx + 7, cy - 9, cx + 7, cy + 9, fundo);
  } else {
    d.fillCircle(cx, cy, 14, tinta);
    d.fillCircle(cx, cy, 11, fundo);
  }
}

void draw_barra_nav(lgfx::LovyanGFX& d)
{
  const int L = d.width();
  const int y = d.height() - ALTURA_NAV;

  tab5_screen_clear(0, y, L, ALTURA_NAV, TFT_BLACK);
  // MARCA. O desenhar_servicos logo abaixo pede a mesma limpeza - e pede sem
  // condicao, como deve. Sem esta marca a faixa era limpa DUAS vezes in home:
  // 276KB onde 138 bastavam, in transicao mais pesada que existe.
  tab5_screen_mark_clean(0, y, L, ALTURA_NAV, TFT_BLACK);
  d.drawFastHLine(0, y, L, SYSTEM_LINE);

  // NA HOME, A FAIXA CONTA O QUE ESTA LIGADO NO APARELHO.
  //
  // Sem app open nao ha "INICIO" para draw e a faixa ficava vazia. A
  // barra of CIMA ja responde "como esta o aparelho" - battery, temperatura,
  // wifi. O que ela nao responde e "o que esta enabled nele", e essa e a
  // pergunta que custou time nas ultimas sessoes: o Grove trocou of modulo e
  // ninguem avisou, o cartao dizia "sem cartao" com cartao dentro.
  //
  // Tudo daqui exit of CACHE - none sondagem, none montagem of cartao, nada
  // que va on I2C. A faixa e desenhada junto of resto of home, e o caminho of
  // drawing nao e lugar of ir on hardware.
  if (!s_app) {
    char e[64], v[64], temp[16];
    montar_servicos(e, sizeof(e), v, sizeof(v));
    temp_chave(temp, sizeof(temp));
    snprintf(s_nav_txt, sizeof(s_nav_txt), "%s|%s|%s", e, v, temp);
    draw_servicos(d, e, v);
    return;
  }

  if (s_app) {
    // Travado: drawn apagado, para o olho saber que nao adianta touch.
    const uint16_t fundo = s_nav_travada ? (uint16_t)TFT_BLACK : (uint16_t)SYSTEM_BACKGROUND;
    const uint16_t tinta = s_nav_travada ? (uint16_t)SYSTEM_LINE  : (uint16_t)TFT_WHITE;
    d.fillRoundRect(L / 2 - NAV_BTN_L / 2, y + NAV_BTN_Y0, NAV_BTN_L, NAV_BTN_A, 16, fundo);
    d.drawRoundRect(L / 2 - NAV_BTN_L / 2, y + NAV_BTN_Y0, NAV_BTN_L, NAV_BTN_A, 16, SYSTEM_LINE);
    draw_icon_nav(d, L / 2, y + NAV_BTN_Y0 + NAV_BTN_A / 2, nav_acao(), tinta, fundo);
    temp_chave(s_nav_temp, sizeof(s_nav_temp));
    draw_temp_nav_app(d);
  }
}

// RESERVA ABAIXO DO ICONE, para o rotulo (name of app, ver desenhar_home) -
// era "60" cravado no middle of conta of py. Reduzida para 45: a
// 9 apps (3x3 exato) nunca sobrava uma 4a line para testar o limit; o app
// Camera virou o 10o e criou uma 4a line com UM icon so, cujo ROTULO
// (drawn 12px abaixo of icon, ver mais adiante) encostava in faixa of
// GROVE/CARTAO - o icon em si passava no teste of "cabe" (line abaixo),
// mas o text of name ficava fora of conta. A folga entre o rotulo of uma
// line e o icon of next era generosa (~50px sobrando) - os 15px a menos
// aqui saem dessa folga, nao of respiro em iteration of icon nem of rotulo.
static const int ICONE_VAO_LINHA = 45;

// Faixa embaixo dos icones para as bolinhas of pagina.
static const int HOME_BOLINHAS_A = 44;

static void celula_of_icon(int i, const area_t& a, int* px, int* py)
{
  const int col = i % ICONE_COLS;
  const int lin = i / ICONE_COLS;
  const int vao = (a.width - ICONE_COLS * ICONE_LADO) / (ICONE_COLS + 1);
  *px = a.x + vao + col * (ICONE_LADO + vao);
  *py = a.y + vao + lin * (ICONE_LADO + ICONE_VAO_LINHA + vao);
}

// ---------------------------------------------------------------------------
// PAGINAS DA HOME
// ---------------------------------------------------------------------------
// A pagina p mostra os apps [p*por_pagina, (p+1)*por_pagina). celula_do_icone()
// continua recebendo o indice DENTRO of pagina, entao drawing e touch (que
// usam a mesma funcao) seguem sempre of acordo. Nada of apps.cpp muda: o
// numero of paginas exit apenas dos aplicativos visiveis.
static int s_pagina_home = 0;

static int home_por_pagina(const area_t& a)
{
  int lines = 0;
  while (lines < TAB5_HOME_LINHAS_MAX) {
    int px, py;
    celula_of_icon(lines * ICONE_COLS, a, &px, &py);
    if (py + ICONE_LADO + 40 > a.y + a.height - HOME_BOLINHAS_A) { break; }
    ++lines;
  }
  return (lines < 1 ? 1 : lines) * ICONE_COLS;
}

static int home_n(void);

static int home_paginas(const area_t& a)
{
  const int pp = home_por_pagina(a);
  return (home_n() + pp - 1) / pp;
}

static bool home_mudar_pagina(int delta)
{
  const int n = home_paginas(area_t(system_area()));
  int nova = s_pagina_home + delta;
  if (nova < 0 || nova >= n) { return false; }
  s_pagina_home = nova;
  s_dirty_layout = true;   // changed a POSICAO dos icones: repintura complete
  return true;
}

// ---------------------------------------------------------------------------
// ORDEM DOS ICONES (ver sistema_home_app em app.h)
// ---------------------------------------------------------------------------
// s_ordem[pos] = indice em SISTEMA_APPS. Desenho e touch leem daqui; so
// move/carregar escrevem, e sempre dentro of s_ordem_mux - o console mexe of
// outra tarefa. Ler sem trava e seguro: cada entrada e one byte, e o pior caso
// e one frame com a order velha, logo repintado (move pede redraw).
#define HOME_APPS_MAX 64
static uint8_t s_order[HOME_APPS_MAX];
static int     s_n_order = 0;
static portMUX_TYPE s_order_mux = portMUX_INITIALIZER_UNLOCKED;

static int home_n(void) { return s_n_order ? s_n_order : SISTEMA_NUM_APPS; }

const app_t* system_home_app(int pos)
{
  if (pos < 0 || pos >= home_n()) { return nullptr; }
  return SISTEMA_APPS[s_n_order ? s_order[pos] : pos];
}

int system_home_position(const app_t* app)
{
  for (int i = 0; i < home_n(); ++i) { if (system_home_app(i) == app) { return i; } }
  return -1;
}

// Um name inteiro numa lista separada por virgulas. A comparacao sem distinguir
// maiusculas segue a of order dos icones.
static bool name_in_lista(const char* lista, const char* name)
{
  const size_t name_n = strlen(name);
  const char* p = lista ? lista : "";
  while (*p) {
    const char* end = strchr(p, ',');
    const size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n == name_n && strncasecmp(p, name, n) == 0) { return true; }
    if (!end) { break; }
    p = end + 1;
  }
  return false;
}

// Monta a order a partir dos nomes: os conhecidos e visiveis in order dada
// (repetido vale uma vez), e depois os visiveis que faltaram in order of
// apps.cpp. Ajustes nunca some, mesmo se uma NVS antiga ou editada o listar.
static void order_of_nomes(const char* nomes, const char* ocultos)
{
  uint8_t nova[HOME_APPS_MAX];
  bool usado[HOME_APPS_MAX] = {};
  const int total = SISTEMA_NUM_APPS < HOME_APPS_MAX ? SISTEMA_NUM_APPS : HOME_APPS_MAX;
  if (SISTEMA_NUM_APPS > HOME_APPS_MAX) {
    ESP_LOGE(TAG, "%d apps, a home so ordena %d (HOME_APPS_MAX)", SISTEMA_NUM_APPS, HOME_APPS_MAX);
  }
  int n = 0;
  const char* p = nomes ? nomes : "";
  while (*p) {
    const char* end = strchr(p, ',');
    const size_t len = end ? (size_t)(end - p) : strlen(p);
    for (int i = 0; i < total; ++i) {
      const char* name = SISTEMA_APPS[i]->name;
      const bool oculto = strcasecmp(name, "Settings") != 0 && name_in_lista(ocultos, name);
      if (!oculto && !usado[i] && strlen(name) == len && strncasecmp(name, p, len) == 0) {
        usado[i] = true; nova[n++] = (uint8_t)i; break;
      }
    }
    if (!end) { break; }
    p = end + 1;
  }
  for (int i = 0; i < total; ++i) {
    const char* name = SISTEMA_APPS[i]->name;
    const bool oculto = strcasecmp(name, "Settings") != 0 && name_in_lista(ocultos, name);
    if (!usado[i] && !oculto) { nova[n++] = (uint8_t)i; }
  }

  taskENTER_CRITICAL(&s_order_mux);
  memcpy(s_order, nova, n);
  s_n_order = n;
  taskEXIT_CRITICAL(&s_order_mux);
}

// A order usa o mesmo buffer para gravar e carregar; a lista of ocultos stays
// on lado porque tambem e usada on restaurar a order of fabrica.
static char s_order_nomes[1024];
static char s_ocultos_nomes[1024];

static void order_gravar(void)
{
  char* const nomes = s_order_nomes;
  const size_t size = sizeof(s_order_nomes);
  size_t k = 0;
  nomes[0] = 0;
  for (int i = 0; i < home_n(); ++i) {
    const char* name = system_home_app(i)->name;
    const size_t len = strlen(name);
    if (k + len + 2 > size) { break; }
    if (k) { nomes[k++] = ','; }
    memcpy(nomes + k, name, len);
    k += len;
    nomes[k] = 0;
  }
  settings_set_apps_order(nomes);
}

void home_ordem_carregar(void)
{
  settings_apps_order(s_order_nomes, sizeof(s_order_nomes));
  settings_apps_hidden(s_ocultos_nomes, sizeof(s_ocultos_nomes));
  order_of_nomes(s_order_nomes, s_ocultos_nomes);
  s_pagina_home = 0;
}

// Tira o app of 'of' e encaixa em 'para'; os of middle andam uma casa.
static void order_move(int of, int para)
{
  taskENTER_CRITICAL(&s_order_mux);
  if (s_n_order && of >= 0 && of < s_n_order && para >= 0 && para < s_n_order && of != para) {
    const uint8_t v = s_order[of];
    if (of < para) { memmove(&s_order[of], &s_order[of + 1], para - of); }
    else           { memmove(&s_order[para + 1], &s_order[para], of - para); }
    s_order[para] = v;
  }
  taskEXIT_CRITICAL(&s_order_mux);
}

bool system_home_move(const app_t* app, int pos)
{
  const int of = system_home_position(app);
  if (of < 0 || pos < 0 || pos >= home_n()) { return false; }
  order_move(of, pos);
  order_gravar();
  system_request_redraw_complete();
  return true;
}

void system_home_order_default(void)
{
  order_of_nomes("", s_ocultos_nomes);
  settings_set_apps_order("");
  system_request_redraw_complete();
}

void system_home_apps_reload(void)
{
  home_ordem_carregar();
  system_request_redraw_complete();
}

// Que position (in order) esta sob o finger, in pagina current; -1 = none.
// Sem folga e o quadrado of icon (o click). Com folga a celula inteira,
// rotulo e middle vao incluidos, e uma celula vazia no end of ultima pagina
// vale a ultima position - e o alvo of arraste, que nao pode cair em buraco.
static int home_pos_em(const area_t& a, int tx, int ty, bool folga)
{
  const int por_pagina = home_por_pagina(a);
  const int first = s_pagina_home * por_pagina;
  const int vao = (a.width - ICONE_COLS * ICONE_LADO) / (ICONE_COLS + 1);
  const int fx = folga ? vao / 2 : 0;
  const int fy_bottom = folga ? 40 + vao / 2 : 0;
  for (int k = 0; k < por_pagina; ++k) {
    int px, py;
    celula_of_icon(k, a, &px, &py);
    if (tx >= px - fx && tx < px + ICONE_LADO + fx && ty >= py - fx && ty < py + ICONE_LADO + fy_bottom) {
      const int pos = first + k;
      if (pos < home_n()) { return pos; }
      return folga ? home_n() - 1 : -1;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// MOVER ICONE (hold e drag) - ver home_segurar em internal.hpp
// ---------------------------------------------------------------------------
// Repinta a home so when o icon TROCA of casa, nunca a cada sample of
// finger: cada repintura complete sao ~70KB por icon (ver desenhar_home).
// Para outra pagina: hold o finger in beirada lateral por HOME_BORDA_MS.
#define HOME_SEGURAR_MS 600
#define HOME_BORDA_PX   60
#define HOME_BORDA_MS   700

static int      s_move = -1;          // position of icon levantado; -1 = nenhum
static bool     s_move_changed = false;
static uint32_t s_borda_since = 0;

bool home_segurar(int tx, int ty, uint32_t now, uint32_t segurando_ms, int desvio)
{
  const area_t a = system_area();
  ty -= TOQUE_DY;   // a mesma correcao of click (tratar_toque)

  if (s_move < 0) {
    if (segurando_ms < HOME_SEGURAR_MS || desvio > ROLAGEM_ARRASTE_MIN) { return false; }
    const int pos = home_pos_em(a, tx, ty, false);
    if (pos < 0) { return false; }
    s_move = pos;
    s_move_changed = false;
    s_borda_since = 0;
    s_dirty_layout = true;
    ESP_LOGI(TAG, "movendo '%s' (posicao %d)", system_home_app(pos)->name, pos + 1);
    return true;
  }

  const int paginas = home_paginas(a);
  const int lado = (tx < a.x + HOME_BORDA_PX && s_pagina_home > 0) ? -1
                 : (tx >= a.x + a.width - HOME_BORDA_PX && s_pagina_home < paginas - 1) ? +1 : 0;
  if (lado) {
    if (!s_borda_since) { s_borda_since = now; return true; }
    if (now - s_borda_since < HOME_BORDA_MS) { return true; }
    s_borda_since = now;   // segurando ali, a next pagina vem depois of outro prazo
    const int pp = home_por_pagina(a);
    s_pagina_home += lado;
    // Entra pela beirada of onde veio: indo para a right, primeira casa.
    int para = (lado > 0) ? s_pagina_home * pp : (s_pagina_home + 1) * pp - 1;
    if (para >= home_n()) { para = home_n() - 1; }
    order_move(s_move, para);
    s_move = para;
    s_move_changed = true;
    s_dirty_layout = true;
    return true;
  }
  s_borda_since = 0;

  const int pos = home_pos_em(a, tx, ty, true);
  if (pos >= 0 && pos != s_move) {
    order_move(s_move, pos);
    s_move = pos;
    s_move_changed = true;
    s_dirty_layout = true;
  }
  return true;
}

bool home_soltar(void)
{
  if (s_move < 0) { return false; }
  if (s_move_changed) {
    ESP_LOGI(TAG, "'%s' agora na posicao %d", system_home_app(s_move)->name, s_move + 1);
    order_gravar();
  }
  s_move = -1;
  s_dirty_layout = true;
  return true;
}

static void draw_bolinhas(lgfx::LovyanGFX& d, const area_t& a)
{
  const int n = home_paginas(a);
  if (n < 2) { return; }
  const int passo = 32;
  const int x0 = a.x + a.width / 2 - (n - 1) * passo / 2;
  const int y  = a.y + a.height - HOME_BOLINHAS_A / 2;
  for (int i = 0; i < n; ++i) {
    if (i == s_pagina_home) { d.fillCircle(x0 + i * passo, y, 9, TFT_WHITE); }
    else                    { d.fillCircle(x0 + i * passo, y, 7, SYSTEM_LINE); }
  }
}

void draw_home(lgfx::LovyanGFX& d, const area_t& a)
{
  // PAPEL DE PAREDE (Ajustes > Tela). So nesta pass - a home e estatica e
  // so e desenhada inteira, com a area recem-limpa. A semente e tirada uma
  // vez por boot - e of novo when o user iteration a escolher "Sorteio" -,
  // entao o drawing nao muda a cada iteration para a home.
  static uint32_t s_semente_parede = 0;
  static uint8_t  s_parede_ant = 0xFF;
  const uint8_t parede = settings_wallpaper();
  if (parede == SETTINGS_WALLPAPER_RANDOM && s_parede_ant != SETTINGS_WALLPAPER_RANDOM) { s_semente_parede = 0; }
  s_parede_ant = parede;
  while (!s_semente_parede) { s_semente_parede = esp_random(); }
  if (parede != SETTINGS_WALLPAPER_NONE) {
    const int estilo = (parede == SETTINGS_WALLPAPER_RANDOM)
                     ? (int)(s_semente_parede % SETTINGS_WALLPAPER_STYLE_COUNT) : parede - 1;
    bq_fundo_parede(d, a, estilo, s_semente_parede);
  }

  const int por_pagina = home_por_pagina(a);
  if (s_pagina_home >= home_paginas(a)) { s_pagina_home = 0; }
  const int first = s_pagina_home * por_pagina;
  // +40 cobre o rotulo (12px of respiro + a height of text of name) - sem
  // isto o icon passava no teste mas o name dele vazava por bottom, sem
  // nenhum aviso ate alguem notar in screen of verdade (foi assim que o
  // ICONE_VAO_LINHA acima precisou ser reduzido).
  for (int k = 0; k < por_pagina && first + k < home_n(); ++k) {
    int px, py;
    celula_of_icon(k, a, &px, &py);

    // RESPIRO ENTRE ICONES, e ele mora AQUI e nao no icon of cada app.
    //
    // Cada icon tem 190x190 = 70KB, que passa por bottom of faixa of 96KB e
    // nunca e fatiado. Oito seguidos sao 564KB numa rajada continua - a maior
    // escrita sem pausa of sistema, e justamente in saida para a home.
    //
    // O loop e of sistema, entao a pausa e of sistema: quem escreve app desenha
    // UM icon e nao tem como esquecer o que nao lhe pertence. Por orcamento e
    // nao por iteration - assim o mesmo code serve se one dia o icon mudar of
    // tamanho ou a home passar a ter vinte apps.

    const app_t* app = system_home_app(first + k);
    // O ICONE LEVANTADO (hold e drag): moldura branca em iteration, no vao
    // entre icones - o drawing of icon em si nao muda.
    if (first + k == s_move) {
      for (int e = 0; e < 4; ++e) {
        d.drawRoundRect(px - 10 + e, py - 10 + e, ICONE_LADO + 20 - 2 * e, ICONE_LADO + 20 - 2 * e, 34, TFT_WHITE);
      }
    }
    if (app->icon) {
      app->icon(d, px, py, ICONE_LADO);
    } else {
      d.fillRoundRect(px, py, ICONE_LADO, ICONE_LADO, 28, app->color);
      char inicial[2] = { app->name[0], 0 };
      tab5_text(d, 6.25f);   // 6.25 = escala real 2.0
      d.setTextDatum(middle_center);
      d.setTextColor(TFT_BLACK, app->color);
      d.drawString(inicial, px + ICONE_LADO / 2, py + ICONE_LADO / 2);
    }

    // NOME DO APP NA ORBITRON: Orbitron so in
    // "interface" (barra of kernel + home), DejaVu dentro of qualquer app. A
    // home e a segunda metade dessa regra. Nasceu em tamanho 1 (nativo, ver o
    // aviso sobre fonte bitmap + escala fracionaria em tab5_ui.hpp); grande demais, reduzido uns 25% -> 0.75.
    tab5_titulo(d, 30);
    d.setTextDatum(top_center);
    // SEM COR DE FUNDO: com ela o name saia numa tarja preta por top of
    // papel of parede. Sem, a borda suave of letra se mistura com o que ja
    // esta in screen - aqui e seguro, porque o name e pintado uma vez so por
    // pass, sobre a area recem-pintada (nunca engrossa).
    d.setTextColor(TFT_WHITE);

    // LARGURA MEDIDA DE VERDADE, nao suposta (nomes vizinhos encostavam).
    // "CreazySnake" (11
    // caracteres) e bem mais comprido que qualquer outro name of app, e o
    // rotulo nunca tinha sido testado contra a width disponivel - podia
    // exit encostando no icon vizinho, comendo a folga que separa os dois.
    // ESCALA-SE PARA BAIXO em vez of CORTAR o text: one name truncado
    // ("CreazyS...") deixa of identificar o app, uma fonte one pouco menor
    // ainda identifica. O piso of 0,8 evita este unico app ficar visivelmente
    // menor que os outros - se nem no piso coubesse, o certo seria encurtar o
    // NOME of app, nao espremer a fonte ate ficar ilegivel.
    const int vao = (a.width - ICONE_COLS * ICONE_LADO) / (ICONE_COLS + 1);
    const int width_disponivel = ICONE_LADO + vao;   // meia folga of cada lado
    // Encolhe em ALTURAS NATIVAS, nunca por escala.
    int alt_name = 30;
    while ((int)d.textWidth(app->name) > width_disponivel && alt_name > 22) {
      alt_name -= 2;
      tab5_titulo(d, alt_name);
    }
    d.drawString(app->name, px + ICONE_LADO / 2, py + ICONE_LADO + 12);
    tab5_fonte_body(d);
  }
  draw_bolinhas(d, a);
}

// ---------------------------------------------------------------------------
// TELA DE BLOQUEIO
// ---------------------------------------------------------------------------
// v1, of proposito simples: hora grande, data, o aviso em destaque (a mesma
// fonte que a barra of status usa) e a dica of deslizar. Sem senha. Tudo
// que muda com o time compara com o cache antes of paint, como o resto of
// sistema - o clock e conferido a cada segundo e so repinta when o
// minuto vira.
static char     s_bq_hora[8]   = "";
static char     s_bq_aviso[4 * (NOTIF_RESUMO + 24)] = "";
static uint32_t s_bq_dia       = 0;

static void data_por_extenso(char* out, size_t n)
{
  static const char* const DIAS[]  = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                       "Thursday", "Friday", "Saturday" };
  static const char* const MESES[] = { "January", "February", "March", "April", "May", "June",
                                       "July", "August", "September", "October", "November", "December" };
  const uint32_t t = tab5_time_epoch();
  if (!t) { snprintf(out, n, " "); return; }
  struct tm lt;
  tab5_local_time(t, &lt);
  snprintf(out, n, "%s, %s %d", DIAS[lt.tm_wday % 7], MESES[lt.tm_mon % 12], lt.tm_mday);
}

// Os textos of screen of bloqueio ficam NA FRENTE of animacao.
//
// Cada text e pintado UMA vez (when muda) num sprite pequeno, com fundo
// preto e a fonte suave of sempre - o mesmo drawing of antes of animacao. A
// cada frame, so onde a animacao passou por top, o sprite e copiado para a
// screen com o PRETO TRANSPARENTE. Assim o text nunca depende of que ha in
// pagina of tras: paint text suave sem fundo faz o M5GFX LER os pixels of
// screen e misturar a borda das letras com eles, e isso engrossa a letra a cada
// repintura e a deixa feia.
//
// Caixas fixas, sem se sobrepor, com folga em iteration of text.
enum { BQ_HORA, BQ_DATA, BQ_AVISO, BQ_DICA, BQ_N };

static lgfx::LGFX_Sprite* s_bq_spr[BQ_N] = {};

// AVISOS NO BLOQUEIO: uma line por app, quantas couberem entre a data e a
// dica (ate 4). Deitado sobra espaco para uma so - a mais grave.
static const int BQ_AVISO_LINHA = 52;
static int bq_lines_aviso(const area_t& a)
{
  const int y_hora = a.y + a.height / 4;
  const int y_dica = a.y + a.height - 110;
  const int n = ((y_dica - 48) - (y_hora + 190)) / BQ_AVISO_LINHA;
  return n < 1 ? 1 : (n > 4 ? 4 : n);
}

static void bq_caixas(const area_t& a, bq_ret_t r[BQ_N])
{
  const int cx = a.x + a.width / 2;
  const int y_hora = a.y + a.height / 4;
  const int y_dica = a.y + a.height - 110;
  r[BQ_HORA]  = { (int16_t)(cx - 240), (int16_t)(y_hora - 92),  480, 170 };
  r[BQ_DATA]  = { (int16_t)(cx - 350), (int16_t)(y_hora + 78),  700, 64 };
  r[BQ_AVISO] = { (int16_t)(a.x + 10), (int16_t)(y_hora + 190), (int16_t)(a.width - 20),
                  (int16_t)(bq_lines_aviso(a) * BQ_AVISO_LINHA + 18) };
  r[BQ_DICA]  = { (int16_t)(cx - 200), (int16_t)(y_dica - 48),  400, 80 };
}

// Desenha o text 'i' em 't' (one sprite ou a screen), com (ox, oy) = canto of
// caixa em coordenadas of screen. Fundo preto, sempre.
static void bq_text(lgfx::LovyanGFX& t, const area_t& a, int i, const char* hora,
                     const char* data, const char* aviso, int ox, int oy)
{
  const int cx = a.x + a.width / 2 - ox;
  const int y_hora = a.y + a.height / 4 - oy;
  t.setTextDatum(middle_center);
  switch (i) {
    case BQ_HORA:
      tab5_fonte_body(t);
      tab5_text(t, 7.0f);    // 56 px, a maior height nativa of fonte of body; dobrada abaixo
      t.setTextSize(2);       // inteiro (nao fracionario): 1 pixel of fonte = 2x2 in screen
      t.setTextColor(TFT_WHITE, TFT_BLACK);
      t.drawString(hora, cx, y_hora);
      t.setTextSize(1);
      break;
    case BQ_DATA:
      tab5_fonte_body(t);
      tab5_text(t, 3.5f);
      t.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      t.drawString(data, cx, y_hora + 110);
      break;
    case BQ_AVISO: {
      // 'aviso' e uma line por app: o first caractere e o level ('0'..'3'),
      // o resto o text, e '\n' separa (montado em desenhar_bloqueio).
      tab5_fonte_body(t);
      tab5_text(t, 3.0f);
      int y = y_hora + 225;
      for (const char* p = aviso; *p; ) {
        const char* end = strchr(p, '\n');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n > 1) {
          char line[NOTIF_RESUMO + 24];
          snprintf(line, sizeof(line), "%.*s", (int)(n - 1), p + 1);
          t.setTextColor(color_of_level((notification_level_t)(p[0] - '0')), TFT_BLACK);
          t.drawString(line, cx, y);
          y += BQ_AVISO_LINHA;
        }
        if (!end) { break; }
        p = end + 1;
      }
    } break;
    default: {
      const int y_dica = a.y + a.height - 110 - oy;
      t.fillRoundRect(cx - 60, y_dica - 40, 120, 8, 4, SYSTEM_LINE);
      tab5_fonte_body(t);
      tab5_text(t, 3.0f);
      t.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      t.drawString("Swipe up", cx, y_dica);
    } break;
  }
}

static void bq_sprites_release(void)
{
  for (int i = 0; i < BQ_N; ++i) {
    delete s_bq_spr[i];
    s_bq_spr[i] = nullptr;
  }
}

// Cria os sprites (PSRAM, so enquanto locked). false = sem memoria: o
// text cai no drawing direto, com fundo preto.
static bool bq_sprites_create(const bq_ret_t cx[BQ_N])
{
  for (int i = 0; i < BQ_N; ++i) {
    auto* sp = new lgfx::LGFX_Sprite();
    if (sp) {
      sp->setColorDepth(16);
      sp->setPsram(true);
    }
    if (!sp || !sp->createSprite(cx[i].w, cx[i].h)) {
      delete sp;
      bq_sprites_release();
      return false;
    }
    s_bq_spr[i] = sp;
  }
  return true;
}

void bloquear_screen(void)
{
  s_locked   = true;
  s_panel_notices = false;   // a central nao sobrevive on bloqueio
  s_bq_hora[0]  = 0;   // forca o first drawing complete
  s_dirty_layout = true;
  bq_anim_sortear();
}

void lockscreen_on_enable_screen(void)
{
  if (s_locked)            { bq_anim_sortear(); }   // acordou ainda bloqueada: outra animacao
  else if (settings_lock_screen()) { bloquear_screen(); }
}

void desbloquear_screen(void)
{
  s_locked   = false;
  s_dirty_layout = true;   // iteration home/app: repintura complete, nav inclusive
  bq_sprites_release();
  bq_anim_release();
}

// Um frame of screen of bloqueio (~25 Hz): animacao on fundo, text in frente.
void draw_bloqueio(lgfx::LovyanGFX& d, const area_t& a, bool complete)
{
  char hora[8], data[48], aviso[sizeof(s_bq_aviso)] = "";
  hora_now(hora, sizeof(hora));
  data_por_extenso(data, sizeof(data));

  // UM APP POR LINHA, e nao so o destaque: com Mesh e ESP-NOW avisando on
  // mesmo time, o bloqueio mostrava one e escondia o outro. A ultima line
  // que cabe vira "+N" when sobra app.
  notification_item_t lista[NOTIF_MAX_FONTES];
  const int n_av = notification_listar(lista, NOTIF_MAX_FONTES);
  const int cabem = bq_lines_aviso(a);
  size_t usado = 0;
  for (int i = 0; i < n_av && i < cabem; ++i) {
    char txt[NOTIF_RESUMO + 24];
    if (i == cabem - 1 && n_av > cabem) {
      snprintf(txt, sizeof(txt), "+%d outros avisos", n_av - i);
    } else if (lista[i].summary[0]) {
      snprintf(txt, sizeof(txt), "%s: %s", lista[i].name, lista[i].summary);
    } else {
      snprintf(txt, sizeof(txt), "%s: %d", lista[i].name, lista[i].count);
    }
    const int k = snprintf(aviso + usado, sizeof(aviso) - usado, "%c%s\n",
                           (char)('0' + (int)lista[i].level), txt);
    if (k < 0 || (size_t)k >= sizeof(aviso) - usado) { break; }
    usado += (size_t)k;
  }

  const bool hora_changed  = strcmp(hora, s_bq_hora) != 0;
  const bool notice_changed = strcmp(aviso, s_bq_aviso) != 0;
  snprintf(s_bq_hora, sizeof(s_bq_hora), "%s", hora);
  snprintf(s_bq_aviso, sizeof(s_bq_aviso), "%s", aviso);

  bq_ret_t caixa[BQ_N];
  bq_caixas(a, caixa);
  const bool changed[BQ_N] = { complete || hora_changed, complete || hora_changed, complete || notice_changed, complete };

  // Os sprites nascem (ou renascem) com o drawing complete e so sao pintados
  // of novo when o text muda.
  if (complete && !s_bq_spr[0]) { bq_sprites_create(caixa); }
  const lgfx::TextStyle estilo = d.getTextStyle();
  for (int i = 0; i < BQ_N; ++i) {
    if (!changed[i] || !s_bq_spr[i]) { continue; }
    s_bq_spr[i]->fillScreen(TFT_BLACK);
    bq_text(*s_bq_spr[i], a, i, hora, data, aviso, caixa[i].x, caixa[i].y);
  }

  // Texto que changed: a caixa e apagada (a animacao se refaz nela) e repintada.
  bq_ret_t extras[BQ_N];
  int n_extras = 0;
  if (complete) {
    bq_anim_start(a);
  } else {
    for (int i = 0; i < BQ_N; ++i) { if (changed[i]) { extras[n_extras++] = caixa[i]; } }
  }
  bq_ret_t clips[BQ_N];
  bq_anim_frame(d, extras, n_extras, caixa, BQ_N, clips);

  const uint16_t preto = 0;   // color transparente of sprite
  for (int i = 0; i < BQ_N; ++i) {
    bq_ret_t clip = clips[i];
    if (changed[i]) { clip = caixa[i]; }
    if (clip.w <= 0) { continue; }
    d.setClipRect(clip.x, clip.y, clip.w, clip.h);
    if (s_bq_spr[i]) {
      s_bq_spr[i]->pushSprite(&d, caixa[i].x, caixa[i].y, preto);
    } else {
      bq_text(d, a, i, hora, data, aviso, 0, 0);
    }
    d.clearClipRect();
  }
  d.setTextStyle(estilo);
  tab5_fonte_body(d);
}

// ---------------------------------------------------------------------------
// CENTRAL DE AVISOS
// ---------------------------------------------------------------------------
// A barra so tem ~170 px para notices in home (em pe): cabe UM summary e uns
// pontos coloridos. Com dois ou tres apps avisando, o user via que havia
// mais coisa e nao tinha onde read. Tocar a barra abre esta lista - a area
// util inteira, uma line por app (notif_listar), in mesma order of barra.
// Tocar uma line abre o app dela; touch fora, a barra of novo, Esc ou
// deslizar para top fecha. Enquanto ela esta open o app of bottom stays
// ENCOBERTO (sistema_app_encoberto): nao desenha, nao ouve, nao "le" nada.
bool s_panel_notices = false;

static notification_item_t s_pa_items[NOTIF_MAX_FONTES];   // o que esta NA TELA (o touch usa isto)
static int          s_pa_n = 0;
static int          s_pa_desenhadas = 0;             // quantas lines couberam
static uint32_t     s_pa_versao = 0;
static const int    PA_TOPO = 96, PA_VAO = 14, PA_MARGEM = 16;

// Deitado a area e baixa: line mais fina para caberem pelo menos tres.
static int pa_line(const area_t& a) { return a.height < 700 ? 96 : 124; }
static int pa_y(const area_t& a, int i) { return a.y + PA_TOPO + i * (pa_line(a) + PA_VAO); }

static const app_t* app_por_name(const char* name)
{
  if (!name) { return nullptr; }
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (strcmp(SISTEMA_APPS[i]->name, name) == 0) { return SISTEMA_APPS[i]; }
  }
  return nullptr;
}

void panel_notices_open(bool open)
{
  if (s_panel_notices == open) { return; }
  s_panel_notices = open;
  s_dirty_layout   = true;   // abre por top / devolve o app: pass complete
}

bool panel_notices_changed(void) { return s_panel_notices && notification_versao() != s_pa_versao; }

void draw_panel_notices(lgfx::LovyanGFX& d, const area_t& a, bool complete)
{
  // So in pass complete (area ja apagada). Um redraw of DADOS request
  // pelo app of bottom nao tem o que mudar aqui; aviso novo chega por
  // painel_avisos_mudou(), que pede a complete.
  if (!complete) { return; }
  s_pa_versao = notification_versao();
  s_pa_n = notification_listar(s_pa_items, NOTIF_MAX_FONTES);

  const int x0 = a.x + PA_MARGEM, w = a.width - 2 * PA_MARGEM;
  tab5_titulo(d, 28);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Notifications", x0 + 8, a.y + PA_TOPO / 2);
  tab5_fonte_body(d);
  tab5_text(d, 2.6f);
  d.setTextDatum(middle_right);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString("tap outside to close", x0 + w - 8, a.y + PA_TOPO / 2);

  const int h = pa_line(a);
  s_pa_desenhadas = 0;
  if (s_pa_n == 0) {
    tab5_text(d, 3.4f);
    d.setTextDatum(middle_center);
    d.drawString("No notifications", a.x + a.width / 2, a.y + a.height / 2);
  }
  for (int i = 0; i < s_pa_n; ++i) {
    const int y = pa_y(a, i);
    // A ultima vaga stays para o rodape "+N" when nao cabe everything.
    if (y + h > a.y + a.height - (i < s_pa_n - 1 ? 60 : 8)) { break; }
    const notification_item_t& it = s_pa_items[i];
    const uint16_t color = color_of_level(it.level);
    d.fillRoundRect(x0, y, w, h, 18, SYSTEM_BACKGROUND);
    d.fillRoundRect(x0, y, 14, h, 7, color);

    // Contagem a right, in color of gravidade (como a pastilha of barra).
    char num[8];
    if (it.count > 999) { snprintf(num, sizeof(num), "999+"); }
    else                     { snprintf(num, sizeof(num), "%d", it.count); }
    tab5_fonte_body_negrito(d);
    tab5_text(d, 3.0f);
    const int bl = (int)d.textWidth(num) + 28, ba = 44;
    const int bx = x0 + w - 20 - bl;
    d.fillRoundRect(bx, y + h / 2 - ba / 2, bl, ba, ba / 2, color);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_BLACK, color);
    d.drawString(num, bx + bl / 2, y + h / 2);

    // Quem (negrito) e o que (summary). Quem abre outro app ganha a seta.
    const bool abre = app_por_name(it.app) != nullptr;
    char quem[40];
    snprintf(quem, sizeof(quem), abre ? "%s  >" : "%s", it.name);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_WHITE, SYSTEM_BACKGROUND);
    tab5_text(d, 3.2f);
    d.drawString(quem, x0 + 34, y + h * 32 / 100);
    tab5_fonte_body(d);
    tab5_text(d, 3.0f);
    d.setTextColor(TFT_LIGHTGREY, SYSTEM_BACKGROUND);
    d.drawString(it.summary[0] ? it.summary : "", x0 + 34, y + h * 70 / 100);
    ++s_pa_desenhadas;
  }
  if (s_pa_desenhadas < s_pa_n) {
    char mais[32];
    snprintf(mais, sizeof(mais), "+%d avisos", s_pa_n - s_pa_desenhadas);
    tab5_text(d, 3.0f);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(mais, a.x + a.width / 2, a.y + a.height - 30);
  }
  d.setTextDatum(top_left);
  tab5_fonte_body(d);
}

// Toque com a central open ('ty' ja corrigido). Linha = abre o app dela;
// qualquer outro lugar = fecha.
static void panel_notices_touch(const area_t& a, int tx, int ty)
{
  const int h = pa_line(a);
  const app_t* destino = nullptr;
  for (int i = 0; i < s_pa_desenhadas; ++i) {
    const int y = pa_y(a, i);
    if (ty >= y && ty < y + h && tx >= a.x + PA_MARGEM && tx < a.x + a.width - PA_MARGEM) {
      destino = app_por_name(s_pa_items[i].app);
      break;
    }
  }
  panel_notices_open(false);
  // O app que ja esta open so reaparece - close e open of novo perderia
  // o que ele estava mostrando.
  if (destino && destino != s_app) { open_app(destino); }
}

// ---------------------------------------------------------------------------
// GESTOS
// ---------------------------------------------------------------------------
static const int GESTO_PAGINA_PX     = 120;   // arraste lateral minimum para switch of pagina
static const int GESTO_DESBLOQUEIO_PX = 220;  // deslize para top minimum para desbloquear

void tratar_gesto(int dx, int dy)
{
  if (s_locked) {
    if (dy < -GESTO_DESBLOQUEIO_PX && abs(dy) > 2 * abs(dx)) { desbloquear_screen(); }
    return;
  }
  if (s_panel_notices) {
    if (dy < -GESTO_PAGINA_PX && abs(dy) > 2 * abs(dx)) { panel_notices_open(false); }
    return;
  }
  if (s_app) { return; }   // dentro of one app o arraste e dele
  if (abs(dx) > GESTO_PAGINA_PX && abs(dx) > 2 * abs(dy)) {
    home_mudar_pagina(dx < 0 ? +1 : -1);   // drag para a left avanca
  }
}

// ---------------------------------------------------------------------------
// SPLASH DE BOOT - "M5Tab OS" grande no middle of screen.
// ---------------------------------------------------------------------------
// #ifdef DE PROPOSITO, nao uma preferencia em runtime:
// durante o desenvolvimento normal a gente reinicia o aparelho o time todo
// para testar uma mudanca, e one splash of 1,4s a cada boot vira atrito puro
// sem agregar nada - o user ja SABE que e o M5tabOS. A chave mora em
// kernel/internal.hpp (TAB5_MOSTRAR_SPLASH) porque core.cpp precisa ver a
// MESMA definicao para decidir se chama esta funcao.
//
// MORA NO SHELL, e a CHAMADA mora em core.cpp, depois of
// tab5_power_dpi_clock() - defeito observado: o splash piscava e sumia
// antes dos apps. Causa: draw antes daquela chamada
// significa draw antes of panel assumir o clock final, e a troca of
// clock DESMONTA E REMONTA o DSI, apagando o que estiver in screen.
#ifdef TAB5_MOSTRAR_SPLASH
void draw_splash(void)
{
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);

  // FONTE PROPRIA DO SPLASH, DE PROPOSITO DIFERENTE of tab5_fonte_titulo()
  // current.
  // TODO titulo of sistema (barra, rotulo of home, secao
  // dentro of one app) era a Orbitron_Light_32; naquele dia ela foi trocada
  // por FreeMonoBold12pt7b em tab5_fonte_titulo() (ver o comentario grande
  // em tab5_ui.hpp) e passou a valer em todo lugar - MENOS aqui: o splash
  // continua com a Orbitron of proposito, e por isso NAO chama
  // tab5_fonte_titulo() como o resto of sistema.
  //
  // TAMANHO 2 (dobro of nativo), NAO one valor fracionario tipo 1.5 - request
  // foi "uns 150% maior" (ou seja, 2,5x), mas fonte bitmap-por-
  // glifo como esta so pode escalar por MULTIPLO INTEIRO sem descartar/
  // duplicar line of pixel of one jeito irregular (ver o aviso grande sobre
  // isso em tab5_ui.hpp - foi exatamente esse defeito que fez "CONFIG" in
  // barra parecer outra fonte). Entre o 1x of antes (pequeno demais, reason
  // deste request) e o 3x (passaria of 500px so no "M5Tab OS", too largo para
  // o request of "uns 150%"), 2x e o degrau limpo mais next.
  d.setFont(&fonts::Orbitron_Light_32);
  d.setTextSize(2);
  d.setTextDatum(middle_center);

  // NEGRITO FALSO, - a Orbitron_Light e
  // "light" por DESENHO (ver o aviso ali em top), e a biblioteca so tem essa
  // variante dela (none bold, em nenhum tamanho) - nao ha fonte pronta
  // mais grossa para switch.
  //
  // SEM COR DE FUNDO aqui (so one argumento em setTextColor = so estampa
  // pixel branco, nunca apaga o que ja esta atras) - a screen inteira ja
  // nasceu preta no fillScreen() ali em top, entao nao falta nada para
  // erase. Desenhando a MESMA string 5x, deslocada 1px para cada lado
  // (cruz: centro + top/bottom/left/right), cada copia so ACRESCENTA
  // pixel branco - o traco engorda em vez of borrar.
  //
  // NO TAMANHO 1x (nativo) isto borraria em vez of engordar - foi o achado
  // documentado in Orbitron of barra of status (tab5_ui.hpp): la, 1px of
  // screen E one pixel of fonte INTEIRO, entao scroll por 1 e pular para o
  // pixel vizinho, nao dividir o mesmo. Aqui a fonte ja esta em 2x: 1px of
  // screen e so METADE of one pixel of fonte, fino o bastante para engrossar a
  // borda sem scroll o drawing inteiro para one vizinho visivel.
  const int cx = d.width() / 2, cy = d.height() / 2;
  d.setTextColor(TFT_WHITE);
  d.drawString("M5Tab OS", cx,     cy);
  d.drawString("M5Tab OS", cx - 1, cy);
  d.drawString("M5Tab OS", cx + 1, cy);
  d.drawString("M5Tab OS", cx,     cy - 1);
  d.drawString("M5Tab OS", cx,     cy + 1);

  // Devolve o estado global of text - MESMA pegadinha of datum/fonte vazando
  // documentada em tab5_botao()/tab5_ui.hpp. O loop principal of core.cpp,
  // logo depois, conta com top_left e a fonte of body, como sempre.
  d.setTextDatum(top_left);
  tab5_fonte_body(d);

  vTaskDelay(pdMS_TO_TICKS(1400));
}
#endif

// ---------------------------------------------------------------------------
// Toque
// ---------------------------------------------------------------------------
void tratar_touch(int tx, int ty)
{
  // AINDA MONTANDO O SD deste app: engole o touch. Nem a navigation ("INICIO")
  // nem o content existem of verdade ainda - ao_abrir() so roda when
  // concluir_abertura_do_sd() (core.cpp) liberar. Dura poucos frames in pratica.
  if (s_app_loading) { return; }
  // Bloqueado: so o deslize (tratar_gesto) desbloqueia; touch simples nao faz nada.
  if (s_locked) { return; }

  // sistema_area(), e nao a funcao privada area_util() of core.cpp - a mesma
  // geometria, so pela porta publica (ver o aviso em kernel/internal.hpp).
  area_t a = system_area();

  // Correcao aplicada UMA vez, in entrada. Daqui para bottom - barra, navigation,
  // icones of home, apps - todo mundo ja recebe o ponto corrigido, e nenhum
  // deles precisa saber que a correcao existe.
  const int ty_cru = ty;
  ty -= TOQUE_DY;

  // CENTRAL DE AVISOS: open, o touch e todo dela (inclusive in barra e in
  // navigation - qualquer touch fora of uma line so fecha). Fechada, touch a
  // barra of status a abre.
  if (s_panel_notices) { panel_notices_touch(a, tx, ty); return; }
  // NAVEGACAO TRAVADA vale para a barra tambem: uma line of central abre
  // outro app, e isso fecharia este por one caminho que a trava existe para
  // impedir (o Pendrive, com o disco montado no computador).
  if (ty < ALTURA_BARRA) { if (!s_nav_travada) { panel_notices_open(true); } return; }

  if (s_diag_touch) {
    // (a barra of status ja voltou acima: aqui so sobra navigation ou area)
    ESP_LOGI(TAG, "toque: cru (%d,%d) -> corrigido (%d,%d) -> app (%d,%d)%s",
             tx, ty_cru, tx, ty, tx - a.x, ty - a.y + s_rol.current,
             (ty >= M5.Display.height() - ALTURA_NAV) ? "  [navegacao]" : "");
  }

  if (ty >= M5.Display.height() - ALTURA_NAV) {
    // SO A REGIAO DO BOTAO fecha o app now.
    //
    // ATE AQUI, qualquer touch in faixa of navigation inteira (96px of
    // height, a LARGURA TODA of screen) fechava o app - so o Y era checado
    // acima, nunca o X. Na pratica isso nunca incomodou porque nao havia
    // mais nada alem of botao naquela faixa para touch por engano - mas era
    // uma area of touch bem maior que a pilula desenhada, e abre espaco para
    // colocar informacao NAO tocavel of lado dela sem essa informacao
    // roubar o touch.
    //
    // O rectangle abaixo e EXATAMENTE o que desenhar_barra_nav() pinta -
    // NAV_BTN_L/NAV_BTN_A/NAV_BTN_Y0, ver o aviso grande junto delas.
    const int nav_y0 = M5.Display.height() - ALTURA_NAV;
    const int L = M5.Display.width();
    //
    // AREA DE TOQUE MAIOR QUE O DESENHO. A pilula encolheu com a
    // faixa (72px) e became justa para o finger - errar por 10px acima ou abaixo
    // dela nao fazia nada. O touch now vale a ALTURA INTEIRA of faixa e
    // NAV_BTN_FOLGA a mais of cada lado; o drawing continua o mesmo. Nao sobe
    // para dentro of area of app: o que esta acima of faixa e of app.
    const bool dentro_of_botao =
        tx >= L / 2 - NAV_BTN_L / 2 - NAV_BTN_FOLGA && tx < L / 2 + NAV_BTN_L / 2 + NAV_BTN_FOLGA &&
        ty >= nav_y0;

    // NAVEGACAO TRAVADA: a faixa passa a ser of app (keyboard in screen). O
    // keyboard encosta in barra, e one finger que passa of fileira of bottom cai
    // aqui - entregue in ultima line of area, vale a key of bottom em vez of
    // sumir. Mesma regra of touch por level (core.cpp).
    if (s_nav_travada) {
      if (app_vivo() && s_app->on_touch) { s_app->on_touch(tx - a.x, a.height - 1 + s_rol.current); }
      return;
    }
    if (dentro_of_botao && s_app) { close_app(); }
    return;
  }

  if (s_app) {
    // ROLAGEM SOMADA AQUI, e so aqui. O app compara contra as MESMAS constantes
    // que usou para draw, sem saber of rolagem none.
    if (app_vivo() && s_app->on_touch) { s_app->on_touch(tx - a.x, ty - a.y + s_rol.current); }
    return;
  }

  const int pos = home_pos_em(a, tx, ty, false);
  if (pos >= 0) { open_app(system_home_app(pos)); }
}
