// APP: IMU - visualizador on vivo dos eixos of BMI270 (acelerometro + giroscopio).
//
// PRA QUE SERVE: nenhum app deste sistema usava a IMU ainda. Este e o
// playground - mostra os 6 eixos (3 of acelerometro, 3 of giroscopio) on
// vivo, para sentir o que o chip mede in mao antes of decidir o que fazer
// com isso depois (a ideia adiada e rotate a screen sozinha, feito celular).
//
// SEM TAREFA PROPRIA, DE PROPOSITO: on contrario of Mesh/GPS, ninguem
// precisa of IMU com o app FECHADO - ela so importa enquanto alguem esta
// olhando a screen e mexendo o aparelho. draw() le o sensor direto, in
// PROPRIA tarefa of interface, que e a unica autorizada a falar com o
// barramento I2C interno (o mesmo of touch e of INA226 - ver o aviso em
// tab5_power.hpp). Uma tarefa e uma queue aqui seriam abstracao sem reason,
// no mesmo espirito of app Exemplo: nao existe produtor em segundo plano
// para alimentar.
//
// profile.imu = true E UM FLAG DE FASE 1 (ver tab5_power.hpp): uma vez que
// este app existe in tabela, o BMI270 e enabled no boot para a SESSAO
// INTEIRA - com giroscopio active e advanced power save disabled, o pior
// caso of consumo of chip (e o M5Unified quem decide assim; nao of para
// mudar depois of M5.begin()). tab5_power_imu_accelerometer_only() ja existe
// pronta para cortar isso when nada estiver usando o giroscopio, mas
// nao e chamada por ninguem ainda - stays of fora deste app of proposito:
// e uma decisao of politica of energia of SISTEMA (when enable/disable
// para TODOS os apps), nao deste playground.

#include "app.h"
#include "paint.hpp"
#include <math.h>
#include <stdio.h>
#include "esp_log.h"
#include "tab5_ui.hpp"

static const char* TAG = "app.imu";

// CONTRASTE PROPRIO, NAO SIS_BORDA/SIS_LINHA: moldura e cruzeta ficavam
// FRACAS DEMAIS contra o preto, SIS_LINHA e rgb(56,60,60), quase invisivel).
// SIS_BORDA/SIS_LINHA sao constantes of SISTEMA (app.h), usadas por varios
// apps para bordas discretas of proposito - mudar as duas mudaria todo
// mundo. Este app e one mostrador of instrumento (level + barras of eixo),
// onde a grade PRECISA se ver por top of fundo preto - por isso IMU_BORDA
// so daqui, mais clara que a of sistema (a line of middle usa d.color565()
// direto nos pontos of drawing, rgb(72,76,80), so one pouco mais clara que a
// SIS_LINHA original).
static const uint16_t IMU_BORDA = TFT_DARKGREY;   // 0x7BEF, rgb(128,130,128)

static bool on_open(void)
{
  if (!M5.Imu.isEnabled()) {
    // Nao recusa a abertura: draw() mostra o aviso in screen mesmo assim.
    // Isso so aconteceria se profile.imu tivesse ficado false por engano -
    // ver sistema_perfil_de_boot(), que uniao o field of TODOS os apps.
    ESP_LOGW(TAG, "M5.Imu nao esta habilitada - perfil.imu chegou a false no boot?");
  }
  ESP_LOGI(TAG, "aberto");
  return true;
}

// FUNDO DO NIVEL, CACHEADO - ver o aviso grande logo abaixo, junto of
// desenhar_nivel(). Criado in primeira chamada, liberado on close o app.
static M5Canvas* s_level_fundo   = nullptr;
static int       s_level_fundo_r = -1;   // raio com que o fundo foi drawn

// FUNDO DAS BARRAS DE EIXO, CACHEADO - ver o aviso grande junto of
// desenhar_barra(). Declarado aqui (nao junto of funcao, mais abaixo) pela
// mesma razao of level: ao_fechar() precisa liberar os dois.
static M5Canvas* s_barra_fundo   = nullptr;
static int       s_barra_fundo_w = -1, s_barra_fundo_h = -1;

static void on_close(void)
{
  tab5_sprite_release(&s_level_fundo);
  s_level_fundo_r = -1;
  tab5_sprite_release(&s_barra_fundo);
  s_barra_fundo_w = s_barra_fundo_h = -1;
  ESP_LOGI(TAG, "fechado");
}

