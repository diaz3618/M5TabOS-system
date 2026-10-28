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

// Uma amostra por minuto: uma descarga completa (5 a 15 horas) rende de 300 a
// 900 pontos, resolucao de sobra para tracar a curva e calibrar a bateria.
// A cada 10 minutos vai para o cartao.
static const uint32_t AMOSTRA_MS = 60000;
static const int      LOTE       = 5;       // amostras por descarga (era 10; o grafico HOJE do app Energia le daqui)
static const int      CAPACIDADE = 32;      // folga: se o cartao falhar, segura mais

typedef struct {
  uint32_t t;        // segundos desde o boot
  uint32_t epoch;    // hora real UTC da amostra; 0 = relogio nao acertado
  float    volts;
  float    mA;
  float    mW;
  int16_t  pct;
  int16_t  mhz;
  uint8_t  repouso;
  float    temp;     // silicio do P4, -273 quando nao medido
  char     app[12];
} amostra_t;

static amostra_t s_buf[CAPACIDADE];
static int       s_n = 0;
static uint32_t  s_gravadas = 0;
static uint32_t  s_falhas = 0;
static char      s_app[12] = "home";

void registro_app_atual(const char* nome)
{
  // Copia por valor: o ponteiro do app pode morrer quando ele fecha.
  snprintf(s_app, sizeof(s_app), "%s", nome ? nome : "home");
}

void registro_estado(int* na_ram, uint32_t* gravadas, uint32_t* falhas)
{
  if (na_ram)   { *na_ram   = s_n; }
  if (gravadas) { *gravadas = s_gravadas; }
  if (falhas)   { *falhas   = s_falhas; }
}

// ---------------------------------------------------------------------------
// SAUDE DAS TAREFAS E DO HEAP - ver a explicacao completa em energy_log.h.
// ---------------------------------------------------------------------------
// Ate 32 tarefas simultaneas - o mesmo teto que o app Tarefas ja usa
// (tarefas/app_tasks.cpp). Repetido aqui em vez de shared de
// proposito: e uma CONSTANTE, nao um estado, e as duas leituras sao
// independentes uma da outra - o app pode nao estar nem aberto.
// (O instantaneo de tarefas em tarefas.csv, a cada descarga, saiu em
// o monitor do sistema (services/tab5_monitor) mede as tarefas a
// cada segundo, com picos e eventos, e grava no cartao o que importa.)

// UM ARQUIVO POR DIA, COM A HORA REAL. Ate aqui era um
// energia.csv unico, crescendo desde sempre, com o tempo em segundos desde o
// BOOT - impossivel recortar "o dia" dele. Agora:
//   Sistema/energia/energia-AAAA-MM-DD.csv   (data LOCAL - o dia de quem usa)
//   ENE3,<epoch UTC>,<HH:MM local>,<V>,<mA>,<mW>,<pct>,<MHz>,<repouso>,<temp>,<app>
// Sem relogio acertado (sem GPS/malha desde o boot) as amostras vao para
// energia-sem-data.csv, com epoch 0. O energia.csv antigo fica no cartao,
// intocado.
static void nome_do_dia(uint32_t epoch, char* out, size_t tam)
{
  tab5_hora_nome_do_dia(epoch, "energia-", ".csv", out, tam);
}

// Pelo gravador do sistema (hal/tab5_sd.hpp): cada amostra vira uma linha
// entregue na hora; montar, abrir e escrever e com a tarefa "cartao". O
// cabecalho vai junto e so e escrito se o arquivo do dia estiver vazio.
static void descarregar(void)
{
  if (s_n == 0) { return; }
  int ok = 0;
  for (int i = 0; i < s_n; ++i) {
    const amostra_t* a = &s_buf[i];
    char nome[32];
    nome_do_dia(a->epoch, nome, sizeof(nome));
    char hora[8] = "--:--";
    if (a->epoch) { struct tm lt; tab5_hora_local(a->epoch, &lt); snprintf(hora, sizeof(hora), "%02d:%02d", lt.tm_hour, lt.tm_min); }
    char linha[160];
    snprintf(linha, sizeof(linha), "ENE3,%lu,%s,%.3f,%.1f,%.1f,%d,%d,%d,%.1f,%s\n",
             (unsigned long)a->epoch, hora, a->volts, a->mA, a->mW,
             (int)a->pct, (int)a->mhz, (int)a->repouso, a->temp, a->app);
    if (tab5_sd_enqueue_append(TAB5_SD_POWER, nome, linha,
                       "tipo,epoch,hora,volts,mA,mW,pct,mhz,repouso,temp_c,app\n")) { ++ok; }
  }
  if (ok == 0) { s_falhas++; return; }
  s_gravadas += ok;
  s_n = 0;
}

