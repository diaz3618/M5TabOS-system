// MONITOR DO SISTEMA - servico que mede o aparelho O TEMPO TODO.
//
// Antes, a medicao de CPU e memoria so existia com o app Tarefas aberto: a
// tarefa de amostragem nascia e morria com ele, e o que acontecesse com ele
// fechado (um pico, a memoria descendo de madrugada) nunca era visto.
//
// Agora uma tarefa de servico (prioridade 1, abaixo de tudo menos o IDLE)
// amostra a cada segundo, sem desenhar nada, e guarda:
//   - a foto atual: CPU de cada tarefa, stack livre, memoria;
//   - os PICOS com a hora em que aconteceram: maior CPU de cada tarefa e do
//     sistema, menor RAM/PSRAM livre, menor maior-bloco, menor stack;
//   - um historico de 5 minutos (CPU, RAM, PSRAM) para o grafico;
//   - EVENTOS, disparados por gatilhos pequenos (ver tab5_monitor.cpp):
//     memoria abaixo de um limite, novo minimo, queda brusca, fragmentacao,
//     stack quase no fim, tarefa presa em CPU alta, tarefa nova ou sumida.
//
// Registro no cartao, em apps/Tarefas/:
//   eventos-AAAA-MM-DD.csv  - um evento por linha, na hora em que acontece;
//   resumo-AAAA-MM-DD.csv   - uma linha por MINUTO: pior CPU, menor RAM,
//                             menor PSRAM, menor bloco - "quando sobrou
//                             quanto de memoria" ao longo do dia.
// E cada evento tambem sai no log (tag "monitor"), que o tab5_log ja grava.
//
// O app Tarefas e so a janela: le daqui, nao mede nada.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

// Era 32 - e o uxTaskGetSystemState() devolve ZERO quando ha mais tarefas que
// vagas, sem dizer nada. Com Wi-Fi + Bluetooth + Mesh o sistema passa de 32
// (so o esp_hosted cria sete), a amostra era descartada e o app Tarefas
// ficava mostrando a ultima foto boa: numeros "defasados da realidade".
#define MON_MAX_TAREFAS 64
#define MON_HIST_N      300     // 5 min a 1 amostra/s
#define MON_EVENTOS_N   32
#define MON_NOME        16

typedef struct {
  char        nome[MON_NOME];
  UBaseType_t numero;         // xTaskNumber - identidade estavel
  UBaseType_t prioridade;
  BaseType_t  core;           // tskNO_AFFINITY = qualquer
  float       pct;            // CPU no ultimo segundo (dos dois cores)
  float       pct_pico;       // maior ja visto desde o boot (ou do zerar)
  uint32_t    pct_pico_em;    // segundos de uptime do pico
  uint32_t    pilha_livre;    // bytes - o minimo que ja sobrou (high water mark)
  uint8_t     tipo;           // dono_tipo_t (hal/tab5_owners.hpp): sistema, servico, app, fundo
  char        dono[MON_NOME]; // quem ("WiFi", "Camera", "Trilha"); vazio = sistema
} mon_tarefa_t;

typedef struct {
  uint32_t livre, total;
  uint32_t maior_bloco;       // maior alocacao possivel AGORA (fragmentacao)
  uint32_t min_livre;         // menor livre visto pelo monitor
  uint32_t min_livre_em;      // uptime em s
  uint32_t min_bloco;         // menor maior-bloco visto
  uint32_t min_bloco_em;
} mon_mem_t;

typedef struct {
  uint32_t     uptime_s;
  float        cpu;           // uso total (100 - IDLE)
  float        cpu_pico;
  uint32_t     cpu_pico_em;
  mon_mem_t    ram;           // interna
  mon_mem_t    psram;
  uint32_t     foto_us;       // duracao da ultima uxTaskGetSystemState (secao critica)
  uint32_t     foto_pior_us;  // a pior desde o boot - cresce com pilha fora da L2MEM
  int          n;
  mon_tarefa_t t[MON_MAX_TAREFAS];   // ordenadas por xTaskNumber
} mon_foto_t;

typedef struct {
  uint32_t uptime_s;
  uint32_t epoch;             // 0 = relogio ainda nao acertado
  char     texto[72];
} mon_evento_t;

// Sobe a tarefa. Chamar no boot, depois do SD e do log.
void tab5_monitor_iniciar(void);

// Copia a foto atual. false = monitor ainda sem primeira amostra.
bool tab5_monitor_foto(mon_foto_t* out);

// Historico, do mais antigo ao mais novo. Devolve quantos.
int tab5_monitor_historico(float* cpu, uint32_t* ram_livre, uint32_t* psram_livre, int max);

// Eventos, do mais NOVO ao mais antigo. Devolve quantos.
int tab5_monitor_eventos(mon_evento_t* out, int max);
uint32_t tab5_monitor_total_eventos(void);   // contador que so sobe

// Recomeca a contagem dos picos (os minimos de memoria tambem).
void tab5_monitor_zerar_picos(void);

// EXTRATO DA RAM INTERNA - quem comeu quanto.
//
// O monitor ve a RAM cair, mas nao sabe POR QUE: "80 KB livres" nao diz se foi
// o painel, o SDIO do C6, o NimBLE ou uma tarefa nova. Cada marco anota a RAM
// interna livre naquele ponto e a diferenca para o marco anterior - como um
// extrato bancario. Os marcos ficam no boot (main.cpp, sistema_iniciar) e na
// subida de cada radio/servico; o comando "mem" do console imprime a tabela, e
// cada marco tambem sai no log (tag "mem").
//
// Pode ser chamado de qualquer tarefa, a qualquer momento, inclusive antes do
// tab5_monitor_iniciar. Nao aloca nada. Nao chamar de ISR.
#define MEM_MARCOS_N 40

typedef struct {
  uint32_t ms;            // uptime em ms
  uint32_t livre;         // RAM interna livre (bytes)
  uint32_t maior_bloco;   // maior bloco interno livre
  uint32_t minimo;        // menor RAM interna livre ja vista ate aqui: abaixo de
                          // 'livre' = pico temporario DENTRO da etapa (some no delta)
  int32_t  delta;         // livre - livre do marco anterior (negativo = consumiu)
  char     etapa[28];
} mem_marco_t;

void tab5_mem_marco(const char* etapa);

// Copia os marcos, do primeiro ao ultimo. Devolve quantos.
int tab5_mem_extrato(mem_marco_t* out, int max);
