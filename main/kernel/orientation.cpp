// ORIENTACAO DA TELA - ver orientation.hpp.
#include "orientation.hpp"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "virtual_screen.hpp"
#include "tab5_imu.hpp"
#include "tab5_keyboard.hpp"
#include "tab5_time.hpp"   // tab5_now_ms - o mesmo relogio do laco da interface
#include "esp_log.h"

static const char* TAG = "orientacao";

// As duas rotacoes do M5GFX que existem no sistema. 3 e nao 1: e a que deixa
// o ExtPort1 - onde o teclado encaixa - na borda de baixo, junto de quem
// digita. Com a 1 a tela ficava de cabeca para baixo em relacao ao teclado.
static const uint8_t ROT_EM_PE   = 0;
static const uint8_t ROT_DEITADO = 3;

// ---------------------------------------------------------------------------
// A DECISAO
// ---------------------------------------------------------------------------
// So o PLANO DA TELA importa (ax, ay). O eixo y do IMU corre ao longo do lado
// comprido do painel (o mesmo referencial do nivel do app IMU), entao:
//
//   gravidade quase toda em y  -> em pe
//   gravidade quase toda em x  -> deitado
//
// O SINAL NAO ENTRA, de proposito. Os dois estados sao um por eixo, e o lado
// oposto de cada um (de cabeca para baixo, ou deitado com o teclado em cima)
// cai no mesmo estado - a tela nao vira para o outro lado, so fica. Isso e o
// que se quer, e ainda dispensa saber a polaridade de cada eixo do BMI270
// neste aparelho: errar um sinal aqui inverteria a regra inteira em
// silencio.
//
// DUAS ZONAS MORTAS, onde nada muda com o app ABERTO:
//   - aparelho na mesa: a gravidade quase toda em z, o plano da tela com
//     menos de ~0,4 g (inclinado menos de ~24 graus). Pousar o aparelho nao
//     pode girar a tela. Era 0,5 g (30 graus): segurando o aparelho recostado
//     na mao, em pe, a leitura ficava abaixo disso e um app aberto deitado
//     nunca se corrigia.
//   - na diagonal: um eixo precisa passar o outro por PLANO_RAZAO (tangente
//     de 60 graus). Entre 30 e 60 graus de giro, fica como esta - a histerese
//     que impede a tela de pular de um estado para o outro na fronteira.
static const float PLANO_MIN_G2 = 0.16f;   // (0,4 g)^2
static const float PLANO_RAZAO  = 1.73f;

// NA ABERTURA NAO HA ESTADO ANTERIOR A PROTEGER: a histerese existe para nao
// pular de um lado para o outro, e abrindo nao ha "outro lado" ainda. Vale o
// eixo que domina, com folga pequena (tangente de ~50 graus), e um plano
// menor (0,3 g, ~17 graus de inclinacao). Fora disso - na mesa, ou
// exatamente na diagonal - abre na posicao em que a tela ja esta (a home e
// sempre em pe), nunca num palpite fixo.
static const float ABRIR_MIN_G2 = 0.09f;   // (0,3 g)^2
static const float ABRIR_RAZAO  = 1.2f;

// TEMPO. Uma amostra a cada 200 ms (5 Hz) e a pose precisa se repetir em 3
// seguidas: ~0,6 s parado na postura nova antes de girar. Um balanco da mao
// nao gira; virar o aparelho de proposito gira logo. Depois de girar, 1,5 s
// de carencia: a passada completa leva um quadro, e trocar de novo no meio
// do gesto seria pior que esperar.
static const uint32_t AMOSTRA_MS   = 200;
static const int      SEGUIDAS     = 3;
static const uint32_t CARENCIA_MS  = 1500;

static orientacao_t s_modo      = ORI_EM_PE;   // o do app aberto
static uint32_t     s_ultima_ms = 0;
static uint32_t     s_girou_ms  = 0;
static int          s_cand      = -1;          // a rotacao que esta pedindo passagem
static int          s_seguidas  = 0;

// A rotacao que a postura de AGORA pede, ou -1 (zona morta, IMU sem leitura).
static int rotacao_pedida(tab5_imu_data_t* m)
{
  // Teclado fisico encaixado: deitado, sempre. Em pe ele ficaria pendurado
  // na lateral, e girar no colo enquanto se digita seria o pior defeito.
  if (tab5_keyboard_present()) { return ROT_DEITADO; }
  if (!tab5_imu_latest(m)) { return -1; }
  const float ax = fabsf(m->ax), ay = fabsf(m->ay);
  if (ax * ax + ay * ay < PLANO_MIN_G2) { return -1; }
  if (ay > ax * PLANO_RAZAO) { return ROT_EM_PE; }
  if (ax > ay * PLANO_RAZAO) { return ROT_DEITADO; }
  return -1;
}