// SEM TAREFA PROPRIA: uma amostra por minuto nao paga 4 KB de pilha parada.
// Pega carona na tarefa "cartao" (tab5_sd_carona), que ja acorda a cada 1 s e
// e quem grava no cartao de qualquer jeito; aqui so se conta o minuto.
static void registro_na_carona(void)
{
  static int64_t proxima_us = esp_timer_get_time() + (int64_t)AMOSTRA_MS * 1000;
  const int64_t agora_us = esp_timer_get_time();
  if (agora_us < proxima_us) { return; }
  proxima_us += (int64_t)AMOSTRA_MS * 1000;
  if (proxima_us <= agora_us) { proxima_us = agora_us + (int64_t)AMOSTRA_MS * 1000; }   // atrasou demais: recomeca
  {
    // Le do CACHE, nunca do I2C: o barramento e da tarefa de interface, que
    // atualiza este valor a cada segundo. Ir ao I2C daqui reproduziria a
    // corrida que ja nos custou uma tarde (tensao alternando 4.37/8.37V).
    tab5_measurement_t m = tab5_power_latest();

    if (s_n >= CAPACIDADE) {
      // Buffer cheio (cartao ausente ha muito tempo): descarta a mais antiga.
      // Perder o passado distante e melhor que perder o presente.
      memmove(&s_buf[0], &s_buf[1], sizeof(amostra_t) * (CAPACIDADE - 1));
      s_n = CAPACIDADE - 1;
    }

    amostra_t* a = &s_buf[s_n++];
    a->t       = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000;
    a->epoch = tab5_hora_epoch();
    a->volts   = m.volts;
    a->mA      = m.mA;
    a->mW      = m.mW;
    a->pct     = (int16_t)m.pct;
    a->mhz     = (int16_t)tab5_power_cpu_mhz();
    a->repouso = tab5_power_is_sleeping() ? 1 : 0;
    { float c; a->temp = tab5_sensors_temp(&c) ? c : -273.0f; }
    snprintf(a->app, sizeof(a->app), "%s", s_app);

    // Descarrega SO no lote. Nao ha gravacao antecipada de propriedade: durante
    // o desenvolvimento a placa reinicia a cada upload, e uma gravacao logo no
    // primeiro minuto encheria o arquivo de amostras soltas de sessoes que
    // duraram segundos. Para saber se o registro esta vivo sem esperar dez
    // minutos, olhe o app Energia - ele mostra quantas amostras estao na RAM.
    //
    // COM A BATERIA NO FIM, O LOTE VIRA UM - e nem toda amostra vale igual.
    //
    // A dez minutos por descarga, um desligamento por bateria leva embora ate
    // dez amostras. E logo AQUELAS dez: o fim da descarga e onde a tensao
    // despenca, o joelho da curva, a unica parte que diz quanto tempo ainda
    // resta quando a resposta importa. O resto do arquivo tem centenas de
    // amostras de bateria cheia, que se parecem todas.
    //
    // Abaixo de 15% o aparelho pode desligar a qualquer minuto, e ai o custo de
    // gravar toda vez (uma montagem de cartao por minuto) fica barato perto de
    // perder o unico trecho que nao da para reproduzir sem descarregar a
    // bateria inteira de novo.
    const bool no_fim = (m.pct >= 0 && m.pct < 15);
    if (s_n >= (no_fim ? 1 : LOTE)) { descarregar(); }
  }
}

void registro_iniciar(void)
{
  // Uma vez por minuto le o cache de energia e, de lote em lote, entrega as
  // linhas ao gravador - tudo dentro da tarefa "cartao".
  tab5_sd_piggyback(registro_na_carona);
  ESP_LOGI(TAG, "no ar: amostra a cada %lus, descarga a cada %d amostras",
           (unsigned long)(AMOSTRA_MS / 1000), LOTE);
}
