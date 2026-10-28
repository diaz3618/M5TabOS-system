// CSI - o estado do canal WiFi entre o roteador e o Tab5, visto do P4.
//
// O QUE ELE DIZ. O CSI e a amplitude e a fase de cada subportadora OFDM de um
// quadro recebido. Com uma antena so ele NAO da direcao - mas muda muito quando
// algo se mexe no caminho entre o roteador e o Tab5: uma pessoa andando, uma
// porta abrindo, mesmo sem celular nenhum. Este servico transforma isso num
// nivel de MOVIMENTO (parado / leve / forte).
//
// DE ONDE VEM. O CSI e medido no preambulo OFDM, e o beacon do roteador em
// 2,4 GHz (1 Mbps, 11b) nao tem: sem trafego para o C6, quase nada chega. Por
// isso o servico pinga o roteador a 20 Hz enquanto ligado (o mesmo truque do
// esp-csi da Espressif) e pede ao C6 so o CSI que vem DO ROTEADOR - com o
// farejo ligado, o C6 tambem mede o CSI de quadros alheios, que nao servem de
// referencia.
//
// O APARELHO NAO PODE MEXER. Girar ou segurar o Tab5 na mao muda o CSI tanto
// quanto uma pessoa andando. Quem usa avisa (tab5_csi_aparelho_mexendo) e o
// servico congela a referencia; o nivel volta depois de o aparelho ficar parado.
//
// DONO UNICO DA MENSAGEM. O peer_data do P4 guarda UM ouvinte por msg_id, e
// registrar de novo SUBSTITUI o anterior. Por isso so este servico registra
// C6_MSG_CSI; quem mais quiser os records crus (o teste do console) usa
// tab5_csi_ouvinte().
//
// CUSTO PARADO: zero - nem tarefa, nem ping, nem pedido ao C6. Ligado: uma vaga
// do pool, o ping de 20 Hz e o receptor do C6 sempre ligado.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

// Quem pede (um bit cada, como tab5_wifi_keep_awake): o servico fica
// ligado enquanto houver um.
#define TAB5_CSI_RADAR    (1u << 0)
#define TAB5_CSI_CONSOLE  (1u << 1)

void tab5_csi_ligar(uint32_t quem);
void tab5_csi_desligar(uint32_t quem);
bool tab5_csi_ativo(void);

typedef enum {
  TAB5_CSI_DESLIGADO = 0,
  TAB5_CSI_SEM_C6,          // C6 sem o firmware nosso, ou sem CSI gravado
  TAB5_CSI_SEM_ROTEADOR,    // sem WiFi conectado: nao ha de onde medir
  TAB5_CSI_APRENDENDO,      // medindo o "parado" deste lugar (primeiros segundos)
  TAB5_CSI_APARELHO_MEXENDO,
  TAB5_CSI_MEDINDO,
} tab5_csi_fase_t;

typedef enum { TAB5_CSI_PARADO = 0, TAB5_CSI_LEVE, TAB5_CSI_FORTE } tab5_csi_nivel_t;

typedef struct {
  tab5_csi_fase_t  fase;
  tab5_csi_nivel_t nivel;      // vale em TAB5_CSI_MEDINDO
  float            razao;      // desvio agora / desvio do parado (1 = parado)
  uint16_t         por_s;      // records de CSI no ultimo segundo
  int8_t           rssi;       // do ultimo record
} tab5_csi_estado_t;

void tab5_csi_estado(tab5_csi_estado_t* out);

// O Tab5 esta se mexendo (giro, mao)? Enquanto sim, a medida fica suspensa e
// a referencia recomeca quando parar. Barato: so grava uma variavel.
void tab5_csi_aparelho_mexendo(bool sim);

// Os records crus, na tarefa de eventos do esp_hosted: so copiar e sair.
// Um ouvinte so (o ultimo a chamar); nullptr tira.
typedef void (*tab5_csi_ouvinte_t)(const c6_csi_cab_t* cab, const uint8_t* dados, uint16_t n);
void tab5_csi_ouvinte(tab5_csi_ouvinte_t cb);
