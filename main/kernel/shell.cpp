// O "shell" do M5tabOS: barra de status, tela inicial (launcher), roteamento
// de toque para o app certo, e a calibracao do toque.
//
// -------------------------------------------------------------------------
// ETAPA 1 do ARCHITECTURE.md - metade do SHELL (aparencia)
// -------------------------------------------------------------------------
// Ate aqui isto vivia dentro de kernel/sistema.cpp, junto do MOTOR (agenda
// de desenho, ciclo de vida dos apps, politica de energia) - 2457 linhas
// fazendo oito coisas diferentes. Partido em dois, sem mudar uma linha de
// logica (a unica excecao esta documentada em kernel/internal.hpp, junto da
// troca area_util() -> sistema_area()): core.cpp e o motor, este arquivo e
// a aparencia.
//
// "shell" no sentido de sistema operacional (Windows Shell, GNOME Shell): a
// camada visual em volta dos apps. NAO tem relacao com console.cpp (que e um
// shell de LINHA DE COMANDO, outro conceito, outro arquivo).
//
// UMA tarefa so manda no display e no I2C, de proposito - ver o aviso
// identico em core.cpp. Os apps nao desenham das proprias tarefas; a funcao
// desenhar() de cada um e chamada pelo motor, que mora no outro arquivo.

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
// GEOMETRIA DO BOTAO DA FAIXA (a pilula na faixa de navegacao) - NUM SO
// LUGAR, porque desenhar_barra_nav() PINTA neste retangulo
// e tratar_toque() agora PRECISA saber o mesmo retangulo para so fechar o
// app quando o toque cai dentro dele (antes, qualquer toque na faixa
// inteira de 96px - a LARGURA TODA da tela - fechava o app; so o Y era
// checado). Duas constantes divergentes aqui voltariam a abrir esse buraco.
static const int NAV_BTN_L  = 160;               // largura da pilula (era 180, junto com a faixa mais baixa)
static const int NAV_BTN_A  = ALTURA_NAV - 32;   // altura da pilula
static const int NAV_BTN_FOLGA = 48;            // toque alem da pilula, de cada lado (ver tratar_toque)
static const int NAV_BTN_Y0 = 16;                // deslocamento do topo da faixa ate o topo da pilula

static const int ICONE_LADO   = 190;
static const int ICONE_COLS   = 3;

// ---------------------------------------------------------------------------
// CORRECAO VERTICAL DO TOQUE
// ---------------------------------------------------------------------------
// O painel capacitivo devolve o CENTROIDE da area de contato, e o dedo nao
// toca a tela com a ponta que voce ve: ele toca com a polpa, alguns milimetros
// abaixo. O olho mira na borda visivel, o sensor mede o meio da mancha - e a
// leitura sai sistematicamente ABAIXO de onde voce achou que tocou.
//
// O sintoma e sempre o mesmo e foi exatamente o observado: "tenho que tocar mais
// para cima para acertar o botao". Nao e imprecisao do painel; e a diferenca
// entre onde voce mira e onde voce encosta.
//
// A 294 DPI deste painel, 10px sao ~0,85mm - dentro da faixa que os sistemas
// operacionais aplicam para a mesma coisa. Nao e chute cego: o comando `toque`
// do console mede o desvio real, e este numero existe para ser ajustado com
// aquele dado, nao com impressao.
//
// Fica no SHELL e nao nos apps de proposito: e propriedade do painel e do
// dedo, igual para todo mundo. Cada app corrigindo por conta seria oito
// correcoes diferentes para uma causa so. O valor (TOQUE_DY) mora em
// kernel/internal.hpp: o toque por nivel, em core.cpp, aplica o mesmo.

// Diagnostico do toque, ligado pelo console. Imprime onde o dedo caiu, cru e
// ja convertido - a unica forma de trocar "parece impreciso" por um numero.
static bool s_diag_toque = false;
bool diag_toque_ligado(void) { return s_diag_toque; }
void sistema_diag_toque(bool ligar)
{
  s_diag_toque = ligar;
  ESP_LOGI(TAG, "diagnostico de toque %s (correcao atual: %d px)",
           ligar ? "LIGADO" : "desligado", TOQUE_DY);
}

// Travado: desenhado apagado na barra de navegacao, para o olho saber que
// nao adianta tocar. Ver sistema_travar_navegacao() logo abaixo.
//
// NAO 'static' - core.cpp tambem escreve false direto em fechar_app() (rede
// de seguranca: um app fechado nao pode deixar a navegacao travada para o
// proximo). Declarada extern em kernel/internal.hpp.
bool s_nav_travada = false;
void sistema_travar_navegacao(bool t)
{
  s_nav_travada = t;
  if (t && s_painel_avisos) { painel_avisos_abrir(false); }   // aberta antes da trava: fecha
  s_sujo_layout = true;
}

// ---------------------------------------------------------------------------
// Icone de bateria
// ---------------------------------------------------------------------------

// Tres estados de alimentacao, nao dois. O erro anterior era detectar "carregando"
// so pela corrente positiva: a 100% o carregador para, a corrente vai a zero e o
// raio sumia, mesmo com o cabo na tomada.
//
// O discriminador e simples e confiavel: o sistema consome mais de 100mA. Se
// essa corrente NAO esta saindo da bateria, ela so pode estar vindo do USB.
typedef enum {
  EN_BATERIA,      // descarregando: corrente claramente negativa
  EN_CARREGANDO,   // corrente entrando na bateria
  EN_TOMADA        // alimentado por fora, bateria sem trocar carga (cheia)
} estado_energia_t;

static estado_energia_t estado_energia(const tab5_measurement_t& m)
{
  if (m.mA < -20.0f) { return EN_BATERIA;    }
  if (m.mA >  20.0f) { return EN_CARREGANDO; }
  return EN_TOMADA;
}

// Raio de carregamento, desenhado com contorno para ficar legivel tanto sobre a
// parte cheia quanto sobre a parte vazia da bateria.
static void desenhar_raio(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor)
{
  d.fillTriangle(cx + 2, cy - 10, cx - 7, cy + 2, cx + 1, cy + 2, cor);
  d.fillTriangle(cx - 2, cy + 10, cx + 7, cy - 2, cx - 1, cy - 2, cor);
}

// Plugue, para quando esta na tomada com a bateria ja cheia - o mesmo que o
// celular mostra quando termina de carregar e voce nao tirou o cabo.
//
// Dimensoes apertadas de proposito: ele desenha DENTRO da bateria, que tem 30px
// de altura e 3px de borda de cada lado. Sobram 24px, e este plugue ocupa 19.
static void desenhar_tomada(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor)
{
  d.fillRect(cx - 5, cy - 9, 2, 5, cor);           // pino esquerdo
  d.fillRect(cx + 3, cy - 9, 2, 5, cor);           // pino direito
  d.fillRoundRect(cx - 7, cy - 4, 14, 9, 2, cor);  // corpo
  d.fillRect(cx - 1, cy + 5, 2, 5, cor);           // cabo
}

// Bateria estilo celular: moldura, polo, preenchimento proporcional e o raio
// quando esta carregando.
static void desenhar_bateria(lgfx::LovyanGFX& d, int x, int y, int pct, estado_energia_t est)
{
  const int L = 58;              // largura do corpo
  const int A = 30;              // altura do corpo
  const int borda = 3;

  const int p = (pct < 0) ? 0 : (pct > 100 ? 100 : pct);

  const uint16_t cor = (est != EN_BATERIA) ? TFT_GREEN
                     : (p < 20)            ? TFT_RED
                     : (p < 50)            ? TFT_YELLOW
                                           : TFT_GREENYELLOW;

  // Corpo: interior apagado e depois preenchido ate a fracao da carga.
  // SIS_FUNDO e nao TFT_DARKGREY: este fillRect limpa a area do icone E pinta
  // a parte VAZIA da bateria. Com o cinza antigo sobrava um retangulo claro em
  // volta e dentro dela - um pedaco da barra velha que nao acompanhou a troca.
  d.fillRect(x, y, L + 6, A, SIS_FUNDO);                    // limpa a area toda
  d.drawRoundRect(x, y, L, A, 6, TFT_WHITE);
  d.drawRoundRect(x + 1, y + 1, L - 2, A - 2, 5, TFT_WHITE); // 2px de moldura
  d.fillRect(x + L + 1, y + A / 2 - 6, 5, 12, TFT_WHITE);    // polo positivo

  const int util = L - 2 * borda;
  const int cheio = (util * p) / 100;
  if (cheio > 0) {
    d.fillRect(x + borda, y + borda, cheio, A - 2 * borda, cor);
  }

  // Simbolo central. Mesma tecnica nos dois casos: contorno escuro deslocado de
  // 1px e figura clara por cima, para ficar legivel tanto sobre a parte cheia
  // quanto sobre a parte vazia da bateria.
  if (est == EN_CARREGANDO) {
    desenhar_raio(d, x + L / 2 + 1, y + A / 2 + 1, TFT_BLACK);
    desenhar_raio(d, x + L / 2,     y + A / 2,     TFT_WHITE);
  } else if (est == EN_TOMADA) {
    desenhar_tomada(d, x + L / 2 + 1, y + A / 2 + 1, TFT_BLACK);
    desenhar_tomada(d, x + L / 2,     y + A / 2,     TFT_WHITE);
  }

  // Se o modelo nao souber estimar (pct < 0), marca com um tracinho.
  if (pct < 0) {
    d.drawFastHLine(x + borda + 4, y + A / 2, util - 8, TFT_LIGHTGREY);
  }
}

// ---------------------------------------------------------------------------
// Desenho
// ---------------------------------------------------------------------------
// Icone de WiFi: quatro arcos concentricos, aceso conforme o sinal.
//
// Simbolo em vez de texto porque a barra e lida de relance: forma e cor chegam
// antes da leitura. O nome da rede continua disponivel no app Ajustes, que e
// onde voce vai quando quer detalhe.
//
// Faixas de RSSI (dBm), as usuais de WiFi:
//   >= -55 excelente (4) | >= -65 bom (3) | >= -75 ok (2) | >= -85 fraco (1)
// O X FICA AO LADO, E NAO POR CIMA.
//
// A versao anterior riscava o proprio leque com dois tracos grossos. Ficou
// legivel e ficou horrivel: o X brigava com os arcos pelo mesmo espaco e o
// simbolo virava um borrao. A convencao boa mantem o
// leque intacto e poe o X SOLTO no canto de cima, a direita.
//
// Para isso o leque anda um pouco para a esquerda e encolhe, abrindo o canto.
// O icone inteiro passa a ocupar ~46px de largura em vez de ~40.
// Termometro de ~14x31 (era 12x26, +20%junto da troca de
// fonte), desenhado a partir do centro do bulbo. Bulbo, haste e a coluna
// colorida dentro dela - o mesmo desenho serve para qualquer cor, entao a
// temperatura pinta o liquido e o vidro fica sempre claro.
//
// ICONE VETORIAL, NAO ACOMPANHA FONTE SOZINHO - por isso os literais abaixo
// sao os originais * 1.2 (arredondados), e nao uma conta em tempo de execucao:
// e um desenho fixo, chamado de um unico lugar, sem motivo para reprocessar
// escala a cada frame.
// A COR DA TEMPERATURA, continua: azul frio -> verde -> amarelo -> laranja ->
// vermelho. Antes eram tres faixas (azul ate 55, laranja, vermelho a 70) e o
// termometro passava a maior parte do dia no mesmo azul - nao contava nada.
// Os degraus seguem o que este chip faz de verdade: ~40 parado, ~50 com a tela
// e o radio ocupados, 55+ trabalhando, 70+ quente.
static uint16_t cor_da_temp(float c)
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

// A COLUNA SOBE COM A TEMPERATURA (20 a 80 C, a faixa do sensor) e o liquido
// toma a cor de cor_da_temp(). Sem leitura: vidro vazio, bulbo cinza.
// Ocupa de cy-20 a cy+12.
static void desenhar_termometro(lgfx::LovyanGFX& d, int cx, int cy, bool tem, float c,
                                uint16_t fundo)
{
  const uint16_t vidro   = TFT_LIGHTGREY;
  const uint16_t liquido = tem ? cor_da_temp(c) : (uint16_t)TFT_DARKGREY;

  // Haste: contorno claro com o miolo na cor do fundo, para o liquido ter onde
  // aparecer sem o vidro sumir.
  d.fillRoundRect(cx - 5, cy - 20, 10, 24, 5, vidro);
  d.fillRoundRect(cx - 2, cy - 18, 5, 22, 2, fundo);
  if (tem) {
    float f = (c - 20.0f) / 60.0f;
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    const int h = 2 + (int)(f * 20.0f + 0.5f);        // 2..22 px de coluna
    d.fillRect(cx - 2, cy + 4 - h, 5, h, liquido);
  }

  d.fillCircle(cx, cy + 5, 7, vidro);
  d.fillCircle(cx, cy + 5, 5, liquido);               // bulbo
}

