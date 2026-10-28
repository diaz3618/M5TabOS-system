// FAREJO - o WiFi em modo promiscuo, visto do P4.
//
// O esp_hosted nao repassa o modo promiscuo, entao ele roda DENTRO do C6
// (firmware nosso, pasta c6/) e chega aqui ja RESUMIDO: o C6 junta por MAC
// durante o periodo do lote e manda um lote so (ver FAREJADOR em
// shared/tab5_c6.h). Este servico guarda o ultimo estado de cada MAC
// ouvido; quem usa (Radar, console) le a tabela quando quiser.
//
// CUSTO PARADO: zero. Nao ha tarefa permanente nem tabela: ao ligar, pega uma
// vaga do pool (renova o pedido no C6 e espera) e aloca a tabela na PSRAM; ao
// desligar, devolve as duas.
//
// ENERGIA: farejar deixa o receptor do C6 ligado o tempo todo. Por isso o C6
// DESLIGA SOZINHO se este servico parar de renovar (app fechou, travou) - e
// por isso quem liga tem que desligar quando a tela que usa o farejo fechar.
//
// CANAIS: com o WiFi conectado o C6 fica no canal do roteador (trocar
// derrubaria a conexao). Para varrer varios canais, solte o roteador antes
// (tab5_wifi_modo_so_radio) - e o que o canal fixo do ESP-NOW ja faz.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

#define FAREJO_MAX 128   // MACs guardados; cheio, sai o ouvido ha mais tempo

typedef struct {
  uint8_t  mac[6];
  int8_t   rssi_ult;      // do ultimo lote
  int8_t   rssi_max;      // do ultimo lote
  int8_t   rssi_med;      // do ultimo lote
  uint8_t  tipos;         // C6_FT_*, acumulado desde que apareceu
  uint8_t  canal;
  uint32_t quadros;       // acumulado
  uint32_t visto_ms;      // tab5_agora_ms() do ultimo lote em que apareceu
} farejo_item_t;

// Liga, ou troca a configuracao (c6_farejo_cfg_t em tab5_c6.h; cfg->liga e
// ignorado). Nao bloqueia. false = C6 sem o firmware nosso, ou sem vaga.
bool tab5_farejo_ligar(const c6_farejo_cfg_t* cfg);
void tab5_farejo_desligar(void);
bool tab5_farejo_ativo(void);

// O firmware do C6 sabe farejar? Lido do PRONTO/INFO; false ate saber.
bool tab5_farejo_c6_suporta(void);
// Todas as capacidades do C6 gravado (C6_CAP_*), do PRONTO/INFO; 0 ate saber.
uint8_t tab5_farejo_caps_c6(void);

// CADA REGISTRO DE CADA LOTE, para quem precisa do RSSI no tempo (o Radar casa
// cada um com o rumo do aparelho naquele instante). Na tarefa de eventos do
// esp_hosted: so copiar e sair. 't_us' e o MEIO do periodo do lote (esp_timer),
// o melhor palpite de quando aquele RSSI medio foi medido. Um ouvinte so;
// nullptr tira.
typedef void (*tab5_farejo_ouvinte_t)(const c6_farejo_reg_t* reg, int64_t t_us);
void tab5_farejo_ouvinte(tab5_farejo_ouvinte_t cb);

// Tabela. Copia (o lote chega em outra tarefa). Ordem: a de chegada.
int      tab5_farejo_n(void);
bool     tab5_farejo_item(int i, farejo_item_t* out);
bool     tab5_farejo_achar(const uint8_t mac[6], farejo_item_t* out);
uint32_t tab5_farejo_lotes(void);      // so sobe: "chegou lote novo desde que olhei"
uint32_t tab5_farejo_perdidos(void);   // MACs que nao couberam no C6, acumulado
void     tab5_farejo_limpar(void);

// Pergunta ao C6 (versao do contrato, heap, quadros). BLOQUEIA ate 'espera_ms'
// - so de tarefa que pode esperar (console), nunca da interface.
bool tab5_farejo_info_c6(c6_info_t* out, uint32_t espera_ms);

// Chamado pelo servico ESP-NOW quando chega o PRONTO (traz versao e capacidades).
void tab5_farejo_anotar_pronto(const uint8_t* dados, uint16_t n);
