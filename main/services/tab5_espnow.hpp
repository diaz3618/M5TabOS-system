// ESP-NOW - servico do sistema.
//
// O P4 nao tem radio, e o esp_hosted nao leva o ESP-NOW: as funcoes esp_now_*
// nem linkam deste lado. Por isso o ESP-NOW roda DENTRO do C6, no firmware
// nosso (pasta c6/), e este servico conversa com ele pelo canal peer_data do
// esp_hosted - o contrato de mensagens mora em shared/tab5_c6.h.
//
// Mesmo molde do Mesh: o servico e dono do radio e do historico, avisa pela
// notificacao com o app fechado, e o app ESP-NOW e so a janela.
//
// SO EXISTE com o firmware nosso no C6 (tab5_wifi_c6_nosso). Com o de fabrica,
// o servico fica esperando, calado - nada quebra.
//
// O CANAL: ESP-NOW so chega entre aparelhos no MESMO canal WiFi. Associado a
// um roteador, o C6 fica no canal DELE - quem conversa com o Tab5 (Cardputer)
// precisa estar nesse canal. tab5_espnow_canal() diz qual e.
//
// ENERGIA: cochilando entre beacons (modem sleep) o C6 perde frames de
// ESP-NOW. O servico PEDE o receptor ligado (tab5_wifi_manter_acordado) so com
// o canal fixo, com o app aberto e a tela acesa, ou com conversa nos ultimos
// 5 min; fora disso o radio economiza como qualquer WiFi.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

#define ESPNOW_HIST 64   // mensagens guardadas em RAM (o cartao guarda tudo)

typedef enum {
  ESPNOW_ENVIO_NADA = 0,   // recebida
  ESPNOW_ENVIO_PENDENTE,   // saiu do Tab5, o C6 ainda nao confirmou
  ESPNOW_ENVIO_OK,         // o C6 transmitiu (em difusao nao ha ACK: "saiu pela antena")
  ESPNOW_ENVIO_FALHOU,
} espnow_envio_t;

typedef struct {
  uint8_t  mac[6];         // quem mandou (recebida) ou para quem (minha)
  bool     minha;
  bool     difusao;
  bool     espressif;       // chegou no formato do componente esp-now (Cardputer oficial); false = texto puro
  uint8_t  envio;          // espnow_envio_t
  uint32_t quando;         // epoch UTC; 0 = relogio ainda nao acertado
  char     texto[C6_TEXTO_MAX + 1];
} espnow_msg_t;

// Sobe a tarefa. Chamar no boot, depois do WiFi.
void tab5_espnow_iniciar(void);

// O C6 confirmou o ESP-NOW no ar (mandou o PRONTO).
bool    tab5_espnow_pronto(void);
uint8_t tab5_espnow_canal(void);
void    tab5_espnow_meu_mac(uint8_t out[6]);
bool    tab5_espnow_c6_compativel(void);   // o C6 tem o firmware nosso?

// Historico em RAM: 0 = a mais antiga. Copia (a tabela muda em outra tarefa).
int      tab5_espnow_n(void);
bool     tab5_espnow_msg(int i, espnow_msg_t* out);
uint32_t tab5_espnow_total(void);    // so sobe - para "mudou desde que olhei"

// Aparelhos distintos ouvidos neste boot, e ha quanto tempo foi o ultimo frame.
int      tab5_espnow_vistos(void);
uint32_t tab5_espnow_ms_desde_rx(void);   // UINT32_MAX = nada ainda

// CANAL. 0 = o do roteador (padrao: o WiFi continua conectado). 1..13 = canal
// FIXO: o WiFi solta o roteador (o radio so fica num canal por vez) e o
// ESP-NOW fica nesse canal - e o jeito de falar com um aparelho que nao esta
// no canal do roteador (um Cardputer sozinho fica no 1), e de o ESP-NOW
// seguir vivo fora do alcance do roteador. Nao bloqueia.
bool    tab5_espnow_definir_canal(uint8_t canal);
uint8_t tab5_espnow_canal_fixo(void);

// Envia em difusao (todo mundo no canal ouve). Nao bloqueia: entra na queue do
// servico. false = queue cheia, texto vazio, ou ESP-NOW fora do ar.
bool tab5_espnow_enviar(const char* texto);

// O app avisa que a janela abriu/fechou - com ela aberta (e a tela acesa) o
// receptor fica ligado. Ver ENERGIA acima.
void tab5_espnow_janela(bool aberta);

// Nao lidas (recebidas desde a ultima vez que alguem olhou).
int  tab5_espnow_nao_lidas(void);
void tab5_espnow_marcar_lidas(void);
