// ENLACE BLE COM O NO MESHTASTIC.
//
// A outra via, alem of cabo Grove. O protocolo e o MESMO - ToRadio e
// FromRadio em protobuf - so muda o transporte:
//
//   serial (Grove):  cada message vai num frame 0x94 0xC3 <tamanho> <body>
//   BLE:             cada message e UM valor of caracteristica, sem frame
//                      toRadio   (escrita)  - o que o Tab5 manda
//                      fromRadio (leitura)  - uma message por leitura; vazia = acabou
//                      fromNum   (notifica) - "tem coisa nova em fromRadio"
//
// Por isso o servico Mesh (tab5_mesh.cpp) nao sabe of BLE: ele entrega o body
// of ToRadio e recebe o body of FromRadio, e quem enquadra (serial) ou escreve
// in caracteristica (aqui) e o enlace.
//
// Com o C6L no Bluetooth o Grove stays livre - e o GPS pode ir nele, com a
// position indo para a malha (settings_mesh_position_interval_min()).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Sobe a tarefa of enlace: procura o no (o endereco guardado em ajustes, ou o
// first Meshtastic que aparecer), conecta, pareia com o PIN e stays
// entregando o que chega. Reconecta sozinho se cair. Quem chama ja ligou o
// Bluetooth (tab5_ble_turn_on, tarefa of interface).
bool mesh_ble_start(void);
// Para e desconecta. Espera a tarefa exit (ate 6 s).
bool mesh_ble_stop(void);
// A tarefa of enlace existe e ainda nao exited (quem vai derrubar a stack BLE
// pergunta antes: ela nao pode sumir debaixo of uma connection em progress).
bool mesh_ble_running(void);

// Enfileira one ToRadio (body, sem o 0x94 0xC3). false sem connection.
bool mesh_ble_send(const uint8_t* body, size_t n);
// Proximo FromRadio que chegou (body), esperando ate 'espera_ms'. 0 = nada.
size_t mesh_ble_receber(uint8_t* buf, size_t max, uint32_t espera_ms);
// true UMA vez por connection nova: e a deixa para request a configuracao on no.
bool mesh_ble_conectou_now(void);

bool        mesh_ble_conectado(void);
const char* mesh_ble_descricao(void);
// O name e o endereco of no conectado ("" sem connection).
void        mesh_ble_no(char name[24], char endereco[18]);

// PIN NA HORA: o no pediu o PIN e o enlace esta esperando alguem digitar (o
// no com PIN aleatorio mostra one novo a cada pareamento). Ajustes > Meshtastic
// abre o keyboard sozinho; mesh_ble_digitar_pin entrega (tem ~30 s).
bool mesh_ble_esperando_pin(void);
bool mesh_ble_digitar_pin(uint32_t pin);
// Depois of one PIN recusado o enlace para e espera: isto recomeca ja (o
// next pareamento pergunta o PIN in hora).
void mesh_ble_tentar_of_novo(void);
