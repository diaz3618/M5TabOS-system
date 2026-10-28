// ENLACE BLE COM O NO MESHTASTIC.
//
// A outra via, alem do cabo Grove. O protocolo e o MESMO - ToRadio e
// FromRadio em protobuf - so muda o transporte:
//
//   serial (Grove):  cada mensagem vai num frame 0x94 0xC3 <tamanho> <corpo>
//   BLE:             cada mensagem e UM valor de caracteristica, sem frame
//                      toRadio   (escrita)  - o que o Tab5 manda
//                      fromRadio (leitura)  - uma mensagem por leitura; vazia = acabou
//                      fromNum   (notifica) - "tem coisa nova em fromRadio"
//
// Por isso o servico Mesh (tab5_mesh.cpp) nao sabe de BLE: ele entrega o corpo
// do ToRadio e recebe o corpo do FromRadio, e quem enquadra (serial) ou escreve
// na caracteristica (aqui) e o enlace.
//
// Com o C6L no Bluetooth o Grove fica livre - e o GPS pode ir nele, com a
// posicao indo para a malha (settings_mesh_position_interval_min()).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Sobe a tarefa do enlace: procura o no (o endereco guardado em ajustes, ou o
// primeiro Meshtastic que aparecer), conecta, pareia com o PIN e fica
// entregando o que chega. Reconecta sozinho se cair. Quem chama ja ligou o
// Bluetooth (tab5_ble_turn_on, tarefa de interface).
bool mesh_ble_iniciar(void);
// Para e desconecta. Espera a tarefa sair (ate 6 s).
bool mesh_ble_parar(void);
// A tarefa do enlace existe e ainda nao saiu (quem vai derrubar a pilha BLE
// pergunta antes: ela nao pode sumir debaixo de uma conexao em curso).
bool mesh_ble_rodando(void);

// Enfileira um ToRadio (corpo, sem o 0x94 0xC3). false sem conexao.
bool mesh_ble_enviar(const uint8_t* corpo, size_t n);
// Proximo FromRadio que chegou (corpo), esperando ate 'espera_ms'. 0 = nada.
size_t mesh_ble_receber(uint8_t* buf, size_t max, uint32_t espera_ms);
// true UMA vez por conexao nova: e a deixa para pedir a configuracao ao no.
bool mesh_ble_conectou_agora(void);

bool        mesh_ble_conectado(void);
const char* mesh_ble_descricao(void);
// O nome e o endereco do no conectado ("" sem conexao).
void        mesh_ble_no(char nome[24], char endereco[18]);

// PIN NA HORA: o no pediu o PIN e o enlace esta esperando alguem digitar (o
// no com PIN aleatorio mostra um novo a cada pareamento). Ajustes > Meshtastic
// abre o teclado sozinho; mesh_ble_digitar_pin entrega (tem ~30 s).
bool mesh_ble_esperando_pin(void);
bool mesh_ble_digitar_pin(uint32_t pin);
// Depois de um PIN recusado o enlace para e espera: isto recomeca ja (o
// proximo pareamento pergunta o PIN na hora).
void mesh_ble_tentar_de_novo(void);
