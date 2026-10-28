// ORIENTACAO DA TELA - ver orientation.hpp.
#include "orientation.hpp"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "virtual_screen.hpp"
#include "tab5_imu.hpp"
#include "tab5_keyboard.hpp"
#include "tab5_time.hpp"   // tab5_now_ms - o mesmo clock of laco of interface
#include "esp_log.h"

static const char* TAG = "orientacao";

// As duas rotacoes of M5GFX que existem no sistema. 3 e nao 1: e a que deixa
// o ExtPort1 - onde o keyboard encaixa - in borda of bottom, junto of quem
// digita. Com a 1 a screen ficava of cabeca para bottom em relacao on keyboard.
static const uint8_t ROT_EM_PE   = 0;
static const uint8_t ROT_DEITADO = 3;

// ---------------------------------------------------------------------------
// A DECISAO
// ---------------------------------------------------------------------------
// So o PLANO DA TELA importa (ax, ay). O eixo y of IMU corre on longo of lado
// comprido of panel (o mesmo referencial of level of app IMU), entao:
//
//   gravidade quase toda em y  -> em pe
//   gravidade quase toda em x  -> deitado
//
// O SINAL NAO ENTRA, of proposito. Os dois estados sao one por eixo, e o lado
// oposto of cada one (of cabeca para bottom, ou deitado com o keyboard em top)
// cai no mesmo estado - a screen nao vira para o outro lado, so stays. Isso e o
// que se quer, e ainda dispensa saber a polaridade of cada eixo of BMI270
// neste aparelho: errar one sinal aqui inverteria a regra inteira em
// silencio.
//
// DUAS ZONAS MORTAS, onde nada muda com o app ABERTO:
//   - aparelho in mesa: a gravidade quase toda em z, o plano of screen com
//     menos of ~0,4 g (inclinado menos of ~24 graus). Pousar o aparelho nao
//     pode rotate a screen. Era 0,5 g (30 graus): segurando o aparelho recostado
//     in mao, em pe, a leitura ficava abaixo disso e one app open deitado
//     nunca se corrigia.
//   - in diagonal: one eixo precisa passar o outro por PLANO_RAZAO (tangente
//     of 60 graus). Entre 30 e 60 graus of giro, stays como esta - a histerese
//     que impede a screen of pular of one estado para o outro in fronteira.
static const float PLANO_MIN_G2 = 0.16f;   // (0,4 g)^2
static const float PLANO_RAZAO  = 1.73f;

// NA ABERTURA NAO HA ESTADO ANTERIOR A PROTEGER: a histerese existe para nao
// pular of one lado para o outro, e abrindo nao ha "outro lado" ainda. Vale o
// eixo que domina, com folga pequena (tangente of ~50 graus), e one plano
// menor (0,3 g, ~17 graus of inclinacao). Fora disso - in mesa, ou
// exatamente in diagonal - abre in position em que a screen ja esta (a home e
// sempre em pe), nunca num palpite fixo.
static const float ABRIR_MIN_G2 = 0.09f;   // (0,3 g)^2
static const float ABRIR_RAZAO  = 1.2f;

// TEMPO. Uma sample a cada 200 ms (5 Hz) e a pose precisa se repetir em 3
// seguidas: ~0,6 s parado in postura nova antes of rotate. Um balanco of mao
// nao gira; virar o aparelho of proposito gira logo. Depois of rotate, 1,5 s
// of carencia: a pass complete leva one frame, e switch of novo no middle
// of gesto seria pior que wait.
static const uint32_t AMOSTRA_MS   = 200;
static const int      SEGUIDAS     = 3;
static const uint32_t CARENCIA_MS  = 1500;

static orientation_t s_modo      = ORIENTATION_PORTRAIT;   // o of app open
static uint32_t     s_ultima_ms = 0;
static uint32_t     s_girou_ms  = 0;
static int          s_cand      = -1;          // a rotation que esta pedindo passagem
static int          s_seguidas  = 0;

// A rotation que a postura of AGORA pede, ou -1 (zona morta, IMU sem leitura).
static int rotation_requested(tab5_imu_data_t* m)
{
  // Teclado fisico encaixado: deitado, sempre. Em pe ele ficaria pendurado
  // in lateral, e rotate no colo enquanto se digita seria o pior defeito.
  if (tab5_keyboard_present()) { return ROT_DEITADO; }
  if (!tab5_imu_latest(m)) { return -1; }
  const float ax = fabsf(m->ax), ay = fabsf(m->ay);
  if (ax * ax + ay * ay < PLANO_MIN_G2) { return -1; }
  if (ay > ax * PLANO_RAZAO) { return ROT_EM_PE; }
  if (ax > ay * PLANO_RAZAO) { return ROT_DEITADO; }
  return -1;
}