static void desenhar_wifi(lgfx::LovyanGFX& d, int cx, int cy, uint16_t cor,
                          int barras, bool cortado)
{
  // Leque deslocado: o centro dele nao e mais o centro do icone.
  cx -= 7;

  // Desenhado como arcos: o ponto na base e mais tres coroas. fillArc cobre de
  // 225 a 315 graus, que e o leque de 90 graus apontando para cima.
  // Apagado em cinza ESCURO, e nao no TFT_DARKGREY de antes: com a barra agora
  // escura, aquele cinza medio ficava mais claro que o fundo e o leque "vazio"
  // chamava mais atencao que o cheio.
  const uint16_t off = 0x52AA;   /* rgb(82, 86, 82) */
  d.fillCircle(cx, cy + 9, 3, barras >= 1 ? cor : off);
  for (int n = 1; n <= 3; ++n)
  {
    const int r = 5 + n * 5;
    d.fillArc(cx, cy + 9, r, r - 2, 225, 315,
              (barras >= n + 1) ? cor : off);
  }

  if (cortado)
  {
    // POR CIMA da ponta do leque, comendo um pedaco dele - como no simbolo
    // usual de "sem conexao". Nao e decoracao: o X sobreposto diz que aquele leque esta
    // interrompido, enquanto um X ao lado dizia so "tem um X aqui tambem".
    //
    // 3px de espessura. A versao anterior tinha 4 mais um contorno preto de 6,
    // e o conjunto ficava mais pesado que o proprio leque que devia negar.
    const int ex = cx + 16;
    const int ey = cy - 4;
    const int b  = 6;
    tab5_linha_grossa(d, ex - b, ey - b, ex + b, ey + b, 3, TFT_RED);
    tab5_linha_grossa(d, ex + b, ey - b, ex - b, ey + b, 3, TFT_RED);
  }
}

// MODO ESP-NOW. Com o canal fixo, o WiFi solta o roteador DE
// PROPOSITO - o radio esta de pe, conversando por ESP-NOW. O leque cinza com X
// dizia "sem conexao", que e mentira: ha conexao, so que outra. Neste modo a
// barra mostra o simbolo do ESP-NOW (o mesmo do icone do app: ponto de origem
// e tres ondas saindo) - ciano no ar, cinza com o radio parado.
static void desenhar_espnow(lgfx::LovyanGFX& d, int cx, int cy, bool no_ar)
{
  const uint16_t cor = no_ar ? TFT_CYAN : (uint16_t)0x52AA;
  const int ox = cx - 9, oy = cy + 9;   // o ponto de origem, embaixo a esquerda
  d.fillCircle(ox, oy, 3, cor);
  for (int n = 1; n <= 3; ++n) {
    const int r = 3 + n * 6;
    d.fillArc(ox, oy, r, r - 2, 270, 360, cor);
  }
}

// BLUETOOTH NA BARRA: a runa, a esquerda do WiFi, so enquanto o
// Bluetooth esta ligado. Cor: azul = conectado a um no (Mesh BLE); cinza
// claro = ligado; cinza escuro = subindo; vermelho = falhou.
static int bt_visual(void)
{
  const tab5_ble_state_t e = tab5_ble_state();
  if (e == TAB5_BLE_OFF) { return 0; }
  if (e == TAB5_BLE_FAILED)    { return 3; }
  if (e == TAB5_BLE_STARTING || e == TAB5_BLE_STOPPING) { return 1; }
  return mesh_ble_conectado() ? 4 : 2;
}

static void desenhar_bt(lgfx::LovyanGFX& d, int cx, int cy, int v)
{
  const uint16_t cor = (v == 4) ? (uint16_t)TFT_SKYBLUE : (v == 3) ? (uint16_t)TFT_RED
                     : (v == 2) ? (uint16_t)TFT_LIGHTGREY : (uint16_t)0x52AA;
  const int t = cy - 12, b = cy + 12, dx = 7, q = 6;
  for (int k = 0; k <= 1; ++k) {
    d.drawLine(cx + k, t, cx + k, b, cor);
    d.drawLine(cx + k, t, cx + dx + k, t + q, cor);
    d.drawLine(cx + dx + k, t + q, cx - dx + k, b - q, cor);
    d.drawLine(cx + k, b, cx + dx + k, b - q, cor);
    d.drawLine(cx + dx + k, b - q, cx - dx + k, t + q, cor);
  }
}

// O que a barra MOSTRA do radio: o estado do WiFi, ou o modo ESP-NOW (100 no
// ar, 101 parado). Um numero so para o desenho e para o barra_mudou - os dois
// tem que concordar, senao trocar de modo nao repinta.
static int radio_visual(void)
{
  if (tab5_espnow_canal_fixo()) { return tab5_espnow_pronto() ? 100 : 101; }
  return (int)tab5_wifi_state();
}

static void hora_agora(char* out, size_t n);   // mais abaixo, junto da faixa de baixo

// Ultimo estado desenhado na barra, para nao repintar quando nada mudou.
static int  s_barra_temp = -999;  // ultimo grau INTEIRO desenhado (so na tela de bloqueio)
static char s_barra_hora[8] = ""; // a hora desenhada no canto (fora do bloqueio)
static bool s_barra_bloq = false; // o canto foi pintado como tela de bloqueio
static int  s_barra_pct  = -999;
static int  s_barra_wifi = -1;    // estado de WiFi com que a barra foi pintada
static int  s_barra_bt   = -1;    // estado de Bluetooth com que a barra foi pintada
static uint32_t s_barra_notif = 0;   // versao de notificacao ja desenhada
static estado_energia_t s_barra_estado = EN_BATERIA;
static const char* s_barra_nome = nullptr;
// A COR DE CADA GRAVIDADE - uma so, para a barra, a central e o bloqueio
// falarem a mesma lingua.
static uint16_t cor_do_nivel(notif_nivel_t n)
{
  return (n >= NOTIF_URGENTE) ? (uint16_t)TFT_RED
       : (n == NOTIF_ATENCAO) ? (uint16_t)TFT_ORANGE
       : (n == NOTIF_INFO)    ? (uint16_t)TFT_SKYBLUE
                              : (uint16_t)TFT_LIGHTGREY;   // ESTADO: atividade, nao novidade
}