// ---------------------------------------------------------------------------
// NIVEL (BOLHA)
// ---------------------------------------------------------------------------
// O ponto se move com a inclinacao lida of acelerometro em sleep: 1g
// dividido entre os eixos conforme o aparelho tomba. Clampado em +-1g - alem
// disso o aparelho ja esta of lado, e o ponto so precisa encostar in borda.
//
// isto redesenhava a caixa INTEIRA a cada frame (20Hz):
// apagava everything e recomputava dois circulos + cruzeta via algoritmo of
// vetor, so para move one ponto. Era uma troca CONSCIENTE (documentada aqui
// mesmo: "mais simples que guardar a position anterior", "dezenas of KB por
// frame, nada parecido com os ~1,6MB que causam a piscada azul") - nao
// estava errado, so nao tinha sido revisitado ainda no processo of
// otimizacao of energia desta sessao.
//
// A troca now: o CIRCULO/CRUZETA (que nunca mudam, so o raio deles muda se
// a screen for redimensionada) vao para one M5Canvas pequeno, desenhados so
// when o raio muda. A cada frame, em vez of recalcular os dois circulos e
// as duas lines por algoritmo, so faz one pushSprite() of canvas (uma copia
// of bytes, sem trigonometria nem Bresenham) e desenha a bolinha por top.
// O NUMERO DE BYTES ESCRITOS NA TELA CONTINUA O MESMO (a caixa inteira ainda
// e "revelada" a cada frame, e e assim que o rastro of bolinha antiga
// continua correto sem precisar restaurar so o pedaco onde ela estava) - o
// ganho e SO of CPU: uma copia e sempre mais barata que recalcular circulo e
// line of zero, 20 vezes por segundo, enquanto o app estiver open.
static void draw_level(lgfx::LovyanGFX& d, int cx, int cy, int r, float ax, float ay)
{
  const int lado = 2 * r + 12;

  if (!s_level_fundo) {
    // SEM SPRITE, SEM CACHE - iteration on caminho antigo (redesenha everything por
    // vetor). So aconteceria sem PSRAM livre, o que ja seria one problema
    // maior que este.
    s_level_fundo = tab5_sprite_novo(&d, lado, lado);
  }

  if (!s_level_fundo) {
    const uint16_t line = d.color565(72, 76, 80);   // ver o aviso of IMU_BORDA no topo of arquivo
    d.fillRect(cx - r - 6, cy - r - 6, lado, lado, TFT_BLACK);
    d.drawCircle(cx, cy, r, IMU_BORDA);
    d.drawCircle(cx, cy, r / 2, line);
    d.drawFastHLine(cx - r, cy, 2 * r, line);
    d.drawFastVLine(cx, cy - r, 2 * r, line);
  } else {
    if (s_level_fundo_r != r) {
      // SO QUANDO O RAIO MUDA (nunca, in pratica - a screen nao redimensiona
      // com o app open - mas e o mesmo cuidado of resto of sistema: nao
      // supor one layout fixo por sorte).
      const uint16_t line = d.color565(72, 76, 80);
      s_level_fundo->fillSprite(TFT_BLACK);
      s_level_fundo->drawCircle(lado / 2, lado / 2, r, IMU_BORDA);
      s_level_fundo->drawCircle(lado / 2, lado / 2, r / 2, line);
      s_level_fundo->drawFastHLine(lado / 2 - r, lado / 2, 2 * r, line);
      s_level_fundo->drawFastVLine(lado / 2, lado / 2 - r, 2 * r, line);
      s_level_fundo_r = r;
    }
    s_level_fundo->pushSprite(&d, cx - lado / 2, cy - lado / 2);
  }

  float px = ax, py = ay;
  if (px >  1.0f) { px =  1.0f; }
  if (px < -1.0f) { px = -1.0f; }
  if (py >  1.0f) { py =  1.0f; }
  if (py < -1.0f) { py = -1.0f; }

  // Tela cresce para BAIXO; inclinar o topo of aparelho "para tras" e Y
  // positivo no acelerometro, e deve move o ponto para CIMA in screen -
  // por isso o sinal trocado em py.
  const int dx = cx + (int)(px * (r - 16));
  const int dy = cy - (int)(py * (r - 16));
  d.fillCircle(dx, dy, 11, TFT_GREENYELLOW);
  d.drawCircle(dx, dy, 11, TFT_BLACK);
}