// UMA LEITURA EM QUE DA PARA CONFIAR, para a abertura. O cache of IMU e
// atualizado 1x/s e pode estar velho; e a primeira leitura depois of o chip
// ficar quieto pode vir zerada (o M5Unified so atualiza os valores when ha
// dado novo, e no modo of economia of BMI270 o dado novo demora). Uma leitura
// zerada caia in zona morta, e a screen abria deitada com o aparelho em pe.
// Aqui: ate 6 leituras, 10 ms entre elas, ate o modulo of gravidade fazer
// sentido (0,6 a 1,4 g - parado, so a gravidade). ~60 ms no pior caso, uma vez
// por abertura of app.
static bool read_postura_fresca(tab5_imu_data_t* m)
{
  for (int k = 0; k < 6; ++k) {
    tab5_imu_measure();
    if (tab5_imu_latest(m)) {
      const float g2 = m->ax * m->ax + m->ay * m->ay + m->az * m->az;
      if (g2 > 0.36f && g2 < 1.96f) { return true; }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

// A rotation para ABRIR (ver ABRIR_RAZAO), ou -1 para ficar como a screen esta.
static int rotation_para_open(tab5_imu_data_t* m)
{
  if (tab5_keyboard_present()) { return ROT_DEITADO; }
  if (!read_postura_fresca(m)) { return -1; }
  const float ax = fabsf(m->ax), ay = fabsf(m->ay);
  if (ax * ax + ay * ay < ABRIR_MIN_G2) { return -1; }
  if (ay > ax * ABRIR_RAZAO) { return ROT_EM_PE; }
  if (ax > ay * ABRIR_RAZAO) { return ROT_DEITADO; }
  return -1;
}

static void aplicar(uint8_t rot)
{
  if (M5.Display.getRotation() == rot) { return; }
  M5.Display.setRotation(rot);
  virtual_screen_follow_rotation();
}

void orientation_on_open(const app_t* app)
{
  s_modo     = app->orientation;
  s_cand     = -1;
  s_seguidas = 0;
  s_ultima_ms = s_girou_ms = tab5_now_ms();

  uint8_t rot = ROT_EM_PE;
  tab5_imu_data_t m = { };
  int p = -1;
  if (s_modo == ORIENTATION_LANDSCAPE) {
    rot = ROT_DEITADO;
  } else if (s_modo == ORIENTATION_FOLLOWS_IMU) {
    // A postura of now, measurement now (ver ler_postura_fresca). Sem decisao
    // clara, abre como a screen ja esta - quem abre one app of home esta olhando
    // para ela em pe. Antes o default era deitado, e era o que aparecia sempre
    // que a leitura falhava.
    p = rotation_para_open(&m);
    const uint8_t now = M5.Display.getRotation();
    rot = (p >= 0) ? (uint8_t)p : (now == ROT_DEITADO ? ROT_DEITADO : ROT_EM_PE);
  }
  aplicar(rot);
  if (s_modo == ORIENTATION_FOLLOWS_IMU) {
    // A gravidade junto, como no giro: confirma no log por que abriu assim.
    ESP_LOGI(TAG, "'%s' abre %s, seguindo o aparelho (%dx%d)  ax=%+.2f ay=%+.2f az=%+.2f%s", app->name,
             rot == ROT_EM_PE ? "em pe" : "deitado", M5.Display.width(), M5.Display.height(),
             (double)m.ax, (double)m.ay, (double)m.az,
             tab5_keyboard_present() ? "  (teclado encaixado)" : p < 0 ? "  (sem decisao: a posicao da tela)" : "");
  } else {
    ESP_LOGI(TAG, "'%s' abre %s (%dx%d)", app->name, rot == ROT_EM_PE ? "em pe" : "deitado",
             M5.Display.width(), M5.Display.height());
  }
}

void orientation_on_close(void)
{
  s_modo = ORIENTATION_PORTRAIT;
  if (M5.Display.getRotation() != ROT_EM_PE) {
    aplicar(ROT_EM_PE);
    ESP_LOGI(TAG, "tela de volta em pe (%dx%d)", M5.Display.width(), M5.Display.height());
  }
}

uint32_t orientation_prazo_ms(uint32_t now)
{
  if (s_modo != ORIENTATION_FOLLOWS_IMU) { return UINT32_MAX; }
  const uint32_t dec = now - s_ultima_ms;
  return (dec < AMOSTRA_MS) ? (AMOSTRA_MS - dec) : 0;
}

bool orientation_vigiar(uint32_t now, bool pode_rotate)
{
  if (s_modo != ORIENTATION_FOLLOWS_IMU) { return false; }
  if ((now - s_ultima_ms) < AMOSTRA_MS) { return false; }
  s_ultima_ms = now;

  tab5_imu_measure();
  tab5_imu_data_t m = { };
  const int p = rotation_requested(&m);
  if (p < 0 || p == (int)M5.Display.getRotation()) {
    s_cand = -1;
    s_seguidas = 0;
    return false;
  }
  if (p == s_cand) { ++s_seguidas; }
  else             { s_cand = p; s_seguidas = 1; }

  if (s_seguidas < SEGUIDAS || !pode_rotate || (now - s_girou_ms) < CARENCIA_MS) { return false; }

  aplicar((uint8_t)p);
  s_girou_ms = now;
  s_cand = -1;
  s_seguidas = 0;
  // A gravidade junto: e o que confirma no log, sem adivinhar, que o eixo e o
  // limiar estao certos neste aparelho.
  ESP_LOGI(TAG, "girou para %s (%dx%d)  ax=%+.2f ay=%+.2f az=%+.2f%s",
           p == ROT_EM_PE ? "em pe" : "deitado", M5.Display.width(), M5.Display.height(),
           (double)m.ax, (double)m.ay, (double)m.az, tab5_keyboard_present() ? "  (teclado encaixado)" : "");
  return true;
}
