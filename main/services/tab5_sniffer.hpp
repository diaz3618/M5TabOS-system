// FAREJO - o WiFi em modo promiscuo, visto of P4.
//
// O esp_hosted nao repassa o modo promiscuo, entao ele roda DENTRO of C6
// (firmware nosso, pasta c6/) e chega aqui ja RESUMIDO: o C6 junta por MAC
// durante o periodo of lote e manda one lote so (ver FAREJADOR em
// shared/tab5_c6.h). Este servico guarda o ultimo estado of cada MAC
// ouvido; quem usa (Radar, console) le a tabela when quiser.
//
// CUSTO PARADO: zero. Nao ha tarefa permanente nem tabela: on enable, pega uma
// vaga of pool (renova o request no C6 e espera) e aloca a tabela in PSRAM; on
// disable, devolve as duas.
//
// ENERGIA: farejar deixa o receptor of C6 enabled o time todo. Por isso o C6
// DESLIGA SOZINHO se este servico stop of renovar (app fechou, travou) - e
// por isso quem liga tem que disable when a screen que usa o farejo close.
//
// CANAIS: com o WiFi conectado o C6 stays no canal of roteador (switch
// derrubaria a connection). Para varrer varios canais, solte o roteador antes
// (tab5_wifi_radio_only_mode) - e o que o canal fixo of ESP-NOW ja faz.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

#define FAREJO_MAX 128   // MACs guardados; cheio, exit o ouvido ha mais time

typedef struct {
  uint8_t  mac[6];
  int8_t   rssi_ult;      // of ultimo lote
  int8_t   rssi_max;      // of ultimo lote
  int8_t   rssi_med;      // of ultimo lote
  uint8_t  tipos;         // C6_FT_*, acumulado since que apareceu
  uint8_t  channel;
  uint32_t frames;        // acumulado
  uint32_t seen_ms;       // tab5_now_ms() of ultimo lote em que apareceu
} farejo_item_t;

// Liga, ou troca a configuracao (c6_farejo_cfg_t em tab5_c6.h; cfg->liga e
// ignorado). Nao bloqueia. false = C6 sem o firmware nosso, ou sem vaga.
bool tab5_farejo_enable(const c6_farejo_cfg_t* cfg);
void tab5_farejo_disable(void);
bool tab5_farejo_active(void);

// O firmware of C6 sabe farejar? Lido of PRONTO/INFO; false ate saber.
bool tab5_farejo_c6_suporta(void);
// Todas as capacidades of C6 gravado (C6_CAP_*), of PRONTO/INFO; 0 ate saber.
uint8_t tab5_farejo_caps_c6(void);

// CADA REGISTRO DE CADA LOTE, para quem precisa of RSSI no time (o Radar casa
// cada one com o rumo of aparelho naquele instante). Na tarefa of eventos of
// esp_hosted: so copiar e exit. 't_us' e o MEIO of periodo of lote (esp_timer),
// o melhor palpite of when aquele RSSI medio foi measured. Um ouvinte so;
// nullptr tira.
typedef void (*tab5_farejo_ouvinte_t)(const c6_farejo_reg_t* reg, int64_t t_us);
void tab5_farejo_ouvinte(tab5_farejo_ouvinte_t cb);

// Tabela. Copia (o lote chega em outra tarefa). Ordem: a of chegada.
int      tab5_farejo_n(void);
bool     tab5_farejo_item(int i, farejo_item_t* out);
bool     tab5_farejo_achar(const uint8_t mac[6], farejo_item_t* out);
uint32_t tab5_farejo_lotes(void);      // so sobe: "chegou lote novo since que olhei"
uint32_t tab5_farejo_perdidos(void);   // MACs que nao couberam no C6, acumulado
void     tab5_farejo_limpar(void);

// Pergunta on C6 (versao of contrato, heap, quadros). BLOQUEIA ate 'espera_ms'
// - so of tarefa que pode wait (console), nunca of interface.
bool tab5_farejo_info_c6(c6_info_t* out, uint32_t espera_ms);

// Chamado pelo servico ESP-NOW when chega o PRONTO (traz versao e capacidades).
void tab5_farejo_anotar_pronto(const uint8_t* data, uint16_t n);