// completo=true pinta o FUNDO da barra antes dos elementos. E preciso sempre
// que a tela tenha sido apagada (abertura, volta do repouso, troca de app),
// senao sobra o preto do framebuffer e a barra parece ter sumido - foi o que
// aconteceu quando o fillRect foi removido achando que era so o flash.
//
// completo=false e a atualizacao periodica: os elementos se sobrescrevem
// sozinhos (texto com cor de fundo, icones limpando o proprio retangulo) e
// pintar o fundo inteiro aqui seria justamente o flash que a gente tirou.
void desenhar_barra_status(lgfx::LovyanGFX& d, bool completo)
{
  const int L = d.width();
  char buf[32];

  // Le do CACHE. A leitura de verdade acontece uma vez por segundo no loop da
  // tarefa; ir ao I2C aqui de novo seria uma transacao a mais por redesenho da
  // barra, sem nenhum dado novo.
  tab5_measurement_t m = tab5_power_latest();
  const estado_energia_t est = estado_energia(m);

  // EM FAIXAS. 1280x64 em deitado sao ~164KB escritos de uma vez, e a barra e
  // repintada em toda troca de app - o mesmo tipo de rajada que produzia a
  // piscada, so que menor. tab5_screen_clear respira entre as faixas.
  if (completo) {
    tab5_screen_clear(0, 0, L, ALTURA_BARRA, SIS_FUNDO);
    // Os blocos da esquerda, meio e direita limpam os proprios retangulos logo
    // abaixo, todos em SIS_FUNDO e todos dentro deste. Marcados, eles somem.
    tab5_screen_mark_clean(0, 0, L, ALTURA_BARRA, SIS_FUNDO);
  }

  // Fora do 'completo', NAO apagamos a barra inteira.
  //
  // O fillRect de largura total era um flash visivel a cada repintura - discreto
  // enquanto o meio da barra era texto, evidente depois que virou um icone com
  // arcos. Cada elemento agora se vira: o texto escreve com cor de fundo (que
  // sobrescreve em vez de limpar) e os dois icones limpam so o proprio retangulo.
  //
  // Preco disso: todo texto daqui PRECISA ter largura fixa. Um nome de app mais
  // curto que o anterior deixaria rabo do antigo na tela.
  // ---- ESQUERDA: a hora, e o app logo em seguida ----------------------
  //
  // Os dois juntos de proposito. Sao as duas coisas que voce le sem procurar -
  // que horas sao e onde estou - e o MEIO fica inteiro para os avisos, que e
  // conteudo que aparece e some. A hora ficava na faixa de baixo e a
  // temperatura do P4 aqui; trocaram de lugar: a hora e o que se consulta o
  // tempo todo, e o canto de cima e onde todo celular a poe.
  //
  // NA TELA DE BLOQUEIO, NAO: ela ja tem a hora grande no meio, e a hora
  // repetida aqui em cima seria a mesma informacao duas vezes. Ali o canto
  // continua com o P4 e a temperatura, como era.
  //
  // ESQ_FIM e a fronteira, e ela existe porque o bloco do meio ja comeu o nome
  // do app uma vez: os dois limpavam retangulos que se cruzavam, e quem
  // desenhasse depois vencia. Agora cada lado sabe onde para.
  static const int ROT_X          = 16;
  static const int GAP_ROT_TERMO  = 14;  // espaco entre "P4" e o termometro
  static const int GAP_TERMO_TEMP = 15;  // espaco entre o termometro (+20%) e o numero
  static const int APP_X          = 168;
  static const int ESQ_FIM        = 340;

  tab5_screen_clear(0, 4, ESQ_FIM, ALTURA_BARRA - 8, SIS_FUNDO);
  if (!s_bloqueado) {
    hora_agora(buf, sizeof(buf));
    tab5_fonte_corpo_negrito(d);
    tab5_texto(d, 3.3f);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_WHITE, SIS_FUNDO);
    d.drawString(buf, ROT_X, ALTURA_BARRA / 2);
    tab5_fonte_corpo(d);
  } else {
    // Ordem de leitura: "P4", o termometro, o numero. O rotulo diz DE QUEM e a
    // medida, o simbolo diz o QUE e, o numero diz QUANTO.
    float c = 0.0f;
    const bool tem = tab5_sensors_temp(&c);
    tab5_texto(d, 3.0f);   // +20%
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_LIGHTGREY, SIS_FUNDO);
    d.drawString("P4", ROT_X, ALTURA_BARRA / 2);
    // Medido, nao fixo: a largura de "P4" depende da fonte do momento.
    const int termo_x = ROT_X + (int)d.textWidth("P4") + GAP_ROT_TERMO;
    desenhar_termometro(d, termo_x, ALTURA_BARRA / 2, tem, c, SIS_FUNDO);
    if (tem) { snprintf(buf, sizeof(buf), "%.0f°C", (double)c); }
    else     { snprintf(buf, sizeof(buf), "--"); }
    d.drawString(buf, termo_x + GAP_TERMO_TEMP, ALTURA_BARRA / 2);
  }

  // Nome do app em BRANCO e mais encorpado que o resto: e a resposta para "onde
  // eu estou", que se le antes de qualquer outra coisa na barra. E o unico
  // TITULO que aparece em toda tela do sistema, entao e o unico texto da
  // barra que troca para a Orbitron_Light (ver tab5_fonte_titulo em
  // tab5_ui.hpp) - o resto da barra (P4, wifi, bateria) sao leituras
  // curtas, nao titulo, e ficam na fonte de corpo.
  //
  // TAMANHO NATIVO (1) e a base ideal para uma fonte bitmap como a Orbitron -
  // escalar por uma fracao descarta/duplica linha de pixel de um jeito
  // irregular e engrossa o traco (foi por isso que "CONFIG" na barra parecia
  // uma fonte DIFERENTE da amostra da galeria em Ajustes). Mas 1.0 ficou
  // grande demais aqui na praticafoi reduzir uns 20%,
  // entao 0.8 (ainda bem mais limpo que uma fracao tipo 0.6, so um pouco
  // aquem do nativo).
  //
  // O negrito duplo-com-deslocamento que existia aqui era remendo para a
  // Font0 nao ter peso - a Orbitron_Light ja e uma fonte "leve" (light) de
  // proposito grafico, entao NAO desenhamos duas vezes: a segunda copia
  // deslocada 1px ficaria borrada, nao "em negrito", numa fonte com traco
  // fino e regular como esta.
  // Roboto negrito nativa (ver tab5_titulo em tab5_ui.hpp)
  // - era a FreeMonoBold a 1,04x, a ultima escala fracionaria da barra.
  tab5_titulo(d, 28);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, SIS_FUNDO);
  snprintf(buf, sizeof(buf), "%.14s", s_app ? s_app->name : "Home");
  d.drawString(buf, APP_X, ALTURA_BARRA / 2);
  tab5_fonte_corpo(d);

  // Estado do WiFi. Tudo daqui sai de cache: tab5_wifi_* nunca vai ao C6, quem
  // conversa pelo SDIO e a tarefa do servico, uma vez a cada 5s.
  const tab5_wifi_state_t ew = tab5_wifi_state();

  uint16_t cor_wifi = TFT_LIGHTGREY;
  int      barras   = 0;
  bool     cortado  = true;

  if (ew == TAB5_WIFI_CONNECTED)
  {
    cor_wifi = TFT_GREENYELLOW;
    cortado  = false;
    const int r = (int)tab5_wifi_rssi();
    // Sem leitura de sinal, mostramos o icone CHEIO em vez de vazio: estamos
    // conectados de fato (temos IP), e desenhar zero barras diria ao olho que a
    // conexao esta ruim - trocaria uma informacao ausente por uma errada.
    barras = (r == TAB5_WIFI_RSSI_UNKNOWN) ? 4
           : (r >= -55) ? 4 : (r >= -65) ? 3 : (r >= -75) ? 2 : 1;
  }
  else if (ew == TAB5_WIFI_STARTING || ew == TAB5_WIFI_CONNECTING)
  {
    // Amarelo e SEM o X: esta tentando, ainda nao falhou. Mostrar o X aqui
    // faria a tela dizer "sem conexao" durante uma conexao em andamento.
    cor_wifi = TFT_YELLOW;
    barras   = 0;
    cortado  = false;
  }
  else if (ew == TAB5_WIFI_FAILED || ew == TAB5_WIFI_NO_CREDENTIALS
        || ew == TAB5_WIFI_C6_UNRESPONSIVE)
  {
    // C6_MUDO tem que estar AQUI e em Ajustes: nao esta desligado, esta
    // QUEBRADO, e a diferenca decide se voce vai ao Ajustes religar (inutil) ou
    // atras do gravador. Todo estado novo do enum precisa ser revisitado nos
    // dois lugares.
    cor_wifi = TFT_RED;
  }

  // ---- DIREITA: aviso, wifi, bateria ----------------------------------
  //
  // Tudo agrupado no mesmo canto de proposito. O wifi ficava no MEIO da barra,
  // longe da bateria, e os dois sao a mesma pergunta - "como esta o aparelho
  // agora". Separados, o olho tinha que varrer a barra inteira para juntar.
  //
  // As posicoes saem da direita para a esquerda, ancoradas no icone da
  // bateria: assim mexer numa largura nao desalinha as outras.
  const int bx      = L - 16 - 64;        // icone da bateria
  const int pct_dir = bx - 12;            // borda direita do "100%"
  // 82: o suficiente para o braco direito do X (que chega a cx+22) nao encostar
  // no numero da porcentagem, e nao mais que isso - a 96 o icone ficava solto
  // no meio do caminho, longe do grupo a que pertence.
  const int wifi_cx = pct_dir - 82;       // centro do icone de wifi

  // O MEIO E SO DOS AVISOS, e ele comeca depois de ESQ_FIM. Antes o retangulo
  // do aviso era ancorado no wifi e crescia 150px para a esquerda, entrando por
  // cima do nome do app - era isso que comia o nome.
  const int bt_v     = bt_visual();
  const int bt_cx    = wifi_cx - 46;
  const int meio_ini = ESQ_FIM + 8;
  const int meio_fim = bt_v ? (bt_cx - 18) : (wifi_cx - 30);

  notif_item_t lista[NOTIF_MAX_FONTES];
  const int n_avisos = notif_listar(lista, NOTIF_MAX_FONTES);
  const notif_item_t& it = lista[0];
  tab5_screen_clear(meio_ini, 4, meio_fim - meio_ini, ALTURA_BARRA - 8, SIS_FUNDO);
  if (n_avisos > 0) {
    // UM PONTO POR APP A MAIS, na cor da gravidade dele, a direita da
    // pastilha. Antes as outras viravam so "+N" - dava para saber QUANTAS,
    // nunca se alguma delas era vermelha. Os pontos cabem ate onde o meio
    // deixa (sempre sobrando 80 px para a pastilha); o que nao couber volta
    // a ser "+N" na frente do texto. Tocar a barra abre a central de avisos,
    // com uma linha inteira por app.
    const int PONTO_P = 22;
    int pontos = n_avisos - 1;
    const int cabem = (meio_fim - meio_ini - 80) / PONTO_P;
    if (pontos > cabem) { pontos = cabem > 0 ? cabem : 0; }
    const int outras = (n_avisos - 1) - pontos;
    for (int k = 0; k < pontos; ++k) {
      d.fillCircle(meio_fim - PONTO_P / 2 - k * PONTO_P, ALTURA_BARRA / 2, 8,
                   cor_do_nivel(lista[pontos - k].nivel));
    }
    const int meio_fim_pastilha = meio_fim - (pontos ? pontos * PONTO_P + 6 : 0);
    const int meio_cx = (meio_ini + meio_fim_pastilha) / 2;
    const notif_nivel_t nivel = it.nivel;
    // A FONTE EM DESTAQUE (notif_destaque): a mais grave, e no empate a mais
    // recente - com o resumo DELA ("1 em LongFast"). As outras ativas viram
    // "+N" na frente, que o corte por largura (abaixo) nunca come. Antes,
    // varias fontes viravam "N avisos" SOMANDO pontos da Trilha com mensagens
    // do Mesh - uma mensagem nova sumia dentro do numero.
    char av[40];
    char resumo[NOTIF_RESUMO];
    if (it.resumo[0]) { snprintf(resumo, sizeof(resumo), "%s", it.resumo); }
    else              { snprintf(resumo, sizeof(resumo), "%d %s", it.quantidade, it.name); }
    if (outras > 0) { snprintf(av, sizeof(av), "+%d %s", outras, resumo); }
    else            { snprintf(av, sizeof(av), "%s", resumo); }
    // PASTILHA, e nao texto solto. Fundo colorido com o texto escuro por cima:
    // o aviso vira um objeto que se destaca da barra, em vez de mais um texto
    // cinza competindo com os outros dois.
    const uint16_t cn = cor_do_nivel(nivel);
    const int alt        = ALTURA_BARRA - 20;
    const int disponivel = meio_fim_pastilha - meio_ini;

    // O BUG DE VERDADE ESTAVA AQUI: "cabe ou nao desenha nada".
    //
    // Na HOME (retrato, L=720) o meio da barra tem so ~168px - o wifi e a
    // bateria ja tomam o resto. "%d no canal" com contagem de tres digitos
    // ("100 no canal", 12 caracteres) mede 172px e NAO cabia; o codigo antigo
    // simplesmente pulava o fillRoundRect e o drawString inteiros. O estado
    // interno estava certo o tempo todo - s_nao_lidas subiu, notif_publicar()
    // rodou, notif_versao() mudou, a barra ate REDESENHOU o retangulo do meio -
    // so que com nada dentro dele. Por fora, silencio; por dentro, tudo certo.
    // E o padrao de bug que ja apareceu neste sistema outras vezes: um estado
    // real sem representacao visual nenhuma.
    //
    // Um canal de malha PARADO semanas acumula tres digitos com facilidade -
    // "no mes passado" e exatamente o tempo que leva.
    //
    // A CORRECAO: o aviso SEMPRE aparece. Se o texto do resumo nao cabe
    // inteiro, ele e cortado para caber - errado por excesso de informacao
    // (numero truncado) e nao por ausencia total, que e o unico dos dois erros
    // que pode ser confundido com "nao tem nada para avisar".
    // MEDIDO DE VERDADE, nao mais "strlen*12": aquela conta vinha da largura
    // fixa da Font0 (6px/caractere) e a Font4 e PROPORCIONAL - um "i" e um "m"
    // nao ocupam o mesmo espaco. d.setTextSize() precisa vir ANTES de
    // d.textWidth(), porque a medida sai no tamanho ATUAL do texto.
    tab5_texto(d, 3);   // +50%
    // NEGRITO, para o aviso ler de longe. ANTES de medir, nao so antes de desenhar: a Bold e um
    // pouco mais larga que a Regular, e a conta de corte por caractere (logo
    // abaixo) tem que cortar pelo tamanho que sera DE FATO desenhado, senao o
    // texto vaza alguns pixels do espaco disponivel.
    tab5_fonte_corpo_negrito(d);
    // Corta MEDINDO (fonte proporcional) e por caractere UTF-8 - o resumo
    // tem acento ("Cartão no computador"), e o corte por byte desenhava lixo.
    tab5_cortar_ao_caber(d, av, disponivel - 28);
    const int larg = (int)d.textWidth(av) + 28;

    if (av[0] && larg <= disponivel) {
      d.fillRoundRect(meio_cx - larg / 2, 10, larg, alt, alt / 2, cn);
      d.setTextDatum(middle_center);
      d.setTextColor(TFT_BLACK, cn);
      d.drawString(av, meio_cx, ALTURA_BARRA / 2);
    } else if (disponivel >= 16) {
      // MESMO 1 CARACTERE NAO COUBE: uma bolinha na cor certa. Perde o numero,
      // mas nao perde a existencia do aviso - que e a garantia que este bloco
      // inteiro existe para dar.
      d.fillCircle(meio_cx, ALTURA_BARRA / 2, 7, cn);
    }
    tab5_fonte_corpo(d);   // devolve - ver o aviso de negrito, mais acima
  }

  // 56 de largura, e nao 32: o icone cresceu para caber o X ao lado. Um
  // retangulo curto deixaria meio X na tela quando a conexao voltasse.
  tab5_screen_clear(wifi_cx - 26, 4, 60, ALTURA_BARRA - 8, SIS_FUNDO);
  if (bt_v) {
    tab5_screen_clear(bt_cx - 14, 4, 28, ALTURA_BARRA - 8, SIS_FUNDO);
    desenhar_bt(d, bt_cx, ALTURA_BARRA / 2, bt_v);
  }
  s_barra_bt = bt_v;

  // A conta do centro, porque "-1" solto viraria numero magico em duas semanas:
  // o desenho poe o ponto da base em cy+9 e o arco maior sobe 20px a partir
  // dali, entao o icone ocupa de (cy-11) a (cy+12) - centro em cy+0.5. Para
  // esse centro cair no meio da barra, cy = ALTURA_BARRA/2 - 1.
  // (Antes estava (ALTURA_BARRA-18)/2, que deixava o icone 8px acima do centro.)
  const int rv = radio_visual();
  if (rv >= 100) { desenhar_espnow(d, wifi_cx, ALTURA_BARRA / 2 - 1, rv == 100); }
  else           { desenhar_wifi(d, wifi_cx, ALTURA_BARRA / 2 - 1, cor_wifi, barras, cortado); }

  // Bateria a direita: so o icone e a porcentagem. Tensao, corrente e potencia
  // vivem no app Energia - a barra nao e lugar de numero de diagnostico.
  desenhar_bateria(d, bx, (ALTURA_BARRA - 30) / 2, m.pct, est);

  // TAMANHO PROPRIO, explicito - antes herdava o que sobrava do P4 ou do
  // aviso (dependendo se havia aviso na passada), um acoplamento fragil que
  // ninguem tinha escolhido. 2.5 (o do P4) * 0.85 = 2.1; +35% -> 2.835.
  tab5_texto(d, 2.835f);
  d.setTextDatum(middle_right);
  d.setTextColor(TFT_WHITE, SIS_FUNDO);
  if (m.pct < 0) { snprintf(buf, sizeof(buf), "--%%"); }
  else           { snprintf(buf, sizeof(buf), "%3d%%", m.pct); }
  d.drawString(buf, pct_dir, ALTURA_BARRA / 2);

  { float c = 0.0f; s_barra_temp = tab5_sensors_temp(&c) ? (int)(c + 0.5f) : -999; }
  hora_agora(s_barra_hora, sizeof(s_barra_hora));
  s_barra_bloq   = s_bloqueado;
  s_barra_pct    = m.pct;
  s_barra_wifi   = rv;
  s_barra_notif  = notif_versao();
  s_barra_estado = est;
  s_barra_nome   = s_app ? s_app->name : nullptr;
}

