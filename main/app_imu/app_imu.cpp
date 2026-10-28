// APP: IMU - visualizador ao vivo dos eixos do BMI270 (acelerometro + giroscopio).
//
// PRA QUE SERVE: nenhum app deste sistema usava a IMU ainda. Este e o
// playground - mostra os 6 eixos (3 do acelerometro, 3 do giroscopio) ao
// vivo, para sentir o que o chip mede na mao antes de decidir o que fazer
// com isso depois (a ideia adiada e girar a tela sozinha, feito celular).
//
// SEM TAREFA PROPRIA, DE PROPOSITO: ao contrario do Mesh/GPS, ninguem
// precisa da IMU com o app FECHADO - ela so importa enquanto alguem esta
// olhando a tela e mexendo o aparelho. desenhar() le o sensor direto, na
// PROPRIA tarefa de interface, que e a unica autorizada a falar com o
// barramento I2C interno (o mesmo do touch e do INA226 - ver o aviso em
// tab5_power.hpp). Uma tarefa e uma queue aqui seriam abstracao sem motivo,
// no mesmo espirito do app Exemplo: nao existe produtor em segundo plano
// para alimentar.
//
// perfil.imu = true E UM FLAG DE FASE 1 (ver tab5_power.hpp): uma vez que
// este app existe na tabela, o BMI270 e ligado no boot para a SESSAO
// INTEIRA - com giroscopio ativo e advanced power save desligado, o pior
// caso de consumo do chip (e o M5Unified quem decide assim; nao da para
// mudar depois do M5.begin()). tab5_power_imu_accelerometer_only() ja existe
// pronta para cortar isso quando nada estiver usando o giroscopio, mas
// nao e chamada por ninguem ainda - fica de fora deste app de proposito:
// e uma decisao de politica de energia do SISTEMA (quando ligar/desligar
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
// SIS_BORDA/SIS_LINHA sao constantes do SISTEMA (app.h), usadas por varios
// apps para bordas discretas de proposito - mudar as duas mudaria todo
// mundo. Este app e um mostrador de instrumento (nivel + barras de eixo),
// onde a grade PRECISA se ver por cima do fundo preto - por isso IMU_BORDA
// so daqui, mais clara que a do sistema (a linha do meio usa d.color565()
// direto nos pontos de desenho, rgb(72,76,80), so um pouco mais clara que a
// SIS_LINHA original).
static const uint16_t IMU_BORDA = TFT_DARKGREY;   // 0x7BEF, rgb(128,130,128)

static bool ao_abrir(void)
{
  if (!M5.Imu.isEnabled()) {
    // Nao recusa a abertura: desenhar() mostra o aviso na tela mesmo assim.
    // Isso so aconteceria se perfil.imu tivesse ficado false por engano -
    // ver sistema_perfil_de_boot(), que uniao o campo de TODOS os apps.
    ESP_LOGW(TAG, "M5.Imu nao esta habilitada - perfil.imu chegou a false no boot?");
  }
  ESP_LOGI(TAG, "aberto");
  return true;
}

// FUNDO DO NIVEL, CACHEADO - ver o aviso grande logo abaixo, junto de
// desenhar_nivel(). Criado na primeira chamada, liberado ao fechar o app.
static M5Canvas* s_nivel_fundo   = nullptr;
static int       s_nivel_fundo_r = -1;   // raio com que o fundo foi desenhado

// FUNDO DAS BARRAS DE EIXO, CACHEADO - ver o aviso grande junto de
// desenhar_barra(). Declarado aqui (nao junto da funcao, mais abaixo) pela
// mesma razao do nivel: ao_fechar() precisa liberar os dois.
static M5Canvas* s_barra_fundo   = nullptr;
static int       s_barra_fundo_w = -1, s_barra_fundo_h = -1;

static void ao_fechar(void)
{
  tab5_sprite_soltar(&s_nivel_fundo);
  s_nivel_fundo_r = -1;
  tab5_sprite_soltar(&s_barra_fundo);
  s_barra_fundo_w = s_barra_fundo_h = -1;
  ESP_LOGI(TAG, "fechado");
}

