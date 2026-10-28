#include "energy_log.h"
#include "tab5_power.hpp"
#include "tab5_sensors.hpp"
#include "tab5_mesh.hpp"
#include <time.h>
#include "tab5_sd.hpp"
#include "tab5_time.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "registro";

// Uma sample por minuto: uma descarga complete (5 a 15 horas) rende of 300 a
// 900 pontos, resolucao of sobra para tracar a curva e calibrar a battery.
// A cada 10 minutos vai para o cartao.
static const uint32_t AMOSTRA_MS = 60000;
static const int      LOTE       = 5;       // amostras por descarga (era 10; o grafico HOJE of app Energia le daqui)
static const int      CAPACIDADE = 32;      // folga: se o cartao falhar, segura mais

typedef struct {
  uint32_t t;        // segundos since o boot
  uint32_t epoch;    // hora real UTC of sample; 0 = clock nao acertado
  float    volts;
  float    mA;
  float    mW;
  int16_t  pct;
  int16_t  mhz;
  uint8_t  sleep;
  float    temp;     // silicio of P4, -273 when nao measured
  char     app[12];
} sample_t;

static sample_t s_buf[CAPACIDADE];
static int       s_n = 0;
static uint32_t  s_gravadas = 0;
static uint32_t  s_falhas = 0;
static char      s_app[12] = "home";

void registro_app_current(const char* name)
{
  // Copia por valor: o ponteiro of app pode morrer when ele fecha.
  snprintf(s_app, sizeof(s_app), "%s", name ? name : "home");
}

void registro_estado(int* in_ram, uint32_t* gravadas, uint32_t* falhas)
{
  if (in_ram)   { *in_ram   = s_n; }
  if (gravadas) { *gravadas = s_gravadas; }
  if (falhas)   { *falhas   = s_falhas; }
}

// ---------------------------------------------------------------------------
// SAUDE DAS TAREFAS E DO HEAP - ver a explicacao complete em energy_log.h.
// ---------------------------------------------------------------------------
// Ate 32 tarefas simultaneas - o mesmo teto que o app Tarefas ja usa
// (tarefas/app_tasks.cpp). Repetido aqui em vez of shared of
// proposito: e uma CONSTANTE, nao one estado, e as duas leituras sao
// independentes uma of outra - o app pode nao estar nem open.
// (O instantaneo of tarefas em tarefas.csv, a cada descarga, exited em
// o monitor of sistema (services/tab5_monitor) mede as tarefas a
// cada segundo, com picos e eventos, e grava no cartao o que importa.)

// UM ARQUIVO POR DIA, COM A HORA REAL. Ate aqui era one
// energia.csv unico, crescendo since sempre, com o time em segundos since o
// BOOT - impossivel recortar "o dia" dele. Agora:
//   Sistema/energia/energia-AAAA-MM-DD.csv   (data LOCAL - o dia of quem usa)
//   ENE3,<epoch UTC>,<HH:MM local>,<V>,<mA>,<mW>,<pct>,<MHz>,<sleep>,<temp>,<app>
// Sem clock acertado (sem GPS/malha since o boot) as amostras vao para
// energia-sem-data.csv, com epoch 0. O energia.csv antigo stays no cartao,
// intocado.
static void name_of_dia(uint32_t epoch, char* out, size_t size)
{
  tab5_day_filename(epoch, "energia-", ".csv", out, size);
}

// Pelo gravador of sistema (hal/tab5_sd.hpp): cada sample vira uma line
// entregue in hora; montar, open e write e com a tarefa "cartao". O
// cabecalho vai junto e so e escrito se o arquivo of dia estiver vazio.
static void descarregar(void)
{
  if (s_n == 0) { return; }
  int ok = 0;
  for (int i = 0; i < s_n; ++i) {
    const sample_t* a = &s_buf[i];
    char name[32];
    name_of_dia(a->epoch, name, sizeof(name));
    char hora[8] = "--:--";
    if (a->epoch) { struct tm lt; tab5_local_time(a->epoch, &lt); snprintf(hora, sizeof(hora), "%02d:%02d", lt.tm_hour, lt.tm_min); }
    char line[160];
    snprintf(line, sizeof(line), "ENE3,%lu,%s,%.3f,%.1f,%.1f,%d,%d,%d,%.1f,%s\n",
             (unsigned long)a->epoch, hora, a->volts, a->mA, a->mW,
             (int)a->pct, (int)a->mhz, (int)a->sleep, a->temp, a->app);
    if (tab5_sd_enqueue_append(TAB5_SD_POWER, name, line,
                       "tipo,epoch,hora,volts,mA,mW,pct,mhz,repouso,temp_c,app\n")) { ++ok; }
  }
  if (ok == 0) { s_falhas++; return; }
  s_gravadas += ok;
  s_n = 0;
}