// UMA LEITURA EM QUE DA PARA CONFIAR, para a abertura. O cache do IMU e
// atualizado 1x/s e pode estar velho; e a primeira leitura depois de o chip
// ficar quieto pode vir zerada (o M5Unified so atualiza os valores quando ha
// dado novo, e no modo de economia do BMI270 o dado novo demora). Uma leitura
// zerada caia na zona morta, e a tela abria deitada com o aparelho em pe.
// Aqui: ate 6 leituras, 10 ms entre elas, ate o modulo da gravidade fazer
// sentido (0,6 a 1,4 g - parado, so a gravidade). ~60 ms no pior caso, uma vez
// por abertura de app.
static bool ler_postura_fresca(tab5_imu_data_t* m)
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

// A rotacao para ABRIR (ver ABRIR_RAZAO), ou -1 para ficar como a tela esta.
static int rotacao_para_abrir(tab5_imu_data_t* m)
{
  if (tab5_keyboard_present()) { return ROT_DEITADO; }
  if (!ler_postura_fresca(m)) { return -1; }
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
  tela_virtual_seguir_rotacao();
}

void orientacao_ao_abrir(const app_t* app)
{
  s_modo     = app->orientacao;
  s_cand     = -1;
  s_seguidas = 0;
  s_ultima_ms = s_girou_ms = tab5_now_ms();

  uint8_t rot = ROT_EM_PE;
  tab5_imu_data_t m = { };
  int p = -1;
  if (s_modo == ORI_DEITADO) {
    rot = ROT_DEITADO;
  } else if (s_modo == ORI_SEGUE_IMU) {
    // A postura de agora, medida agora (ver ler_postura_fresca). Sem decisao
    // clara, abre como a tela ja esta - quem abre um app da home esta olhando
    // para ela em pe. Antes o padrao era deitado, e era o que aparecia sempre
    // que a leitura falhava.
    p = rotacao_para_abrir(&m);
    const uint8_t agora = M5.Display.getRotation();
    rot = (p >= 0) ? (uint8_t)p : (agora == ROT_DEITADO ? ROT_DEITADO : ROT_EM_PE);
  }
  aplicar(rot);
  if (s_modo == ORI_SEGUE_IMU) {
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

void orientacao_ao_fechar(void)
{
  s_modo = ORI_EM_PE;
  if (M5.Display.getRotation() != ROT_EM_PE) {
    aplicar(ROT_EM_PE);
    ESP_LOGI(TAG, "tela de volta em pe (%dx%d)", M5.Display.width(), M5.Display.height());
  }
}

uint32_t orientacao_prazo_ms(uint32_t agora)
{
  if (s_modo != ORI_SEGUE_IMU) { return UINT32_MAX; }
  const uint32_t dec = agora - s_ultima_ms;
  return (dec < AMOSTRA_MS) ? (AMOSTRA_MS - dec) : 0;
}

bool orientacao_vigiar(uint32_t agora, bool pode_girar)
{
  if (s_modo != ORI_SEGUE_IMU) { return false; }
  if ((agora - s_ultima_ms) < AMOSTRA_MS) { return false; }
  s_ultima_ms = agora;

  tab5_imu_measure();
  tab5_imu_data_t m = { };
  const int p = rotacao_pedida(&m);
  if (p < 0 || p == (int)M5.Display.getRotation()) {
    s_cand = -1;
    s_seguidas = 0;
    return false;
  }
  if (p == s_cand) { ++s_seguidas; }
  else             { s_cand = p; s_seguidas = 1; }

  if (s_seguidas < SEGUIDAS || !pode_girar || (agora - s_girou_ms) < CARENCIA_MS) { return false; }

  aplicar((uint8_t)p);
  s_girou_ms = agora;
  s_cand = -1;
  s_seguidas = 0;
  // A gravidade junto: e o que confirma no log, sem adivinhar, que o eixo e o
  // limiar estao certos neste aparelho.
  ESP_LOGI(TAG, "girou para %s (%dx%d)  ax=%+.2f ay=%+.2f az=%+.2f%s",
           p == ROT_EM_PE ? "em pe" : "deitado", M5.Display.width(), M5.Display.height(),
           (double)m.ax, (double)m.ay, (double)m.az, tab5_keyboard_present() ? "  (teclado encaixado)" : "");
  return true;
}