// Verdadeiro se algo visivel na barra mudou desde o ultimo desenho dela.
bool barra_mudou(void)
{
  tab5_measurement_t m = tab5_power_latest();     // idem: cache, nao I2C
  const char* nome = s_app ? s_app->name : nullptr;
  // GRAU INTEIRO, e nao o float: a decimal muda quase toda leitura e a barra
  // seria repintada 1x por segundo para sempre - justamente o gasto que a
  // funcao existe para evitar.
  // A temperatura so conta na tela de bloqueio (e o unico lugar em que ela
  // esta nesta barra); fora dela, o que vira sozinho e a hora.
  float c = 0.0f;
  const int temp = tab5_sensors_temp(&c) ? (int)(c + 0.5f) : -999;
  char hora[8];
  hora_agora(hora, sizeof(hora));

  return (m.pct != s_barra_pct)
      || (s_bloqueado != s_barra_bloq)
      || (s_bloqueado ? (temp != s_barra_temp) : (strcmp(hora, s_barra_hora) != 0))
      || (estado_energia(m) != s_barra_estado)
      || (nome != s_barra_nome)
      // Sem esta linha o icone de WiFi so mudava quando a BATERIA mudava:
      // conectar ou cair nao repintava nada, e a barra mentia por minutos.
      || (radio_visual() != s_barra_wifi)
      || (bt_visual() != s_barra_bt)
      // Aviso novo tem que aparecer na hora, nao no proximo 1% de bateria. A
      // VERSAO muda so quando algo mudou de verdade - um servico que republica
      // o mesmo estado nao faz a barra repintar.
      || (notif_versao() != s_barra_notif);
}

// Ultima linha de servicos desenhada. Existe pelo mesmo motivo do
// barra_mudou(): a faixa e redesenhada so em repintura COMPLETA, e o conteudo
// dela muda sozinho - o Grove troca de modulo, o cartao entra, o radio cala.
// Sem comparar, a linha envelheceria na tela ate alguem abrir e fechar um app.
static char s_nav_txt[128] = "";
static char s_nav_temp[16] = "";  // a temperatura que a faixa mostra (com app aberto)

