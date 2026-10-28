// Sensores de ambiente do proprio aparelho.
//
// NAO HA SENSOR DE UMIDADE NO TAB5. Procurei: o M5Unified nao declara nenhum
// para esta placa, e o barramento interno so tem o toque (GT911), o INA226 de
// energia, a IMU e os expansores. Umidade exigiria um modulo no Grove - e o
// Grove ja tem dono.
//
// TEMPERATURA existe, com uma ressalva que precisa acompanhar o numero: e a
// temperatura do SILICIO do ESP32-P4, nao a do ar. O chip se aquece, entao ela
// fica acima da ambiente - tipicamente bem acima com a tela ligada. Serve para
// ver o aparelho esquentando (e o efeito do DFS), nao para saber se esta frio
// na sala.
//
// Escolhido em vez da temperatura da IMU por dois motivos: nao custa transacao
// I2C nenhuma (e um ADC interno) e nao depende de a IMU estar acordada.
//
// MODULO, E NAO API DE QUEUE, POR ENQUANTO. Hoje o unico consumidor e a barra de
// status, e uma queue para um leitor so seria estrutura sem carga. A leitura ja
// e cache - quem for consumir depois le a mesma funcao, e a queue entra no dia
// em que houver quem espere por evento em vez de perguntar.
#pragma once

#include <stdbool.h>

// Liga o sensor. Uma vez, no boot. Sem isto as leituras devolvem false.
void tab5_sensores_iniciar(void);

// Le o sensor DE VERDADE e atualiza o cache. So da tarefa de interface, junto
// da amostragem de energia - e barato, mas nao e de graca.
void tab5_sensores_medir(void);

// Ultima leitura. false quando o sensor nao subiu ou ainda nao mediu.
bool tab5_sensores_temp(float* celsius);

// Media de todas as leituras desde o boot - o "normal" deste aparelho nesta
// sessao, para a leitura do momento ter com o que ser comparada. false
// enquanto nao houver nenhuma.
bool tab5_sensores_temp_media(float* celsius);
