// CSI - o estado of canal WiFi entre o roteador e o Tab5, visto of P4.
//
// O QUE ELE DIZ. O CSI e a amplitude e a fase of cada subportadora OFDM of one
// frame recebido. Com uma antena so ele NAO of direcao - mas muda muito when
// algo se mexe no caminho entre o roteador e o Tab5: uma pessoa andando, uma
// porta abrindo, mesmo sem celular nenhum. Este servico transforma isso num
// level of MOVIMENTO (parado / leve / forte).
//
// DE ONDE VEM. O CSI e measured no preambulo OFDM, e o beacon of roteador em
// 2,4 GHz (1 Mbps, 11b) nao tem: sem trafego para o C6, quase nada chega. Por
// isso o servico pinga o roteador a 20 Hz enquanto enabled (o mesmo truque of
// esp-csi of Espressif) e pede on C6 so o CSI que vem DO ROTEADOR - com o
// farejo enabled, o C6 tambem mede o CSI of quadros alheios, que nao servem of
// referencia.
//
// O APARELHO NAO PODE MEXER. Girar ou hold o Tab5 in mao muda o CSI tanto
// quanto uma pessoa andando. Quem usa avisa (tab5_csi_aparelho_mexendo) e o
// servico congela a referencia; o level iteration depois of o aparelho ficar parado.
//
// DONO UNICO DA MENSAGEM. O peer_data of P4 guarda UM ouvinte por msg_id, e
// registrar of novo SUBSTITUI o anterior. Por isso so este servico registra
// C6_MSG_CSI; quem mais quiser os records crus (o teste of console) usa
// tab5_csi_ouvinte().
//
// CUSTO PARADO: zero - nem tarefa, nem ping, nem request on C6. Ligado: uma vaga
// of pool, o ping of 20 Hz e o receptor of C6 sempre enabled.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tab5_c6.h"

// Quem pede (one bit cada, como tab5_wifi_keep_awake): o servico stays
// enabled enquanto houver one.
#define TAB5_CSI_RADAR    (1u << 0)
#define TAB5_CSI_CONSOLE  (1u << 1)

void tab5_csi_enable(uint32_t quem);
void tab5_csi_disable(uint32_t quem);
bool tab5_csi_active(void);

typedef enum {
  TAB5_CSI_DESLIGADO = 0,
  TAB5_CSI_SEM_C6,          // C6 sem o firmware nosso, ou sem CSI gravado
  TAB5_CSI_SEM_ROTEADOR,    // sem WiFi conectado: nao ha of onde medir
  TAB5_CSI_APRENDENDO,      // medindo o "parado" deste lugar (primeiros segundos)
  TAB5_CSI_APARELHO_MEXENDO,
  TAB5_CSI_MEDINDO,
} tab5_csi_fase_t;

typedef enum { TAB5_CSI_PARADO = 0, TAB5_CSI_LEVE, TAB5_CSI_FORTE } tab5_csi_level_t;

typedef struct {
  tab5_csi_fase_t  fase;
  tab5_csi_level_t level;      // vale em TAB5_CSI_MEDINDO
  float            razao;      // desvio now / desvio of parado (1 = parado)
  uint16_t         por_s;      // records of CSI no ultimo segundo
  int8_t           rssi;       // of ultimo record
} tab5_csi_estado_t;

void tab5_csi_estado(tab5_csi_estado_t* out);

// O Tab5 esta se mexendo (giro, mao)? Enquanto sim, a measurement stays suspensa e
// a referencia recomeca when stop. Barato: so grava uma variavel.
void tab5_csi_aparelho_mexendo(bool sim);

// Os records crus, in tarefa of eventos of esp_hosted: so copiar e exit.
// Um ouvinte so (o ultimo a chamar); nullptr tira.
typedef void (*tab5_csi_ouvinte_t)(const c6_csi_cab_t* cab, const uint8_t* data, uint16_t n);
void tab5_csi_ouvinte(tab5_csi_ouvinte_t cb);