// ---------------------------------------------------------------------------
// NIVEL (BOLHA)
// ---------------------------------------------------------------------------
// O ponto se move com a inclinacao lida do acelerometro em repouso: 1g
// dividido entre os eixos conforme o aparelho tomba. Clampado em +-1g - alem
// disso o aparelho ja esta de lado, e o ponto so precisa encostar na borda.
//
// isto redesenhava a caixa INTEIRA a cada frame (20Hz):
// apagava tudo e recomputava dois circulos + cruzeta via algoritmo de
// vetor, so para mover um ponto. Era uma troca CONSCIENTE (documentada aqui
// mesmo: "mais simples que guardar a posicao anterior", "dezenas de KB por
// frame, nada parecido com os ~1,6MB que causam a piscada azul") - nao
// estava errado, so nao tinha sido revisitado ainda no processo de
// otimizacao de energia desta sessao.
//
// A troca agora: o CIRCULO/CRUZETA (que nunca mudam, so o raio deles muda se
// a tela for redimensionada) vao para um M5Canvas pequeno, desenhados so
// quando o raio muda. A cada frame, em vez de recalcular os dois circulos e
// as duas linhas por algoritmo, so faz um pushSprite() do canvas (uma copia
// de bytes, sem trigonometria nem Bresenham) e desenha a bolinha por cima.
// O NUMERO DE BYTES ESCRITOS NA TELA CONTINUA O MESMO (a caixa inteira ainda
// e "revelada" a cada frame, e e assim que o rastro da bolinha antiga
// continua correto sem precisar restaurar so o pedaco onde ela estava) - o
// ganho e SO de CPU: uma copia e sempre mais barata que recalcular circulo e
// linha do zero, 20 vezes por segundo, enquanto o app estiver aberto.
static void desenhar_nivel(lgfx::LovyanGFX& d, int cx, int cy, int r, float ax, float ay)
{
  const int lado = 2 * r + 12;

  if (!s_nivel_fundo) {
    // SEM SPRITE, SEM CACHE - volta ao caminho antigo (redesenha tudo por
    // vetor). So aconteceria sem PSRAM livre, o que ja seria um problema
    // maior que este.
    s_nivel_fundo = tab5_sprite_novo(&d, lado, lado);
  }

  if (!s_nivel_fundo) {
    const uint16_t linha = d.color565(72, 76, 80);   // ver o aviso de IMU_BORDA no topo do arquivo
    d.fillRect(cx - r - 6, cy - r - 6, lado, lado, TFT_BLACK);
    d.drawCircle(cx, cy, r, IMU_BORDA);
    d.drawCircle(cx, cy, r / 2, linha);
    d.drawFastHLine(cx - r, cy, 2 * r, linha);
    d.drawFastVLine(cx, cy - r, 2 * r, linha);
  } else {
    if (s_nivel_fundo_r != r) {
      // SO QUANDO O RAIO MUDA (nunca, na pratica - a tela nao redimensiona
      // com o app aberto - mas e o mesmo cuidado do resto do sistema: nao
      // supor um layout fixo por sorte).
      const uint16_t linha = d.color565(72, 76, 80);
      s_nivel_fundo->fillSprite(TFT_BLACK);
      s_nivel_fundo->drawCircle(lado / 2, lado / 2, r, IMU_BORDA);
      s_nivel_fundo->drawCircle(lado / 2, lado / 2, r / 2, linha);
      s_nivel_fundo->drawFastHLine(lado / 2 - r, lado / 2, 2 * r, linha);
      s_nivel_fundo->drawFastVLine(lado / 2, lado / 2 - r, 2 * r, linha);
      s_nivel_fundo_r = r;
    }
    s_nivel_fundo->pushSprite(&d, cx - lado / 2, cy - lado / 2);
  }

  float px = ax, py = ay;
  if (px >  1.0f) { px =  1.0f; }
  if (px < -1.0f) { px = -1.0f; }
  if (py >  1.0f) { py =  1.0f; }
  if (py < -1.0f) { py = -1.0f; }

  // Tela cresce para BAIXO; inclinar o topo do aparelho "para tras" e Y
  // positivo no acelerometro, e deve mover o ponto para CIMA na tela -
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
// chama). Preenche do centro para o lado que o sinal indicar - o mesmo
// desenho de um medidor de balanco de audio, que e a metafora certa aqui:
// o que importa e o AFASTAMENTO de zero, nos dois sentidos.
//
// FUNDO CACHEADO, (s_barra_fundo, declarado la em cima junto de
// s_nivel_fundo) - achado na mesma varredura que motivou o fix do nivel
// (desenhar_nivel(), ver o aviso grande la): esta funcao roda 6 vezes por
// frame a 20Hz (uma por eixo) e apagava + redesenhava borda e linha de
// centro TODA VEZ, com as SEIS chamadas usando exatamente o mesmo w/h - a
// mesma imagem estatica, seis vezes por frame, sempre recalculada do zero
// por algoritmo de retangulo/linha. Um sprite so, do tamanho de UMA barra,
// serve as seis (todas identicas) - so o preenchimento (que de fato muda por
// eixo) continua desenhado direto na tela.
static void desenhar_barra(lgfx::LovyanGFX& d, int x, int y, int w, int h,
                           float frac, uint16_t cor)
{
  const int cx = x + w / 2;

  if (s_barra_fundo && (s_barra_fundo_w != w || s_barra_fundo_h != h)) {
    tab5_sprite_soltar(&s_barra_fundo);
  }
  if (!s_barra_fundo) {
    // SEM PSRAM PARA O SPRITE (nullptr) - volta ao caminho antigo, mesma
    // defesa do nivel.
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
  if (px >= 0) { d.fillRect(cx, y + 3, px, h - 6, cor); }
  else         { d.fillRect(cx + px, y + 3, -px, h - 6, cor); }
}

typedef struct {
  const char* rotulo;
  float       valor;
  float       escala;    // valor que enche a barra inteira (1.0 == fundo de escala)
  uint16_t    cor;
  const char* unidade;
} eixo_t;

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  static pintura_t s_p;
  pintura_iniciar(&s_p);

  // O sistema tenta subir de novo uma IMU que falhou no boot (tab5_imu_measure):
  // quando ela aparece, o aviso sai numa repintura completa.
  static bool s_sem_imu = false;
  if (!M5.Imu.isEnabled()) {
    if (pintura_completa(&s_p) || !s_sem_imu) {
      s_sem_imu = true;
      tab5_texto(d, 2.3f);
      d.setTextDatum(middle_center);
      d.setTextColor(TFT_RED, TFT_BLACK);
      d.drawString("IMU não respondeu", a.x + a.largura / 2, a.y + a.altura / 2);
    }
    return;
  }
  if (s_sem_imu) {
    s_sem_imu = false;
    sistema_pedir_redesenho_completo();
    return;
  }

  // UMA leitura por frame, na tarefa de interface - mesma regra do INA226 e
  // do teclado (ver o aviso grande em tab5_power.hpp sobre o barramento
  // interno sem mutex). M5.Imu.update() e quem fala com o BMI270; getAccel/
  // getGyro so leem o cache que ele acabou de encher.
  //
  // POR QUE NAO tab5_imu_latest() (ver hal/tab5_imu.hpp): aquele
  // cache existe para OUTROS apps perguntarem "como o aparelho esta" sem
  // tocar no I2C - mas so e alimentado 1x/segundo, rapido demais para
  // energia/temperatura, devagar demais para um nivel bolha nao parecer
  // travado. Este app CONTINUA lendo direto de proposito - e o unico lugar
  // que precisa da leitura rapida, e ja e seguro (mesma tarefa).
  M5.Imu.update();
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  M5.Imu.getAccel(&ax, &ay, &az);
  M5.Imu.getGyro(&gx, &gy, &gz);

  const int nivel_r  = 130;
  const int nivel_cx = a.x + a.largura / 2;
  const int nivel_cy = a.y + 24 + nivel_r;
  desenhar_nivel(d, nivel_cx, nivel_cy, nivel_r, ax, ay);

  if (pintura_completa(&s_p)) {
    tab5_texto(d, 2.3f);
    d.setTextDatum(top_center);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString("incline o aparelho", nivel_cx, nivel_cy + nivel_r + 6);
  }

  // Escala do acelerometro em g (1g = repouso na vertical) e do giroscopio em
  // graus/segundo. Os numeros sao teto de TELA, nao teto do sensor: passar
  // deles so encosta a barra na borda, o valor numerico continua exato.
  const eixo_t eixos[6] = {
    { "AX", ax,   2.0f, TFT_RED,     "g"   },
    { "AY", ay,   2.0f, TFT_GREEN,   "g"   },
    { "AZ", az,   2.0f, TFT_SKYBLUE, "g"   },
    { "GX", gx, 250.0f, TFT_ORANGE,  "dps" },
    { "GY", gy, 250.0f, TFT_YELLOW,  "dps" },
    { "GZ", gz, 250.0f, TFT_MAGENTA, "dps" },
  };

  const int y0        = nivel_cy + nivel_r + 46;
  const int alt_linha  = 72;
  const int barra_x    = a.x + 100;
  const int barra_w    = a.largura - 100 - 150;
  const int barra_h    = 36;

  for (int i = 0; i < 6; ++i) {
    const int cy = y0 + i * alt_linha;

    // Rotulo: nunca muda, so entra na passada completa.
    if (pintura_completa(&s_p)) {
      tab5_texto(d, 2.3f);
      d.setTextDatum(middle_left);
      d.setTextColor(TFT_WHITE, TFT_BLACK);
      d.drawString(eixos[i].rotulo, a.x + 16, cy + barra_h / 2);
    }

    desenhar_barra(d, barra_x, cy, barra_w, barra_h,
                   eixos[i].valor / eixos[i].escala, eixos[i].cor);

    char buf[24];
    snprintf(buf, sizeof(buf), "%+6.2f %s", (double)eixos[i].valor, eixos[i].unidade);
    tab5_texto(d, 2.3f);
    d.setTextDatum(middle_left);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(buf, barra_x + barra_w + 12, cy + barra_h / 2);
  }
}

// ---------------------------------------------------------------------------
// ICONE DA TELA INICIAL
// ---------------------------------------------------------------------------
// Tres eixos, convencao comum de ferramenta 3D: X vermelho para a direita, Y
// verde para cima, Z azul como um circulo pontilhado (o eixo que "sai da
// tela" nao tem como virar uma seta plana).
static void icone_imu(lgfx::LovyanGFX& d, int x, int y, int lado)
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

  tab5_linha_grossa(d, cx, cy, cx + r, cy, lado / 30.0f, TFT_RED);
  d.fillTriangle(cx + r, cy - lado / 22, cx + r, cy + lado / 22, cx + r + lado / 14, cy, TFT_RED);

  tab5_linha_grossa(d, cx, cy, cx, cy - r, lado / 30.0f, TFT_GREEN);
  d.fillTriangle(cx - lado / 22, cy - r, cx + lado / 22, cy - r, cx, cy - r - lado / 14, TFT_GREEN);

  d.fillCircle(cx, cy, lado / 26, TFT_WHITE);
}

extern const app_t APP_IMU = {
  .name  = "IMU",
  .cor   = TFT_PURPLE,
  // So a IMU. E FASE 1 (ver o aviso grande no topo do arquivo): uma vez
  // registrado, o BMI270 fica ligado o boot inteiro, nao so enquanto este
  // app esta em primeiro plano.
  .perfil = { .imu = true },
  .usa_sd = false,
  .orientacao = ORI_EM_PE,
  .piso_repouso_mhz = 0,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = nullptr,
  .ao_tocar_nivel = nullptr,
  .ao_teclar = nullptr,
  // 50ms = 20Hz. Fluido o bastante para acompanhar um gesto de mao sem gastar
  // mais I2C do que precisa - o mesmo raciocinio do TECLADO_MS em core.cpp.
  .periodo_ms = 50,
  .altura_conteudo = 0,
  .icone = icone_imu,
};