// SEM TAREFA PROPRIA: uma sample por minuto nao paga 4 KB of stack parada.
// Pega carona in tarefa "cartao" (tab5_sd_carona), que ja acorda a cada 1 s e
// e quem grava no cartao of qualquer jeito; aqui so se conta o minuto.
static void registro_in_carona(void)
{
  static int64_t next_us = esp_timer_get_time() + (int64_t)AMOSTRA_MS * 1000;
  const int64_t now_us = esp_timer_get_time();
  if (now_us < next_us) { return; }
  next_us += (int64_t)AMOSTRA_MS * 1000;
  if (next_us <= now_us) { next_us = now_us + (int64_t)AMOSTRA_MS * 1000; }   // atrasou demais: recomeca
  {
    // Le of CACHE, nunca of I2C: o barramento e of tarefa of interface, que
    // atualiza este valor a cada segundo. Ir on I2C daqui reproduziria a
    // corrida que ja nos custou uma tarde (tensao alternando 4.37/8.37V).
    tab5_measurement_t m = tab5_power_latest();

    if (s_n >= CAPACIDADE) {
      // Buffer cheio (cartao ausente ha muito time): descarta a mais antiga.
      // Perder o passado distante e melhor que perder o presente.
      memmove(&s_buf[0], &s_buf[1], sizeof(sample_t) * (CAPACIDADE - 1));
      s_n = CAPACIDADE - 1;
    }

    sample_t* a = &s_buf[s_n++];
    a->t       = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000;
    a->epoch = tab5_time_epoch();
    a->volts   = m.volts;
    a->mA      = m.mA;
    a->mW      = m.mW;
    a->pct     = (int16_t)m.pct;
    a->mhz     = (int16_t)tab5_power_cpu_mhz();
    a->sleep = tab5_power_is_sleeping() ? 1 : 0;
    { float c; a->temp = tab5_sensors_temp(&c) ? c : -273.0f; }
    snprintf(a->app, sizeof(a->app), "%s", s_app);

    // Descarrega SO no lote. Nao ha gravacao antecipada of propriedade: durante
    // o desenvolvimento a placa reinicia a cada upload, e uma gravacao logo no
    // first minuto encheria o arquivo of amostras soltas of sessoes que
    // duraram segundos. Para saber se o registro esta vivo sem wait dez
    // minutos, olhe o app Energia - ele mostra quantas amostras estao in RAM.
    //
    // COM A BATERIA NO FIM, O LOTE VIRA UM - e nem toda sample vale igual.
    //
    // A dez minutos por descarga, one desligamento por battery leva embora ate
    // dez amostras. E logo AQUELAS dez: o end of descarga e onde a tensao
    // despenca, o joelho of curva, a unica parte que diz quanto time ainda
    // resta when a resposta importa. O resto of arquivo tem centenas of
    // amostras of battery cheia, que se parecem todas.
    //
    // Abaixo of 15% o aparelho pode disable a qualquer minuto, e ai o custo of
    // gravar toda vez (uma montagem of cartao por minuto) stays barato perto of
    // perder o unico trecho que nao of para reproduzir sem descarregar a
    // battery inteira of novo.
    const bool no_end = (m.pct >= 0 && m.pct < 15);
    if (s_n >= (no_end ? 1 : LOTE)) { descarregar(); }
  }
}

void registro_start(void)
{
  // Uma vez por minuto le o cache of energia e, of lote em lote, entrega as
  // lines on gravador - everything dentro of tarefa "cartao".
  tab5_sd_piggyback(registro_in_carona);
  ESP_LOGI(TAG, "no ar: amostra a cada %lus, descarga a cada %d amostras",
           (unsigned long)(AMOSTRA_MS / 1000), LOTE);
}
