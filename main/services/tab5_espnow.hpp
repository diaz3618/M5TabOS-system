// ESP-NOW - servico of sistema.
//
// O P4 nao tem radio, e o esp_hosted nao leva o ESP-NOW: as funcoes esp_now_*
// nem linkam deste lado. Por isso o ESP-NOW roda DENTRO of C6, no firmware
// nosso (pasta c6/), e este servico conversa com ele pelo canal peer_data of
// esp_hosted - o contrato of mensagens mora em shared/tab5_c6.h.
//
// Mesmo molde of Mesh: o servico e dono of radio e of historico, avisa pela
// notification com o app closed, e o app ESP-NOW e so a window.
//
// SO EXISTE com o firmware nosso no C6 (tab5_wifi_c6_is_ours). Com o of fabrica,
// o servico stays esperando, calado - nada quebra.
//
// O CANAL: ESP-NOW so chega entre aparelhos no MESMO canal WiFi. Associado a
// one roteador, o C6 stays no canal DELE - quem conversa com o Tab5 (Cardputer)
// precisa estar nesse canal. tab5_espnow_canal() diz qual e.
//
// ENERGIA: cochilando entre beacons (modem sleep) o C6 perde frames of
// ESP-NOW. O servico PEDE o receptor enabled (tab5_wifi_keep_awake) so com
// o canal fixo, com o app open e a screen acesa, ou com conversa nos ultimos
// 5 min; fora disso o radio economiza como qualquer WiFi.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

#define ESPNOW_HIST 64   // mensagens guardadas em RAM (o cartao guarda everything)

typedef enum {
  ESPNOW_ENVIO_NADA = 0,   // recebida
  ESPNOW_ENVIO_PENDENTE,   // exited of Tab5, o C6 ainda nao confirmou
  ESPNOW_ENVIO_OK,         // o C6 transmitiu (em difusao nao ha ACK: "exited pela antena")
  ESPNOW_ENVIO_FALHOU,
} espnow_send_state_t;

typedef struct {
  uint8_t  mac[6];         // quem mandou (recebida) ou para quem (minha)
  bool     minha;
  bool     difusao;
  bool     espressif;       // chegou no formato of componente esp-now (Cardputer oficial); false = text puro
  uint8_t  send_state;          // espnow_envio_t
  uint32_t when;         // epoch UTC; 0 = clock ainda nao acertado
  char     text[C6_TEXTO_MAX + 1];
} espnow_msg_t;

// Sobe a tarefa. Chamar no boot, depois of WiFi.
void tab5_espnow_start(void);

// O C6 confirmou o ESP-NOW no ar (mandou o PRONTO).
bool    tab5_espnow_pronto(void);
uint8_t tab5_espnow_canal(void);
void    tab5_espnow_meu_mac(uint8_t out[6]);
bool    tab5_espnow_c6_compativel(void);   // o C6 tem o firmware nosso?

// Historico em RAM: 0 = a mais antiga. Copia (a tabela muda em outra tarefa).
int      tab5_espnow_n(void);
bool     tab5_espnow_msg(int i, espnow_msg_t* out);
uint32_t tab5_espnow_total(void);    // so sobe - para "changed since que olhei"

// Aparelhos distintos ouvidos neste boot, e ha quanto time foi o ultimo frame.
int      tab5_espnow_vistos(void);
uint32_t tab5_espnow_ms_since_rx(void);   // UINT32_MAX = nada ainda

// CANAL. 0 = o of roteador (default: o WiFi continua conectado). 1..13 = canal
// FIXO: o WiFi released o roteador (o radio so stays num canal por vez) e o
// ESP-NOW stays nesse canal - e o jeito of falar com one aparelho que nao esta
// no canal of roteador (one Cardputer sozinho stays no 1), e of o ESP-NOW
// follow vivo fora of alcance of roteador. Nao bloqueia.
bool    tab5_espnow_set_canal(uint8_t canal);
uint8_t tab5_espnow_canal_fixo(void);

// Envia em difusao (todo mundo no canal ouve). Nao bloqueia: entra in queue of
// servico. false = queue cheia, text vazio, ou ESP-NOW fora of ar.
bool tab5_espnow_send(const char* text);

// O app avisa que a window abriu/fechou - com ela open (e a screen acesa) o
// receptor stays enabled. Ver ENERGIA acima.
void tab5_espnow_window(bool open);

// Nao lidas (recebidas since a ultima vez que alguem olhou).
int  tab5_espnow_nao_lidas(void);
void tab5_espnow_marcar_lidas(void);