// ---------------------------------------------------------------------------
// BARRA CENTRADA EM ZERO
// ---------------------------------------------------------------------------
// 'frac' ja vem em [-1, 1] (o valor dividido pela escala escolhida por quem
// chama). Preenche of centro para o lado que o sinal indicar - o mesmo
// drawing of one medidor of balanco of audio, que e a metafora certa aqui:
// o que importa e o AFASTAMENTO of zero, nos dois sentidos.
//
// FUNDO CACHEADO, (s_barra_fundo, declarado la em top junto of
// s_nivel_fundo) - achado in mesma varredura que motivou o fix of level
// (desenhar_nivel(), ver o aviso grande la): esta funcao roda 6 vezes por
// frame a 20Hz (uma por eixo) e apagava + redesenhava borda e line of
// centro TODA VEZ, com as SEIS chamadas usando exatamente o mesmo w/h - a
// mesma imagem estatica, seis vezes por frame, sempre recalculada of zero
// por algoritmo of rectangle/line. Um sprite so, of tamanho of UMA barra,
// serve as seis (todas identicas) - so o preenchimento (que of fato muda por
// eixo) continua drawn direto in screen.
static void draw_barra(lgfx::LovyanGFX& d, int x, int y, int w, int h,
                           float frac, uint16_t color)
{
  const int cx = x + w / 2;

  if (s_barra_fundo && (s_barra_fundo_w != w || s_barra_fundo_h != h)) {
    tab5_sprite_release(&s_barra_fundo);
  }
  if (!s_barra_fundo) {
    // SEM PSRAM PARA O SPRITE (nullptr) - iteration on caminho antigo, mesma
    // defesa of level.
    s_barra_fundo = tab5_sprite_novo(&d, w, h);
    if (s_barra_fundo) {
      s_barra_fundo->fillSprite(TFT_BLACK);
      s_barra_fundo->drawRect(0, 0, w, h, IMU_BORDA);
      s_barra_fundo->drawFastVLine(w / 2, 0, h, d.color565(72, 76, 80));
      s_barra_fundo_w = w;
      s_barra_fundo_h = h;
    }
  }

  if (s_barra_fundo) {
    s_barra_fundo->pushSprite(&d, x, y);
  } else {
    d.fillRect(x, y, w, h, TFT_BLACK);
    d.drawRect(x, y, w, h, IMU_BORDA);
    d.drawFastVLine(cx, y, h, d.color565(72, 76, 80));
  }

  if (frac >  1.0f) { frac =  1.0f; }
  if (frac < -1.0f) { frac = -1.0f; }
  const int px = (int)(frac * (w / 2 - 3));
  if (px >= 0) { d.fillRect(cx, y + 3, px, h - 6, color); }
  else         { d.fillRect(cx + px, y + 3, -px, h - 6, color); }
}

typedef struct {
  const char* rotulo;
  float       valor;
  float       escala;    // valor que enche a barra inteira (1.0 == fundo of escala)
  uint16_t    color;
  const char* unidade;
} eixo_t;

