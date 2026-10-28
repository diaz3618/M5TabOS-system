// DISTANCIA PELO SINAL - estimativa a partir do RSSI, para WiFi e Bluetooth.
//
// Modelo log-distancia, o mesmo dos apps de "encontrar meu rastreador":
//
//   d = 10 ^ ((P1m - RSSI) / (10 * n))
//
//   P1m - o RSSI esperado a 1 metro do transmissor;
//   n   - quanto o ambiente atenua (2 no espaco livre, 2,5-3,5 dentro de casa).
//
// E ESTIMATIVA, e grosseira: parede, corpo, orientacao da antena e reflexo
// mudam o RSSI em 5-10 dB, o que multiplica ou divide a distancia por ~2.
// Serve para "perto / na sala / longe" e para ver se esta chegando mais perto
// andando com o aparelho - nao para medir. Por isso a tela arredonda.
#pragma once

#include <math.h>
#include <stdio.h>
#include <stddef.h>

// P1m tipicos quando o proprio sinal nao diz: roteador (transmite ~20 dBm) e
// aparelho BLE comum (~0 dBm). A 2,4 GHz a perda no primeiro metro e ~40 dB.
#define TAB5_SINAL_P1M_WIFI   (-40)
#define TAB5_SINAL_P1M_BLE    (-59)
#define TAB5_SINAL_PERDA_1M   41     // tx_power do anuncio -> P1m
#define TAB5_SINAL_N_INTERNO  2.7f

static inline float tab5_sinal_distancia_m(int rssi, int p1m)
{
  return powf(10.0f, (float)(p1m - rssi) / (10.0f * TAB5_SINAL_N_INTERNO));
}

// "~40 cm", "~3 m", "~15 m", "> 50 m" - arredondado a um algarismo que o
// erro da estimativa ainda sustenta.
static inline void tab5_sinal_distancia_txt(int rssi, int p1m, char* out, size_t tam)
{
  const float d = tab5_sinal_distancia_m(rssi, p1m);
  if      (d < 1.0f)  { snprintf(out, tam, "~%d cm", ((int)(d * 10.0f + 0.5f)) * 10); }
  else if (d < 10.0f) { snprintf(out, tam, "~%d m", (int)(d + 0.5f)); }
  else if (d < 50.0f) { snprintf(out, tam, "~%d m", ((int)(d / 5.0f + 0.5f)) * 5); }
  else                { snprintf(out, tam, "> 50 m"); }
}