// A HORA DA BARRA DE CIMA (e da tela de bloqueio). "--:--" enquanto o relogio nao foi
// acertado (sem RTC valido nem rede) - melhor que uma hora de 1970.
static void hora_agora(char* out, size_t n)
{
  const uint32_t t = tab5_time_epoch();
  if (!t) { snprintf(out, n, "--:--"); return; }
  struct tm lt;
  tab5_local_time(t, &lt);
  snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

// A TEMPERATURA DO P4 NA FAIXA DE BAIXO (a hora subiu para a barra de cima).
//
// O mesmo desenho de coluna que GROVE e CARTAO usam - rubrica pequena em cima,
// valor embaixo - com o termometro a esquerda, na altura das duas linhas:
//
//   |  P4 · méd 41°
//   o  47°C
//
// A rubrica diz DE QUEM e a medida e traz a MEDIA desde o boot
// (tab5_sensors_average_temp): um numero sozinho nao diz se 47 e muito ou e o
// normal deste aparelho; ao lado da media, diz. O termometro sobe e muda de
// cor com a leitura (cor_da_temp), entao da para ler o estado sem ler o numero.

// Chave do que esta desenhado: so repinta quando o grau INTEIRO (ou a media)
// muda - a decimal mudaria quase toda leitura.
static void temp_chave(char* out, size_t n)
{
  float c = 0.0f, m = 0.0f;
  const int ci = tab5_sensors_temp(&c) ? (int)(c + 0.5f) : -999;
  const int mi = tab5_sensors_average_temp(&m) ? (int)(m + 0.5f) : -999;
  snprintf(out, n, "%d/%d", ci, mi);
}

static const int TEMP_TERMO_L = 26;   // termometro (14) e o respiro ate o texto

// Monta os dois textos e devolve a largura do bloco inteiro, para quem quiser
// centra-lo. Deixa a fonte no corpo.
static int temp_textos(lgfx::LovyanGFX& d, char* rot, size_t nr, char* val, size_t nv,
                       bool* tem, float* c)
{
  float m = 0.0f;
  *tem = tab5_sensors_temp(c);
  if (tab5_sensors_average_temp(&m)) { snprintf(rot, nr, "P4 · méd %.0f°", (double)m); }
  else                              { snprintf(rot, nr, "P4"); }
  if (*tem) { snprintf(val, nv, "%.0f°C", (double)*c); }
  else      { snprintf(val, nv, "--"); }
  tab5_fonte_corpo(d);
  tab5_texto(d, 1.95f);
  const int lr = (int)d.textWidth(rot);
  tab5_texto(d, 3.45f);
  const int lv = (int)d.textWidth(val);
  return TEMP_TERMO_L + (lr > lv ? lr : lv);
}

// Desenha o bloco com a borda esquerda em x0. Nao limpa nada: quem chama ja
// limpou a faixa (ou a parte dela que e sua).
static void desenhar_temp_nav(lgfx::LovyanGFX& d, int x0)
{
  const int y = d.height() - ALTURA_NAV;
  char rot[32], val[16];
  bool tem; float c = 0.0f;
  temp_textos(d, rot, sizeof(rot), val, sizeof(val), &tem, &c);

  // O termometro ocupa de cy-20 a cy+12: cy = meio da faixa + 4 o centra.
  desenhar_termometro(d, x0 + 7, y + ALTURA_NAV / 2 + 4, tem, c, TFT_BLACK);

  const int tx = x0 + TEMP_TERMO_L;
  d.setTextDatum(top_left);
  tab5_texto(d, 1.95f);   // o mesmo par de tamanhos da rubrica/valor de GROVE e CARTAO
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  const int rot_y = y + 8;
  d.drawString(rot, tx, rot_y);
  const int alt_rot = d.fontHeight();
  tab5_texto(d, 3.45f);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString(val, tx, rot_y + alt_rot + 4);
  tab5_fonte_corpo(d);
}

// Com um app aberto a temperatura fica a ESQUERDA da pilula - so ela e
// repintada quando o grau vira, nunca a pilula.
static void desenhar_temp_nav_app(lgfx::LovyanGFX& d)
{
  const int y = d.height() - ALTURA_NAV;
  int l = d.width() / 2 - NAV_BTN_L / 2 - 24;   // ate perto da pilula, sem encostar
  if (l > 240) { l = 240; }
  tab5_screen_clear(16, y + 1, l, ALTURA_NAV - 1, TFT_BLACK);
  desenhar_temp_nav(d, 24);
}

// Monta os dois campos. TUDO DE CACHE: nenhuma sondagem, nenhuma montagem de
// cartao, nada que va ao I2C. Esta funcao roda no tique periodico, e o caminho
// de desenho nao e lugar de ir ao hardware.
static void montar_servicos(char* esq, size_t te, char* dir, size_t td)
{
  if (!settings_grove()) {
    snprintf(esq, te, "off");
  } else if (tab5_mesh_servico_ativo()) {
    // Com o servico de pe, o que interessa nao e "tem um C6L ali" e sim se ele
    // esta respondendo e quantos nos alcanca.
    snprintf(esq, te, "Meshtastic - %d nodes, %s",
             tab5_mesh_num_nos(), tab5_mesh_no_ar() ? "online" : "silent");
  } else if (tab5_gps_service_active()) {
    // Mesma ideia do Mesh acima: o que importa e se ha FIX de verdade, nao so
    // "tem um GPS ali". qualidade==0 e "recebo o modulo mas ainda sem fix" -
    // acontece nos primeiros segundos, ou a ceu fechado.
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

// DUAS COLUNAS COM RUBRICA, e nao uma linha corrida.
//
// A linha corrida saia da area em portrait - 90 caracteres nao cabem em 720px -
// e mesmo cabendo era um paragrafo onde deveria haver um painel. Com rubrica
// pequena em cima e valor embaixo, o olho acha o campo sem ler o texto todo.
static void desenhar_servicos(lgfx::LovyanGFX& d, const char* esq, const char* dir)
{
  const int L = d.width();
  const int y = d.height() - ALTURA_NAV;

  // A MAIOR DAS QUE SOBRARAM. 1280x95 sao ~243KB numa
  // rajada, e ela e repintada a cada mudanca da linha de servicos.
  tab5_screen_clear(0, y + 1, L, ALTURA_NAV - 1, TFT_BLACK);

  // Teto e divisor: e o que transforma dois textos soltos num painel. O divisor
  // vai so na altura do conteudo, sem encostar nas bordas - encostando, ele
  // viraria uma parede e cortaria a faixa em duas telas.
  // TRES COLUNAS: GROVE | P4 | CARTAO. A do meio e estreita e
  // so tem a temperatura; as dos lados dividem o resto.
  const int meio_l = 210;
  const int lado_l = (L - meio_l) / 2;
  d.drawFastHLine(0, y, L, SIS_LINHA);
  d.drawFastVLine(lado_l, y + 16, ALTURA_NAV - 32, SIS_LINHA);
  d.drawFastVLine(lado_l + meio_l, y + 16, ALTURA_NAV - 32, SIS_LINHA);

  {
    char rot[32], val[16];
    bool tem; float c = 0.0f;
    const int bl = temp_textos(d, rot, sizeof(rot), val, sizeof(val), &tem, &c);
    desenhar_temp_nav(d, L / 2 - bl / 2);
  }

  struct { const char* rot; const char* val; int cx; } col[2] = {
    { "GROVE",  esq, lado_l / 2 },
    { "CARD",   dir, L - lado_l / 2 },
  };

  for (int i = 0; i < 2; ++i) {
    d.setTextDatum(top_center);

    // POSICAO DO ROTULO E DO VALOR MEDIDA DE VERDADE (d.fontHeight()), nao
    // mais "y+22"/"y+44" fixos (cortavam a parte de baixo do texto): os offsets antigos foram calculados a
    // olho para o tamanho de ANTES do aumento, e nao acompanharam quando o
    // rotulo/valor cresceram (+30%/+15%). Medir a altura
    // real de CADA tamanho antes de posicionar o proximo elimina esse
    // descompasso de vez - cresce o texto, cresce o espaco reservado, junto.
    tab5_texto(d, 1.95f);  // +30%; +50%
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    const int rot_y = y + 8;   // faixa de 72 px: rotulo + valor + respiro cabem com 8 em cima
    d.drawString(col[i].rot, col[i].cx, rot_y);
    const int alt_rot = d.fontHeight();

    tab5_texto(d, 3.45f);  // +15%; +50%
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    // Corta no que cabe na METADE da tela, e nao no tamanho do buffer: o texto
    // e centrado, entao um valor comprido sangraria para os dois lados. Corta
    // caractere por caractere medindo de verdade (d.textWidth) - a conta por
    // "12px por caractere" era da Font0 (largura fixa) e nao vale para a Font4
    // (proporcional).
    char cabe[40];
    snprintf(cabe, sizeof(cabe), "%s", col[i].val);
    tab5_cortar_ao_caber(d, cabe, lado_l - 32);   // por caractere: "nós" tem acento
    d.drawString(cabe, col[i].cx, rot_y + alt_rot + 4);
  }
}

// Chamada pelo loop principal (core.cpp) no mesmo tique da barra de status -
// so redesenha se o TEXTO mudou. Comparar strings de 90 bytes de dois em dois
// segundos e mais barato que pintar a faixa a toa. So faz sentido na HOME
// (s_app==NULL): com um app aberto a faixa mostra o botao, nao servicos.
void atualizar_linha_servicos(void)
{
  if (s_bloqueado) { return; }   // a faixa de baixo e preta enquanto bloqueado
  char temp[16];
  temp_chave(temp, sizeof(temp));
  if (s_app) {
    // Com app aberto so a temperatura muda sozinha nesta faixa.
    if (strcmp(temp, s_nav_temp) != 0) {
      snprintf(s_nav_temp, sizeof(s_nav_temp), "%s", temp);
      tela_virtual().startWrite();
      desenhar_temp_nav_app(tela_virtual());
      tela_virtual().endWrite();
    }
    return;
  }
  char e[64], v[64], junto[128];
  montar_servicos(e, sizeof(e), v, sizeof(v));
  snprintf(junto, sizeof(junto), "%s|%s|%s", e, v, temp);
  if (strcmp(junto, s_nav_txt) != 0) {
    snprintf(s_nav_txt, sizeof(s_nav_txt), "%s", junto);
    tela_virtual().startWrite();
    desenhar_servicos(tela_virtual(), e, v);
    tela_virtual().endWrite();
  }
}

// O QUE O BOTAO DA FAIXA FAZ, e o desenho segue. Hoje ele so tem um trabalho
// - fechar o app e voltar a tela inicial - mas o simbolo sai daqui, e nao de
// um texto cravado no desenho: no dia em que o botao fizer outra coisa (um app
// com niveis voltando um nivel, por exemplo), muda esta funcao e o tratamento
// do toque, e o desenho acompanha sozinho.
typedef enum {
  NAV_INICIO,   // circulo: a tela inicial (o "home" do Android)
  NAV_VOLTAR,   // triangulo para a esquerda: um passo para tras
} nav_acao_t;

static nav_acao_t nav_acao(void)
{
  return NAV_INICIO;
}

// Os simbolos que todo celular usa, sem texto: cabem em qualquer idioma e
// leem antes de qualquer palavra. Traco de 3 px, na tinta do botao.
static void desenhar_icone_nav(lgfx::LovyanGFX& d, int cx, int cy, nav_acao_t acao,
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

void desenhar_barra_nav(lgfx::LovyanGFX& d)
{
  const int L = d.width();
  const int y = d.height() - ALTURA_NAV;

  tab5_screen_clear(0, y, L, ALTURA_NAV, TFT_BLACK);
  // MARCA. O desenhar_servicos logo abaixo pede a mesma limpeza - e pede sem
  // condicao, como deve. Sem esta marca a faixa era limpa DUAS vezes na home:
  // 276KB onde 138 bastavam, na transicao mais pesada que existe.
  tab5_screen_mark_clean(0, y, L, ALTURA_NAV, TFT_BLACK);
  d.drawFastHLine(0, y, L, SIS_LINHA);

  // NA HOME, A FAIXA CONTA O QUE ESTA LIGADO NO APARELHO.
  //
  // Sem app aberto nao ha "INICIO" para desenhar e a faixa ficava vazia. A
  // barra de CIMA ja responde "como esta o aparelho" - bateria, temperatura,
  // wifi. O que ela nao responde e "o que esta ligado nele", e essa e a
  // pergunta que custou tempo nas ultimas sessoes: o Grove trocou de modulo e
  // ninguem avisou, o cartao dizia "sem cartao" com cartao dentro.
  //
  // Tudo daqui sai de CACHE - nenhuma sondagem, nenhuma montagem de cartao, nada
  // que va ao I2C. A faixa e desenhada junto do resto da home, e o caminho de
  // desenho nao e lugar de ir ao hardware.
  if (!s_app) {
    char e[64], v[64], temp[16];
    montar_servicos(e, sizeof(e), v, sizeof(v));
    temp_chave(temp, sizeof(temp));
    snprintf(s_nav_txt, sizeof(s_nav_txt), "%s|%s|%s", e, v, temp);
    desenhar_servicos(d, e, v);
    return;
  }

  if (s_app) {
    // Travado: desenhado apagado, para o olho saber que nao adianta tocar.
    const uint16_t fundo = s_nav_travada ? (uint16_t)TFT_BLACK : (uint16_t)SIS_FUNDO;
    const uint16_t tinta = s_nav_travada ? (uint16_t)SIS_LINHA  : (uint16_t)TFT_WHITE;
    d.fillRoundRect(L / 2 - NAV_BTN_L / 2, y + NAV_BTN_Y0, NAV_BTN_L, NAV_BTN_A, 16, fundo);
    d.drawRoundRect(L / 2 - NAV_BTN_L / 2, y + NAV_BTN_Y0, NAV_BTN_L, NAV_BTN_A, 16, SIS_LINHA);
    desenhar_icone_nav(d, L / 2, y + NAV_BTN_Y0 + NAV_BTN_A / 2, nav_acao(), tinta, fundo);
    temp_chave(s_nav_temp, sizeof(s_nav_temp));
    desenhar_temp_nav_app(d);
  }
}

// RESERVA ABAIXO DO ICONE, para o rotulo (nome do app, ver desenhar_home) -
// era "60" cravado no meio da conta de py. Reduzida para 45: a
// 9 apps (3x3 exato) nunca sobrava uma 4a linha para testar o limite; o app
// Camera virou o 10o e criou uma 4a linha com UM icone so, cujo ROTULO
// (desenhado 12px abaixo do icone, ver mais adiante) encostava na faixa de
// GROVE/CARTAO - o icone em si passava no teste de "cabe" (linha abaixo),
// mas o texto do nome ficava fora da conta. A folga entre o rotulo de uma
// linha e o icone da proxima era generosa (~50px sobrando) - os 15px a menos
// aqui saem dessa folga, nao do respiro em volta do icone nem do rotulo.
static const int ICONE_VAO_LINHA = 45;

// Faixa embaixo dos icones para as bolinhas de pagina.
static const int HOME_BOLINHAS_A = 44;

static void celula_do_icone(int i, const area_t& a, int* px, int* py)
{
  const int col = i % ICONE_COLS;
  const int lin = i / ICONE_COLS;
  const int vao = (a.largura - ICONE_COLS * ICONE_LADO) / (ICONE_COLS + 1);
  *px = a.x + vao + col * (ICONE_LADO + vao);
  *py = a.y + vao + lin * (ICONE_LADO + ICONE_VAO_LINHA + vao);
}

// ---------------------------------------------------------------------------
// PAGINAS DA HOME
// ---------------------------------------------------------------------------
// A pagina p mostra os apps [p*por_pagina, (p+1)*por_pagina). celula_do_icone()
// continua recebendo o indice DENTRO da pagina, entao desenho e toque (que
// usam a mesma funcao) seguem sempre de acordo. Nada de apps.cpp muda: o
// numero de paginas sai apenas dos aplicativos visiveis.
static int s_pagina_home = 0;

static int home_por_pagina(const area_t& a)
{
  int linhas = 0;
  while (linhas < TAB5_HOME_LINHAS_MAX) {
    int px, py;
    celula_do_icone(linhas * ICONE_COLS, a, &px, &py);
    if (py + ICONE_LADO + 40 > a.y + a.altura - HOME_BOLINHAS_A) { break; }
    ++linhas;
  }
  return (linhas < 1 ? 1 : linhas) * ICONE_COLS;
}

static int home_n(void);

static int home_paginas(const area_t& a)
{
  const int pp = home_por_pagina(a);
  return (home_n() + pp - 1) / pp;
}

static bool home_mudar_pagina(int delta)
{
  const int n = home_paginas(area_t(sistema_area()));
  int nova = s_pagina_home + delta;
  if (nova < 0 || nova >= n) { return false; }
  s_pagina_home = nova;
  s_sujo_layout = true;   // mudou a POSICAO dos icones: repintura completa
  return true;
}

// ---------------------------------------------------------------------------
// ORDEM DOS ICONES (ver sistema_home_app em app.h)
// ---------------------------------------------------------------------------
// s_ordem[pos] = indice em SISTEMA_APPS. Desenho e toque leem daqui; so
// mover/carregar escrevem, e sempre dentro de s_ordem_mux - o console mexe de
// outra tarefa. Ler sem trava e seguro: cada entrada e um byte, e o pior caso
// e um quadro com a ordem velha, logo repintado (mover pede redesenho).
#define HOME_APPS_MAX 64
static uint8_t s_ordem[HOME_APPS_MAX];
static int     s_n_ordem = 0;
static portMUX_TYPE s_ordem_mux = portMUX_INITIALIZER_UNLOCKED;

static int home_n(void) { return s_n_ordem ? s_n_ordem : SISTEMA_NUM_APPS; }

const app_t* sistema_home_app(int pos)
{
  if (pos < 0 || pos >= home_n()) { return nullptr; }
  return SISTEMA_APPS[s_n_ordem ? s_ordem[pos] : pos];
}

int sistema_home_posicao(const app_t* app)
{
  for (int i = 0; i < home_n(); ++i) { if (sistema_home_app(i) == app) { return i; } }
  return -1;
}

// Um nome inteiro numa lista separada por virgulas. A comparacao sem distinguir
// maiusculas segue a da ordem dos icones.
static bool nome_na_lista(const char* lista, const char* nome)
{
  const size_t nome_n = strlen(nome);
  const char* p = lista ? lista : "";
  while (*p) {
    const char* fim = strchr(p, ',');
    const size_t n = fim ? (size_t)(fim - p) : strlen(p);
    if (n == nome_n && strncasecmp(p, nome, n) == 0) { return true; }
    if (!fim) { break; }
    p = fim + 1;
  }
  return false;
}

// Monta a ordem a partir dos nomes: os conhecidos e visiveis na ordem dada
// (repetido vale uma vez), e depois os visiveis que faltaram na ordem de
// apps.cpp. Ajustes nunca some, mesmo se uma NVS antiga ou editada o listar.
static void ordem_de_nomes(const char* nomes, const char* ocultos)
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
    const char* fim = strchr(p, ',');
    const size_t len = fim ? (size_t)(fim - p) : strlen(p);
    for (int i = 0; i < total; ++i) {
      const char* nome = SISTEMA_APPS[i]->name;
      const bool oculto = strcasecmp(nome, "Settings") != 0 && nome_na_lista(ocultos, nome);
      if (!oculto && !usado[i] && strlen(nome) == len && strncasecmp(nome, p, len) == 0) {
        usado[i] = true; nova[n++] = (uint8_t)i; break;
      }
    }
    if (!fim) { break; }
    p = fim + 1;
  }
  for (int i = 0; i < total; ++i) {
    const char* nome = SISTEMA_APPS[i]->name;
    const bool oculto = strcasecmp(nome, "Settings") != 0 && nome_na_lista(ocultos, nome);
    if (!usado[i] && !oculto) { nova[n++] = (uint8_t)i; }
  }

  taskENTER_CRITICAL(&s_ordem_mux);
  memcpy(s_ordem, nova, n);
  s_n_ordem = n;
  taskEXIT_CRITICAL(&s_ordem_mux);
}

// A ordem usa o mesmo buffer para gravar e carregar; a lista de ocultos fica
// ao lado porque tambem e usada ao restaurar a ordem de fabrica.
static char s_ordem_nomes[1024];
static char s_ocultos_nomes[1024];

static void ordem_gravar(void)
{
  char* const nomes = s_ordem_nomes;
  const size_t tam = sizeof(s_ordem_nomes);
  size_t k = 0;
  nomes[0] = 0;
  for (int i = 0; i < home_n(); ++i) {
    const char* nome = sistema_home_app(i)->name;
    const size_t len = strlen(nome);
    if (k + len + 2 > tam) { break; }
    if (k) { nomes[k++] = ','; }
    memcpy(nomes + k, nome, len);
    k += len;
    nomes[k] = 0;
  }
  settings_set_apps_order(nomes);
}

void home_ordem_carregar(void)
{
  settings_apps_order(s_ordem_nomes, sizeof(s_ordem_nomes));
  settings_apps_hidden(s_ocultos_nomes, sizeof(s_ocultos_nomes));
  ordem_de_nomes(s_ordem_nomes, s_ocultos_nomes);
  s_pagina_home = 0;
}

// Tira o app de 'de' e encaixa em 'para'; os do meio andam uma casa.
static void ordem_mover(int de, int para)
{
  taskENTER_CRITICAL(&s_ordem_mux);
  if (s_n_ordem && de >= 0 && de < s_n_ordem && para >= 0 && para < s_n_ordem && de != para) {
    const uint8_t v = s_ordem[de];
    if (de < para) { memmove(&s_ordem[de], &s_ordem[de + 1], para - de); }
    else           { memmove(&s_ordem[para + 1], &s_ordem[para], de - para); }
    s_ordem[para] = v;
  }
  taskEXIT_CRITICAL(&s_ordem_mux);
}

bool sistema_home_mover(const app_t* app, int pos)
{
  const int de = sistema_home_posicao(app);
  if (de < 0 || pos < 0 || pos >= home_n()) { return false; }
  ordem_mover(de, pos);
  ordem_gravar();
  sistema_pedir_redesenho_completo();
  return true;
}

void sistema_home_ordem_padrao(void)
{
  ordem_de_nomes("", s_ocultos_nomes);
  settings_set_apps_order("");
  sistema_pedir_redesenho_completo();
}

void sistema_home_apps_recarregar(void)
{
  home_ordem_carregar();
  sistema_pedir_redesenho_completo();
}

// Que posicao (na ordem) esta sob o dedo, na pagina atual; -1 = nenhuma.
// Sem folga e o quadrado do icone (o clique). Com folga a celula inteira,
// rotulo e meio vao incluidos, e uma celula vazia no fim da ultima pagina
// vale a ultima posicao - e o alvo do arraste, que nao pode cair em buraco.
static int home_pos_em(const area_t& a, int tx, int ty, bool folga)
{
  const int por_pagina = home_por_pagina(a);
  const int primeiro = s_pagina_home * por_pagina;
  const int vao = (a.largura - ICONE_COLS * ICONE_LADO) / (ICONE_COLS + 1);
  const int fx = folga ? vao / 2 : 0;
  const int fy_baixo = folga ? 40 + vao / 2 : 0;
  for (int k = 0; k < por_pagina; ++k) {
    int px, py;
    celula_do_icone(k, a, &px, &py);
    if (tx >= px - fx && tx < px + ICONE_LADO + fx && ty >= py - fx && ty < py + ICONE_LADO + fy_baixo) {
      const int pos = primeiro + k;
      if (pos < home_n()) { return pos; }
      return folga ? home_n() - 1 : -1;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// MOVER ICONE (segurar e arrastar) - ver home_segurar em internal.hpp
// ---------------------------------------------------------------------------
// Repinta a home so quando o icone TROCA de casa, nunca a cada amostra do
// dedo: cada repintura completa sao ~70KB por icone (ver desenhar_home).
// Para outra pagina: segurar o dedo na beirada lateral por HOME_BORDA_MS.
#define HOME_SEGURAR_MS 600
#define HOME_BORDA_PX   60
#define HOME_BORDA_MS   700

static int      s_mover = -1;          // posicao do icone levantado; -1 = nenhum
static bool     s_mover_mudou = false;
static uint32_t s_borda_desde = 0;

bool home_segurar(int tx, int ty, uint32_t agora, uint32_t segurando_ms, int desvio)
{
  const area_t a = sistema_area();
  ty -= TOQUE_DY;   // a mesma correcao do clique (tratar_toque)

  if (s_mover < 0) {
    if (segurando_ms < HOME_SEGURAR_MS || desvio > ROLAGEM_ARRASTE_MIN) { return false; }
    const int pos = home_pos_em(a, tx, ty, false);
    if (pos < 0) { return false; }
    s_mover = pos;
    s_mover_mudou = false;
    s_borda_desde = 0;
    s_sujo_layout = true;
    ESP_LOGI(TAG, "movendo '%s' (posicao %d)", sistema_home_app(pos)->name, pos + 1);
    return true;
  }

  const int paginas = home_paginas(a);
  const int lado = (tx < a.x + HOME_BORDA_PX && s_pagina_home > 0) ? -1
                 : (tx >= a.x + a.largura - HOME_BORDA_PX && s_pagina_home < paginas - 1) ? +1 : 0;
  if (lado) {
    if (!s_borda_desde) { s_borda_desde = agora; return true; }
    if (agora - s_borda_desde < HOME_BORDA_MS) { return true; }
    s_borda_desde = agora;   // segurando ali, a proxima pagina vem depois de outro prazo
    const int pp = home_por_pagina(a);
    s_pagina_home += lado;
    // Entra pela beirada de onde veio: indo para a direita, primeira casa.
    int para = (lado > 0) ? s_pagina_home * pp : (s_pagina_home + 1) * pp - 1;
    if (para >= home_n()) { para = home_n() - 1; }
    ordem_mover(s_mover, para);
    s_mover = para;
    s_mover_mudou = true;
    s_sujo_layout = true;
    return true;
  }
  s_borda_desde = 0;

  const int pos = home_pos_em(a, tx, ty, true);
  if (pos >= 0 && pos != s_mover) {
    ordem_mover(s_mover, pos);
    s_mover = pos;
    s_mover_mudou = true;
    s_sujo_layout = true;
  }
  return true;
}

bool home_soltar(void)
{
  if (s_mover < 0) { return false; }
  if (s_mover_mudou) {
    ESP_LOGI(TAG, "'%s' agora na posicao %d", sistema_home_app(s_mover)->name, s_mover + 1);
    ordem_gravar();
  }
  s_mover = -1;
  s_sujo_layout = true;
  return true;
}

static void desenhar_bolinhas(lgfx::LovyanGFX& d, const area_t& a)
{
  const int n = home_paginas(a);
  if (n < 2) { return; }
  const int passo = 32;
  const int x0 = a.x + a.largura / 2 - (n - 1) * passo / 2;
  const int y  = a.y + a.altura - HOME_BOLINHAS_A / 2;
  for (int i = 0; i < n; ++i) {
    if (i == s_pagina_home) { d.fillCircle(x0 + i * passo, y, 9, TFT_WHITE); }
    else                    { d.fillCircle(x0 + i * passo, y, 7, SIS_LINHA); }
  }
}

void desenhar_home(lgfx::LovyanGFX& d, const area_t& a)
{
  // PAPEL DE PAREDE (Ajustes > Tela). So nesta passada - a home e estatica e
  // so e desenhada inteira, com a area recem-limpa. A semente e tirada uma
  // vez por boot - e de novo quando o usuario volta a escolher "Sorteio" -,
  // entao o desenho nao muda a cada volta para a home.
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
  const int primeiro = s_pagina_home * por_pagina;
  // +40 cobre o rotulo (12px de respiro + a altura do texto do nome) - sem
  // isto o icone passava no teste mas o nome dele vazava por baixo, sem
  // nenhum aviso ate alguem notar na tela de verdade (foi assim que o
  // ICONE_VAO_LINHA acima precisou ser reduzido).
  for (int k = 0; k < por_pagina && primeiro + k < home_n(); ++k) {
    int px, py;
    celula_do_icone(k, a, &px, &py);

    // RESPIRO ENTRE ICONES, e ele mora AQUI e nao no icone de cada app.
    //
    // Cada icone tem 190x190 = 70KB, que passa por baixo da faixa de 96KB e
    // nunca e fatiado. Oito seguidos sao 564KB numa rajada continua - a maior
    // escrita sem pausa do sistema, e justamente na saida para a home.
    //
    // O loop e do sistema, entao a pausa e do sistema: quem escreve app desenha
    // UM icone e nao tem como esquecer o que nao lhe pertence. Por orcamento e
    // nao por volta - assim o mesmo codigo serve se um dia o icone mudar de
    // tamanho ou a home passar a ter vinte apps.

    const app_t* app = sistema_home_app(primeiro + k);
    // O ICONE LEVANTADO (segurar e arrastar): moldura branca em volta, no vao
    // entre icones - o desenho do icone em si nao muda.
    if (primeiro + k == s_mover) {
      for (int e = 0; e < 4; ++e) {
        d.drawRoundRect(px - 10 + e, py - 10 + e, ICONE_LADO + 20 - 2 * e, ICONE_LADO + 20 - 2 * e, 34, TFT_WHITE);
      }
    }
    if (app->icone) {
      app->icone(d, px, py, ICONE_LADO);
    } else {
      d.fillRoundRect(px, py, ICONE_LADO, ICONE_LADO, 28, app->cor);
      char inicial[2] = { app->name[0], 0 };
      tab5_texto(d, 6.25f);   // 6.25 = escala real 2.0
      d.setTextDatum(middle_center);
      d.setTextColor(TFT_BLACK, app->cor);
      d.drawString(inicial, px + ICONE_LADO / 2, py + ICONE_LADO / 2);
    }

    // NOME DO APP NA ORBITRON: Orbitron so na
    // "interface" (barra do kernel + home), DejaVu dentro de qualquer app. A
    // home e a segunda metade dessa regra. Nasceu em tamanho 1 (nativo, ver o
    // aviso sobre fonte bitmap + escala fracionaria em tab5_ui.hpp); grande demais, reduzido uns 25% -> 0.75.
    tab5_titulo(d, 30);
    d.setTextDatum(top_center);
    // SEM COR DE FUNDO: com ela o nome saia numa tarja preta por cima do
    // papel de parede. Sem, a borda suave da letra se mistura com o que ja
    // esta na tela - aqui e seguro, porque o nome e pintado uma vez so por
    // passada, sobre a area recem-pintada (nunca engrossa).
    d.setTextColor(TFT_WHITE);

    // LARGURA MEDIDA DE VERDADE, nao suposta (nomes vizinhos encostavam).
    // "CreazySnake" (11
    // caracteres) e bem mais comprido que qualquer outro nome de app, e o
    // rotulo nunca tinha sido testado contra a largura disponivel - podia
    // sair encostando no icone vizinho, comendo a folga que separa os dois.
    // ESCALA-SE PARA BAIXO em vez de CORTAR o texto: um nome truncado
    // ("CreazyS...") deixa de identificar o app, uma fonte um pouco menor
    // ainda identifica. O piso de 0,8 evita este unico app ficar visivelmente
    // menor que os outros - se nem no piso coubesse, o certo seria encurtar o
    // NOME do app, nao espremer a fonte ate ficar ilegivel.
    const int vao = (a.largura - ICONE_COLS * ICONE_LADO) / (ICONE_COLS + 1);
    const int largura_disponivel = ICONE_LADO + vao;   // meia folga de cada lado
    // Encolhe em ALTURAS NATIVAS, nunca por escala.
    int alt_nome = 30;
    while ((int)d.textWidth(app->name) > largura_disponivel && alt_nome > 22) {
      alt_nome -= 2;
      tab5_titulo(d, alt_nome);
    }
    d.drawString(app->name, px + ICONE_LADO / 2, py + ICONE_LADO + 12);
    tab5_fonte_corpo(d);
  }
  desenhar_bolinhas(d, a);
}

// ---------------------------------------------------------------------------
// TELA DE BLOQUEIO
// ---------------------------------------------------------------------------
// v1, de proposito simples: hora grande, data, o aviso em destaque (a mesma
// fonte que a barra de status usa) e a dica de deslizar. Sem senha. Tudo
// que muda com o tempo compara com o cache antes de pintar, como o resto do
// sistema - o relogio e conferido a cada segundo e so repinta quando o
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

// Os textos da tela de bloqueio ficam NA FRENTE da animacao.
//
// Cada texto e pintado UMA vez (quando muda) num sprite pequeno, com fundo
// preto e a fonte suave de sempre - o mesmo desenho de antes da animacao. A
// cada quadro, so onde a animacao passou por cima, o sprite e copiado para a
// tela com o PRETO TRANSPARENTE. Assim o texto nunca depende do que ha na
// pagina de tras: pintar texto suave sem fundo faz o M5GFX LER os pixels da
// tela e misturar a borda das letras com eles, e isso engrossa a letra a cada
// repintura e a deixa feia.
//
// Caixas fixas, sem se sobrepor, com folga em volta do texto.
enum { BQ_HORA, BQ_DATA, BQ_AVISO, BQ_DICA, BQ_N };

static lgfx::LGFX_Sprite* s_bq_spr[BQ_N] = {};

// AVISOS NO BLOQUEIO: uma linha por app, quantas couberem entre a data e a
// dica (ate 4). Deitado sobra espaco para uma so - a mais grave.
static const int BQ_AVISO_LINHA = 52;
static int bq_linhas_aviso(const area_t& a)
{
  const int y_hora = a.y + a.altura / 4;
  const int y_dica = a.y + a.altura - 110;
  const int n = ((y_dica - 48) - (y_hora + 190)) / BQ_AVISO_LINHA;
  return n < 1 ? 1 : (n > 4 ? 4 : n);
}

static void bq_caixas(const area_t& a, bq_ret_t r[BQ_N])
{
  const int cx = a.x + a.largura / 2;
  const int y_hora = a.y + a.altura / 4;
  const int y_dica = a.y + a.altura - 110;
  r[BQ_HORA]  = { (int16_t)(cx - 240), (int16_t)(y_hora - 92),  480, 170 };
  r[BQ_DATA]  = { (int16_t)(cx - 350), (int16_t)(y_hora + 78),  700, 64 };
  r[BQ_AVISO] = { (int16_t)(a.x + 10), (int16_t)(y_hora + 190), (int16_t)(a.largura - 20),
                  (int16_t)(bq_linhas_aviso(a) * BQ_AVISO_LINHA + 18) };
  r[BQ_DICA]  = { (int16_t)(cx - 200), (int16_t)(y_dica - 48),  400, 80 };
}

// Desenha o texto 'i' em 't' (um sprite ou a tela), com (ox, oy) = canto da
// caixa em coordenadas de tela. Fundo preto, sempre.
static void bq_texto(lgfx::LovyanGFX& t, const area_t& a, int i, const char* hora,
                     const char* data, const char* aviso, int ox, int oy)
{
  const int cx = a.x + a.largura / 2 - ox;
  const int y_hora = a.y + a.altura / 4 - oy;
  t.setTextDatum(middle_center);
  switch (i) {
    case BQ_HORA:
      tab5_fonte_corpo(t);
      tab5_texto(t, 7.0f);    // 56 px, a maior altura nativa da fonte de corpo; dobrada abaixo
      t.setTextSize(2);       // inteiro (nao fracionario): 1 pixel de fonte = 2x2 na tela
      t.setTextColor(TFT_WHITE, TFT_BLACK);
      t.drawString(hora, cx, y_hora);
      t.setTextSize(1);
      break;
    case BQ_DATA:
      tab5_fonte_corpo(t);
      tab5_texto(t, 3.5f);
      t.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      t.drawString(data, cx, y_hora + 110);
      break;
    case BQ_AVISO: {
      // 'aviso' e uma linha por app: o primeiro caractere e o nivel ('0'..'3'),
      // o resto o texto, e '\n' separa (montado em desenhar_bloqueio).
      tab5_fonte_corpo(t);
      tab5_texto(t, 3.0f);
      int y = y_hora + 225;
      for (const char* p = aviso; *p; ) {
        const char* fim = strchr(p, '\n');
        const size_t n = fim ? (size_t)(fim - p) : strlen(p);
        if (n > 1) {
          char linha[NOTIF_RESUMO + 24];
          snprintf(linha, sizeof(linha), "%.*s", (int)(n - 1), p + 1);
          t.setTextColor(cor_do_nivel((notif_nivel_t)(p[0] - '0')), TFT_BLACK);
          t.drawString(linha, cx, y);
          y += BQ_AVISO_LINHA;
        }
        if (!fim) { break; }
        p = fim + 1;
      }
    } break;
    default: {
      const int y_dica = a.y + a.altura - 110 - oy;
      t.fillRoundRect(cx - 60, y_dica - 40, 120, 8, 4, SIS_LINHA);
      tab5_fonte_corpo(t);
      tab5_texto(t, 3.0f);
      t.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      t.drawString("Swipe up", cx, y_dica);
    } break;
  }
}

static void bq_sprites_soltar(void)
{
  for (int i = 0; i < BQ_N; ++i) {
    delete s_bq_spr[i];
    s_bq_spr[i] = nullptr;
  }
}

// Cria os sprites (PSRAM, so enquanto bloqueado). false = sem memoria: o
// texto cai no desenho direto, com fundo preto.
static bool bq_sprites_criar(const bq_ret_t cx[BQ_N])
{
  for (int i = 0; i < BQ_N; ++i) {
    auto* sp = new lgfx::LGFX_Sprite();
    if (sp) {
      sp->setColorDepth(16);
      sp->setPsram(true);
    }
    if (!sp || !sp->createSprite(cx[i].w, cx[i].h)) {
      delete sp;
      bq_sprites_soltar();
      return false;
    }
    s_bq_spr[i] = sp;
  }
  return true;
}

void bloquear_tela(void)
{
  s_bloqueado   = true;
  s_painel_avisos = false;   // a central nao sobrevive ao bloqueio
  s_bq_hora[0]  = 0;   // forca o primeiro desenho completo
  s_sujo_layout = true;
  bq_anim_sortear();
}

void bloqueio_ao_ligar_tela(void)
{
  if (s_bloqueado)            { bq_anim_sortear(); }   // acordou ainda bloqueada: outra animacao
  else if (settings_lock_screen()) { bloquear_tela(); }
}

void desbloquear_tela(void)
{
  s_bloqueado   = false;
  s_sujo_layout = true;   // volta home/app: repintura completa, nav inclusive
  bq_sprites_soltar();
  bq_anim_soltar();
}

// Um quadro da tela de bloqueio (~25 Hz): animacao ao fundo, texto na frente.
void desenhar_bloqueio(lgfx::LovyanGFX& d, const area_t& a, bool completo)
{
  char hora[8], data[48], aviso[sizeof(s_bq_aviso)] = "";
  hora_agora(hora, sizeof(hora));
  data_por_extenso(data, sizeof(data));

  // UM APP POR LINHA, e nao so o destaque: com Mesh e ESP-NOW avisando ao
  // mesmo tempo, o bloqueio mostrava um e escondia o outro. A ultima linha
  // que cabe vira "+N" quando sobra app.
  notif_item_t lista[NOTIF_MAX_FONTES];
  const int n_av = notif_listar(lista, NOTIF_MAX_FONTES);
  const int cabem = bq_linhas_aviso(a);
  size_t usado = 0;
  for (int i = 0; i < n_av && i < cabem; ++i) {
    char txt[NOTIF_RESUMO + 24];
    if (i == cabem - 1 && n_av > cabem) {
      snprintf(txt, sizeof(txt), "+%d outros avisos", n_av - i);
    } else if (lista[i].resumo[0]) {
      snprintf(txt, sizeof(txt), "%s: %s", lista[i].name, lista[i].resumo);
    } else {
      snprintf(txt, sizeof(txt), "%s: %d", lista[i].name, lista[i].quantidade);
    }
    const int k = snprintf(aviso + usado, sizeof(aviso) - usado, "%c%s\n",
                           (char)('0' + (int)lista[i].nivel), txt);
    if (k < 0 || (size_t)k >= sizeof(aviso) - usado) { break; }
    usado += (size_t)k;
  }

  const bool hora_mudou  = strcmp(hora, s_bq_hora) != 0;
  const bool aviso_mudou = strcmp(aviso, s_bq_aviso) != 0;
  snprintf(s_bq_hora, sizeof(s_bq_hora), "%s", hora);
  snprintf(s_bq_aviso, sizeof(s_bq_aviso), "%s", aviso);

  bq_ret_t caixa[BQ_N];
  bq_caixas(a, caixa);
  const bool mudou[BQ_N] = { completo || hora_mudou, completo || hora_mudou, completo || aviso_mudou, completo };

  // Os sprites nascem (ou renascem) com o desenho completo e so sao pintados
  // de novo quando o texto muda.
  if (completo && !s_bq_spr[0]) { bq_sprites_criar(caixa); }
  const lgfx::TextStyle estilo = d.getTextStyle();
  for (int i = 0; i < BQ_N; ++i) {
    if (!mudou[i] || !s_bq_spr[i]) { continue; }
    s_bq_spr[i]->fillScreen(TFT_BLACK);
    bq_texto(*s_bq_spr[i], a, i, hora, data, aviso, caixa[i].x, caixa[i].y);
  }

  // Texto que mudou: a caixa e apagada (a animacao se refaz nela) e repintada.
  bq_ret_t extras[BQ_N];
  int n_extras = 0;
  if (completo) {
    bq_anim_iniciar(a);
  } else {
    for (int i = 0; i < BQ_N; ++i) { if (mudou[i]) { extras[n_extras++] = caixa[i]; } }
  }
  bq_ret_t clips[BQ_N];
  bq_anim_quadro(d, extras, n_extras, caixa, BQ_N, clips);

  const uint16_t preto = 0;   // cor transparente do sprite
  for (int i = 0; i < BQ_N; ++i) {
    bq_ret_t clip = clips[i];
    if (mudou[i]) { clip = caixa[i]; }
    if (clip.w <= 0) { continue; }
    d.setClipRect(clip.x, clip.y, clip.w, clip.h);
    if (s_bq_spr[i]) {
      s_bq_spr[i]->pushSprite(&d, caixa[i].x, caixa[i].y, preto);
    } else {
      bq_texto(d, a, i, hora, data, aviso, 0, 0);
    }
    d.clearClipRect();
  }
  d.setTextStyle(estilo);
  tab5_fonte_corpo(d);
}

// ---------------------------------------------------------------------------
// CENTRAL DE AVISOS
// ---------------------------------------------------------------------------
// A barra so tem ~170 px para avisos na home (em pe): cabe UM resumo e uns
// pontos coloridos. Com dois ou tres apps avisando, o usuario via que havia
// mais coisa e nao tinha onde ler. Tocar a barra abre esta lista - a area
// util inteira, uma linha por app (notif_listar), na mesma ordem da barra.
// Tocar uma linha abre o app dela; tocar fora, a barra de novo, Esc ou
// deslizar para cima fecha. Enquanto ela esta aberta o app de baixo fica
// ENCOBERTO (sistema_app_encoberto): nao desenha, nao ouve, nao "le" nada.
bool s_painel_avisos = false;

static notif_item_t s_pa_itens[NOTIF_MAX_FONTES];   // o que esta NA TELA (o toque usa isto)
static int          s_pa_n = 0;
static int          s_pa_desenhadas = 0;             // quantas linhas couberam
static uint32_t     s_pa_versao = 0;
static const int    PA_TOPO = 96, PA_VAO = 14, PA_MARGEM = 16;

// Deitado a area e baixa: linha mais fina para caberem pelo menos tres.
static int pa_linha(const area_t& a) { return a.altura < 700 ? 96 : 124; }
static int pa_y(const area_t& a, int i) { return a.y + PA_TOPO + i * (pa_linha(a) + PA_VAO); }

static const app_t* app_por_nome(const char* nome)
{
  if (!nome) { return nullptr; }
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    if (strcmp(SISTEMA_APPS[i]->name, nome) == 0) { return SISTEMA_APPS[i]; }
  }
  return nullptr;
}

void painel_avisos_abrir(bool abrir)
{
  if (s_painel_avisos == abrir) { return; }
  s_painel_avisos = abrir;
  s_sujo_layout   = true;   // abre por cima / devolve o app: passada completa
}

bool painel_avisos_mudou(void) { return s_painel_avisos && notif_versao() != s_pa_versao; }

void desenhar_painel_avisos(lgfx::LovyanGFX& d, const area_t& a, bool completo)
{
  // So na passada completa (area ja apagada). Um redesenho de DADOS pedido
  // pelo app de baixo nao tem o que mudar aqui; aviso novo chega por
  // painel_avisos_mudou(), que pede a completa.
  if (!completo) { return; }
  s_pa_versao = notif_versao();
  s_pa_n = notif_listar(s_pa_itens, NOTIF_MAX_FONTES);

  const int x0 = a.x + PA_MARGEM, w = a.largura - 2 * PA_MARGEM;
  tab5_titulo(d, 28);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Notifications", x0 + 8, a.y + PA_TOPO / 2);
  tab5_fonte_corpo(d);
  tab5_texto(d, 2.6f);
  d.setTextDatum(middle_right);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString("tap outside to close", x0 + w - 8, a.y + PA_TOPO / 2);

  const int h = pa_linha(a);
  s_pa_desenhadas = 0;
  if (s_pa_n == 0) {
    tab5_texto(d, 3.4f);
    d.setTextDatum(middle_center);
    d.drawString("No notifications", a.x + a.largura / 2, a.y + a.altura / 2);
  }
  for (int i = 0; i < s_pa_n; ++i) {
    const int y = pa_y(a, i);
    // A ultima vaga fica para o rodape "+N" quando nao cabe tudo.
    if (y + h > a.y + a.altura - (i < s_pa_n - 1 ? 60 : 8)) { break; }
    const notif_item_t& it = s_pa_itens[i];
    const uint16_t cor = cor_do_nivel(it.nivel);
    d.fillRoundRect(x0, y, w, h, 18, SIS_FUNDO);
    d.fillRoundRect(x0, y, 14, h, 7, cor);

    // Contagem a direita, na cor da gravidade (como a pastilha da barra).
    char num[8];
    if (it.quantidade > 999) { snprintf(num, sizeof(num), "999+"); }
    else                     { snprintf(num, sizeof(num), "%d", it.quantidade); }
    tab5_fonte_corpo_negrito(d);
    tab5_texto(d, 3.0f);
    const int bl = (int)d.textWidth(num) + 28, ba = 44;
    const int bx = x0 + w - 20 - bl;
    d.fillRoundRect(bx, y + h / 2 - ba / 2, bl, ba, ba / 2, cor);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_BLACK, cor);
    d.drawString(num, bx + bl / 2, y + h / 2);

    // Quem (negrito) e o que (resumo). Quem abre outro app ganha a seta.
    const bool abre = app_por_nome(it.app) != nullptr;
    char quem[40];
    snprintf(quem, sizeof(quem), abre ? "%s  >" : "%s", it.name);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_WHITE, SIS_FUNDO);
    tab5_texto(d, 3.2f);
    d.drawString(quem, x0 + 34, y + h * 32 / 100);
    tab5_fonte_corpo(d);
    tab5_texto(d, 3.0f);
    d.setTextColor(TFT_LIGHTGREY, SIS_FUNDO);
    d.drawString(it.resumo[0] ? it.resumo : "", x0 + 34, y + h * 70 / 100);
    ++s_pa_desenhadas;
  }
  if (s_pa_desenhadas < s_pa_n) {
    char mais[32];
    snprintf(mais, sizeof(mais), "+%d avisos", s_pa_n - s_pa_desenhadas);
    tab5_texto(d, 3.0f);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(mais, a.x + a.largura / 2, a.y + a.altura - 30);
  }
  d.setTextDatum(top_left);
  tab5_fonte_corpo(d);
}

// Toque com a central aberta ('ty' ja corrigido). Linha = abre o app dela;
// qualquer outro lugar = fecha.
static void painel_avisos_tocar(const area_t& a, int tx, int ty)
{
  const int h = pa_linha(a);
  const app_t* destino = nullptr;
  for (int i = 0; i < s_pa_desenhadas; ++i) {
    const int y = pa_y(a, i);
    if (ty >= y && ty < y + h && tx >= a.x + PA_MARGEM && tx < a.x + a.largura - PA_MARGEM) {
      destino = app_por_nome(s_pa_itens[i].app);
      break;
    }
  }
  painel_avisos_abrir(false);
  // O app que ja esta aberto so reaparece - fechar e abrir de novo perderia
  // o que ele estava mostrando.
  if (destino && destino != s_app) { abrir_app(destino); }
}

// ---------------------------------------------------------------------------
// GESTOS
// ---------------------------------------------------------------------------
static const int GESTO_PAGINA_PX     = 120;   // arraste lateral minimo para trocar de pagina
static const int GESTO_DESBLOQUEIO_PX = 220;  // deslize para cima minimo para desbloquear

void tratar_gesto(int dx, int dy)
{
  if (s_bloqueado) {
    if (dy < -GESTO_DESBLOQUEIO_PX && abs(dy) > 2 * abs(dx)) { desbloquear_tela(); }
    return;
  }
  if (s_painel_avisos) {
    if (dy < -GESTO_PAGINA_PX && abs(dy) > 2 * abs(dx)) { painel_avisos_abrir(false); }
    return;
  }
  if (s_app) { return; }   // dentro de um app o arraste e dele
  if (abs(dx) > GESTO_PAGINA_PX && abs(dx) > 2 * abs(dy)) {
    home_mudar_pagina(dx < 0 ? +1 : -1);   // arrastar para a esquerda avanca
  }
}

// ---------------------------------------------------------------------------
// SPLASH DE BOOT - "M5Tab OS" grande no meio da tela.
// ---------------------------------------------------------------------------
// #ifdef DE PROPOSITO, nao uma preferencia em runtime:
// durante o desenvolvimento normal a gente reinicia o aparelho o tempo todo
// para testar uma mudanca, e um splash de 1,4s a cada boot vira atrito puro
// sem agregar nada - o usuario ja SABE que e o M5tabOS. A chave mora em
// kernel/internal.hpp (TAB5_MOSTRAR_SPLASH) porque core.cpp precisa ver a
// MESMA definicao para decidir se chama esta funcao.
//
// MORA NO SHELL, e a CHAMADA mora em core.cpp, depois de
// tab5_power_dpi_clock() - defeito observado: o splash piscava e sumia
// antes dos apps. Causa: desenhar antes daquela chamada
// significa desenhar antes do painel assumir o relogio final, e a troca de
// relogio DESMONTA E REMONTA o DSI, apagando o que estiver na tela.
#ifdef TAB5_MOSTRAR_SPLASH
void desenhar_splash(void)
{
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);

  // FONTE PROPRIA DO SPLASH, DE PROPOSITO DIFERENTE da tab5_fonte_titulo()
  // atual.
  // TODO titulo do sistema (barra, rotulo da home, secao
  // dentro de um app) era a Orbitron_Light_32; naquele dia ela foi trocada
  // por FreeMonoBold12pt7b em tab5_fonte_titulo() (ver o comentario grande
  // em tab5_ui.hpp) e passou a valer em todo lugar - MENOS aqui: o splash
  // continua com a Orbitron de proposito, e por isso NAO chama
  // tab5_fonte_titulo() como o resto do sistema.
  //
  // TAMANHO 2 (dobro do nativo), NAO um valor fracionario tipo 1.5 - pedido
  // foi "uns 150% maior" (ou seja, 2,5x), mas fonte bitmap-por-
  // glifo como esta so pode escalar por MULTIPLO INTEIRO sem descartar/
  // duplicar linha de pixel de um jeito irregular (ver o aviso grande sobre
  // isso em tab5_ui.hpp - foi exatamente esse defeito que fez "CONFIG" na
  // barra parecer outra fonte). Entre o 1x de antes (pequeno demais, motivo
  // deste pedido) e o 3x (passaria de 500px so no "M5Tab OS", too largo para
  // o pedido de "uns 150%"), 2x e o degrau limpo mais proximo.
  d.setFont(&fonts::Orbitron_Light_32);
  d.setTextSize(2);
  d.setTextDatum(middle_center);

  // NEGRITO FALSO, - a Orbitron_Light e
  // "light" por DESENHO (ver o aviso ali em cima), e a biblioteca so tem essa
  // variante dela (nenhuma bold, em nenhum tamanho) - nao ha fonte pronta
  // mais grossa para trocar.
  //
  // SEM COR DE FUNDO aqui (so um argumento em setTextColor = so estampa
  // pixel branco, nunca apaga o que ja esta atras) - a tela inteira ja
  // nasceu preta no fillScreen() ali em cima, entao nao falta nada para
  // apagar. Desenhando a MESMA string 5x, deslocada 1px para cada lado
  // (cruz: centro + cima/baixo/esquerda/direita), cada copia so ACRESCENTA
  // pixel branco - o traco engorda em vez de borrar.
  //
  // NO TAMANHO 1x (nativo) isto borraria em vez de engordar - foi o achado
  // documentado na Orbitron da barra de status (tab5_ui.hpp): la, 1px de
  // tela E um pixel de fonte INTEIRO, entao deslocar por 1 e pular para o
  // pixel vizinho, nao dividir o mesmo. Aqui a fonte ja esta em 2x: 1px de
  // tela e so METADE de um pixel de fonte, fino o bastante para engrossar a
  // borda sem deslocar o desenho inteiro para um vizinho visivel.
  const int cx = d.width() / 2, cy = d.height() / 2;
  d.setTextColor(TFT_WHITE);
  d.drawString("M5Tab OS", cx,     cy);
  d.drawString("M5Tab OS", cx - 1, cy);
  d.drawString("M5Tab OS", cx + 1, cy);
  d.drawString("M5Tab OS", cx,     cy - 1);
  d.drawString("M5Tab OS", cx,     cy + 1);

  // Devolve o estado global do texto - MESMA pegadinha do datum/fonte vazando
  // documentada em tab5_botao()/tab5_ui.hpp. O loop principal de core.cpp,
  // logo depois, conta com top_left e a fonte de corpo, como sempre.
  d.setTextDatum(top_left);
  tab5_fonte_corpo(d);

  vTaskDelay(pdMS_TO_TICKS(1400));
}
#endif

// ---------------------------------------------------------------------------
// Toque
// ---------------------------------------------------------------------------
void tratar_toque(int tx, int ty)
{
  // AINDA MONTANDO O SD deste app: engole o toque. Nem a navegacao ("INICIO")
  // nem o conteudo existem de verdade ainda - ao_abrir() so roda quando
  // concluir_abertura_do_sd() (core.cpp) liberar. Dura poucos frames na pratica.
  if (s_app_carregando) { return; }
  // Bloqueado: so o deslize (tratar_gesto) desbloqueia; toque simples nao faz nada.
  if (s_bloqueado) { return; }

  // sistema_area(), e nao a funcao privada area_util() de core.cpp - a mesma
  // geometria, so pela porta publica (ver o aviso em kernel/internal.hpp).
  area_t a = sistema_area();

  // Correcao aplicada UMA vez, na entrada. Daqui para baixo - barra, navegacao,
  // icones da home, apps - todo mundo ja recebe o ponto corrigido, e nenhum
  // deles precisa saber que a correcao existe.
  const int ty_cru = ty;
  ty -= TOQUE_DY;

  // CENTRAL DE AVISOS: aberta, o toque e todo dela (inclusive na barra e na
  // navegacao - qualquer toque fora de uma linha so fecha). Fechada, tocar a
  // barra de status a abre.
  if (s_painel_avisos) { painel_avisos_tocar(a, tx, ty); return; }
  // NAVEGACAO TRAVADA vale para a barra tambem: uma linha da central abre
  // outro app, e isso fecharia este por um caminho que a trava existe para
  // impedir (o Pendrive, com o disco montado no computador).
  if (ty < ALTURA_BARRA) { if (!s_nav_travada) { painel_avisos_abrir(true); } return; }

  if (s_diag_toque) {
    // (a barra de status ja voltou acima: aqui so sobra navegacao ou area)
    ESP_LOGI(TAG, "toque: cru (%d,%d) -> corrigido (%d,%d) -> app (%d,%d)%s",
             tx, ty_cru, tx, ty, tx - a.x, ty - a.y + s_rol.atual,
             (ty >= M5.Display.height() - ALTURA_NAV) ? "  [navegacao]" : "");
  }

  if (ty >= M5.Display.height() - ALTURA_NAV) {
    // SO A REGIAO DO BOTAO fecha o app agora.
    //
    // ATE AQUI, qualquer toque na faixa de navegacao inteira (96px de
    // altura, a LARGURA TODA da tela) fechava o app - so o Y era checado
    // acima, nunca o X. Na pratica isso nunca incomodou porque nao havia
    // mais nada alem do botao naquela faixa para tocar por engano - mas era
    // uma area de toque bem maior que a pilula desenhada, e abre espaco para
    // colocar informacao NAO tocavel do lado dela sem essa informacao
    // roubar o toque.
    //
    // O retangulo abaixo e EXATAMENTE o que desenhar_barra_nav() pinta -
    // NAV_BTN_L/NAV_BTN_A/NAV_BTN_Y0, ver o aviso grande junto delas.
    const int nav_y0 = M5.Display.height() - ALTURA_NAV;
    const int L = M5.Display.width();
    //
    // AREA DE TOQUE MAIOR QUE O DESENHO. A pilula encolheu com a
    // faixa (72px) e ficou justa para o dedo - errar por 10px acima ou abaixo
    // dela nao fazia nada. O toque agora vale a ALTURA INTEIRA da faixa e
    // NAV_BTN_FOLGA a mais de cada lado; o desenho continua o mesmo. Nao sobe
    // para dentro da area do app: o que esta acima da faixa e do app.
    const bool dentro_do_botao =
        tx >= L / 2 - NAV_BTN_L / 2 - NAV_BTN_FOLGA && tx < L / 2 + NAV_BTN_L / 2 + NAV_BTN_FOLGA &&
        ty >= nav_y0;

    // NAVEGACAO TRAVADA: a faixa passa a ser do app (teclado na tela). O
    // teclado encosta na barra, e um dedo que passa da fileira de baixo cai
    // aqui - entregue na ultima linha da area, vale a tecla de baixo em vez de
    // sumir. Mesma regra do toque por nivel (core.cpp).
    if (s_nav_travada) {
      if (app_vivo() && s_app->ao_tocar) { s_app->ao_tocar(tx - a.x, a.altura - 1 + s_rol.atual); }
      return;
    }
    if (dentro_do_botao && s_app) { fechar_app(); }
    return;
  }

  if (s_app) {
    // ROLAGEM SOMADA AQUI, e so aqui. O app compara contra as MESMAS constantes
    // que usou para desenhar, sem saber de rolagem nenhuma.
    if (app_vivo() && s_app->ao_tocar) { s_app->ao_tocar(tx - a.x, ty - a.y + s_rol.atual); }
    return;
  }

  const int pos = home_pos_em(a, tx, ty, false);
  if (pos >= 0) { abrir_app(sistema_home_app(pos)); }
}
