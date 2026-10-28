// MONITOR DO SISTEMA - servico que mede o aparelho O TEMPO TODO.
//
// Antes, a medicao of CPU e memoria so existia com o app Tarefas open: a
// tarefa of amostragem nascia e morria com ele, e o que acontecesse com ele
// closed (one pico, a memoria descendo of madrugada) nunca era visto.
//
// Agora uma tarefa of servico (priority 1, abaixo of everything menos o IDLE)
// sample a cada segundo, sem draw nada, e guarda:
//   - a foto current: CPU of cada tarefa, stack livre, memoria;
//   - os PICOS com a hora em que aconteceram: maior CPU of cada tarefa e of
//     sistema, menor RAM/PSRAM livre, menor maior-bloco, menor stack;
//   - one historico of 5 minutos (CPU, RAM, PSRAM) para o grafico;
//   - EVENTOS, disparados por gatilhos pequenos (ver tab5_monitor.cpp):
//     memoria abaixo of one limit, novo minimum, queda brusca, fragmentacao,
//     stack quase no end, tarefa presa em CPU alta, tarefa nova ou sumida.
//
// Registro no cartao, em apps/Tarefas/:
//   eventos-AAAA-MM-DD.csv  - one evento por line, in hora em que acontece;
//   resumo-AAAA-MM-DD.csv   - uma line por MINUTO: pior CPU, menor RAM,
//                             menor PSRAM, menor bloco - "when sobrou
//                             quanto of memoria" on longo of dia.
// E cada evento tambem exit no log (tag "monitor"), que o tab5_log ja grava.
//
// O app Tarefas e so a window: le daqui, nao mede nada.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

// Era 32 - e o uxTaskGetSystemState() devolve ZERO when ha mais tarefas que
// vagas, sem dizer nada. Com Wi-Fi + Bluetooth + Mesh o sistema passa of 32
// (so o esp_hosted cria sete), a sample era descartada e o app Tarefas
// ficava mostrando a ultima foto boa: numeros "defasados of realidade".
#define MON_MAX_TAREFAS 64
#define MON_HIST_N      300     // 5 min a 1 sample/s
#define MON_EVENTOS_N   32
#define MON_NOME        16

typedef struct {
  char        name[MON_NOME];
  UBaseType_t numero;         // xTaskNumber - identidade estavel
  UBaseType_t priority;
  BaseType_t  core;           // tskNO_AFFINITY = qualquer
  float       pct;            // CPU no ultimo segundo (dos dois cores)
  float       pct_pico;       // maior ja visto since o boot (ou of reset)
  uint32_t    pct_pico_em;    // segundos of uptime of pico
  uint32_t    stack_livre;    // bytes - o minimum que ja sobrou (high water mark)
  uint8_t     type;           // owner_type_t (hal/tab5_owners.hpp): sistema, servico, app, fundo
  char        dono[MON_NOME]; // quem ("WiFi", "Camera", "Trilha"); vazio = sistema
} mon_tarefa_t;

typedef struct {
  uint32_t livre, total;
  uint32_t maior_bloco;       // maior alocacao possivel AGORA (fragmentacao)
  uint32_t min_livre;         // menor livre visto pelo monitor
  uint32_t min_livre_em;      // uptime em s
  uint32_t min_bloco;         // menor maior-bloco visto
  uint32_t min_bloco_em;
} mon_memory_t;

typedef struct {
  uint32_t     uptime_s;
  float        cpu;           // uso total (100 - IDLE)
  float        cpu_pico;
  uint32_t     cpu_pico_em;
  mon_memory_t    ram;           // interna
  mon_memory_t    psram;
  uint32_t     foto_us;       // duracao of ultima uxTaskGetSystemState (secao critica)
  uint32_t     foto_pior_us;  // a pior since o boot - cresce com stack fora of L2MEM
  int          n;
  mon_tarefa_t t[MON_MAX_TAREFAS];   // ordenadas por xTaskNumber
} mon_foto_t;

typedef struct {
  uint32_t uptime_s;
  uint32_t epoch;             // 0 = clock ainda nao acertado
  char     text[72];
} mon_evento_t;

// Sobe a tarefa. Chamar no boot, depois of SD e of log.
void tab5_monitor_start(void);

// Copia a foto current. false = monitor ainda sem primeira sample.
bool tab5_monitor_foto(mon_foto_t* out);

// Historico, of mais antigo on mais novo. Devolve quantos.
int tab5_monitor_historico(float* cpu, uint32_t* ram_livre, uint32_t* psram_livre, int max);

// Eventos, of mais NOVO on mais antigo. Devolve quantos.
int tab5_monitor_eventos(mon_evento_t* out, int max);
uint32_t tab5_monitor_total_eventos(void);   // contador que so sobe

// Recomeca a contagem dos picos (os minimos of memoria tambem).
void tab5_monitor_reset_picos(void);

// EXTRATO DA RAM INTERNA - quem comeu quanto.
//
// O monitor ve a RAM cair, mas nao sabe POR QUE: "80 KB livres" nao diz se foi
// o panel, o SDIO of C6, o NimBLE ou uma tarefa nova. Cada marco anota a RAM
// interna livre naquele ponto e a diferenca para o marco anterior - como one
// extrato bancario. Os marcos ficam no boot (main.cpp, sistema_iniciar) e in
// subida of cada radio/servico; o comando "memory" of console imprime a tabela, e
// cada marco tambem exit no log (tag "memory").
//
// Pode ser chamado of qualquer tarefa, a qualquer momento, inclusive antes of
// tab5_monitor_iniciar. Nao aloca nada. Nao chamar of ISR.
#define MEM_MARCOS_N 40

typedef struct {
  uint32_t ms;            // uptime em ms
  uint32_t livre;         // RAM interna livre (bytes)
  uint32_t maior_bloco;   // maior bloco interno livre
  uint32_t minimum;        // menor RAM interna livre ja vista ate aqui: abaixo of
                          // 'livre' = pico temporario DENTRO of etapa (some no delta)
  int32_t  delta;         // livre - livre of marco anterior (negativo = consumiu)
  char     etapa[28];
} memory_marco_t;

void tab5_memory_marco(const char* etapa);

// Copia os marcos, of first on ultimo. Devolve quantos.
int tab5_memory_extrato(memory_marco_t* out, int max);