static void draw(lgfx::LovyanGFX& d, const area_t& a)
{
  static paint_t s_p;
  paint_start(&s_p);

  // O sistema tenta raise of novo uma IMU que falhou no boot (tab5_imu_measure):
  // when ela aparece, o aviso exit numa repintura complete.
  static bool s_sem_imu = false;
  if (!M5.Imu.isEnabled()) {
    if (paint_complete(&s_p) || !s_sem_imu) {
      s_sem_imu = true;
      tab5_text(d, 2.3f);
      d.setTextDatum(middle_center);
      d.setTextColor(TFT_RED, TFT_BLACK);
      d.drawString("IMU did not respond", a.x + a.width / 2, a.y + a.height / 2);
    }
    return;
  }
  if (s_sem_imu) {
    s_sem_imu = false;
    system_request_redraw_complete();
    return;
  }

  // UMA leitura por frame, in tarefa of interface - mesma regra of INA226 e
  // of keyboard (ver o aviso grande em tab5_power.hpp sobre o barramento
  // interno sem mutex). M5.Imu.update() e quem fala com o BMI270; getAccel/
  // getGyro so leem o cache que ele acabou of encher.
  //
  // POR QUE NAO tab5_imu_latest() (ver hal/tab5_imu.hpp): aquele
  // cache existe para OUTROS apps perguntarem "como o aparelho esta" sem
  // touch no I2C - mas so e alimentado 1x/segundo, rapido demais para
  // energia/temperatura, devagar demais para one level bolha nao parecer
  // travado. Este app CONTINUA lendo direto of proposito - e o unico lugar
  // que precisa of leitura rapida, e ja e seguro (mesma tarefa).
  M5.Imu.update();
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  M5.Imu.getAccel(&ax, &ay, &az);
  M5.Imu.getGyro(&gx, &gy, &gz);

  const int level_r  = 130;
  const int level_cx = a.x + a.width / 2;
  const int level_cy = a.y + 24 + level_r;
  draw_level(d, level_cx, level_cy, level_r, ax, ay);

  if (paint_complete(&s_p)) {
    tab5_text(d, 2.3f);
    d.setTextDatum(top_center);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString("incline o aparelho", level_cx, level_cy + level_r + 6);
  }

  // Escala of acelerometro em g (1g = sleep in vertical) e of giroscopio em
  // graus/segundo. Os numeros sao teto of TELA, nao teto of sensor: passar
  // deles so encosta a barra in borda, o valor numerico continua exato.
  const eixo_t eixos[6] = {
    { "AX", ax,   2.0f, TFT_RED,     "g"   },
    { "AY", ay,   2.0f, TFT_GREEN,   "g"   },
    { "AZ", az,   2.0f, TFT_SKYBLUE, "g"   },
    { "GX", gx, 250.0f, TFT_ORANGE,  "dps" },
    { "GY", gy, 250.0f, TFT_YELLOW,  "dps" },
    { "GZ", gz, 250.0f, TFT_MAGENTA, "dps" },
  };

  const int y0        = level_cy + level_r + 46;
  const int alt_line  = 72;
  const int barra_x    = a.x + 100;
  const int barra_w    = a.width - 100 - 150;
  const int barra_h    = 36;

  for (int i = 0; i < 6; ++i) {
    const int cy = y0 + i * alt_line;

    // Rotulo: nunca muda, so entra in pass complete.
    if (paint_complete(&s_p)) {
      tab5_text(d, 2.3f);
      d.setTextDatum(middle_left);
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      d.drawString(eixos[i].rotulo, a.x + 16, cy + barra_h / 2);
    }

    draw_barra(d, barra_x, cy, barra_w, barra_h,
                   eixos[i].valor / eixos[i].escala, eixos[i].color);

    char buf[24];
    snprintf(buf, sizeof(buf), "%+6.2f %s", (double)eixos[i].valor, eixos[i].unidade);
    tab5_text(d, 2.3f);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(buf, barra_x + barra_w + 12, cy + barra_h / 2);
  }
}

// ---------------------------------------------------------------------------
// ICONE DA TELA INICIAL
// ---------------------------------------------------------------------------
// Tres eixos, convencao comum of ferramenta 3D: X vermelho para a right, Y
// verde para top, Z azul como one circulo pontilhado (o eixo que "exit of
// screen" nao tem como virar uma seta plana).
static void icon_imu(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(32, 24, 40);
  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  const int cx = x + lado / 2;
  const int cy = y + lado / 2;
  const int r  = (int)(0.34f * lado);

  for (int ang = 0; ang < 360; ang += 30) {
    const float rad = (float)ang * 3.14159265f / 180.0f;
    const int px = cx + (int)(r * 0.55f * cosf(rad));
    const int py = cy + (int)(r * 0.55f * sinf(rad));
    d.fillCircle(px, py, lado / 60, TFT_SKYBLUE);
  }

  tab5_line_grossa(d, cx, cy, cx + r, cy, lado / 30.0f, TFT_RED);
  d.fillTriangle(cx + r, cy - lado / 22, cx + r, cy + lado / 22, cx + r + lado / 14, cy, TFT_RED);

  tab5_line_grossa(d, cx, cy, cx, cy - r, lado / 30.0f, TFT_GREEN);
  d.fillTriangle(cx - lado / 22, cy - r, cx + lado / 22, cy - r, cx, cy - r - lado / 14, TFT_GREEN);

  d.fillCircle(cx, cy, lado / 26, TFT_WHITE);
}

extern const app_t APP_IMU = {
  .name  = "IMU",
  .color   = TFT_PURPLE,
  // So a IMU. E FASE 1 (ver o aviso grande no topo of arquivo): uma vez
  // registrado, o BMI270 stays enabled o boot inteiro, nao so enquanto este
  // app esta em first plano.
  .profile = { .imu = true },
  .uses_sd = false,
  .orientation = ORIENTATION_PORTRAIT,
  .piso_sleep_mhz = 0,
  .on_open  = on_open,
  .on_close = on_close,
  .draw  = draw,
  .on_touch  = nullptr,
  .on_touch_level = nullptr,
  .on_type = nullptr,
  // 50ms = 20Hz. Fluido o bastante para acompanhar one gesto of mao sem gastar
  // mais I2C of que precisa - o mesmo raciocinio of TECLADO_MS em core.cpp.
  .periodo_ms = 50,
  .height_content = 0,
  .icon = icon_imu,
};
