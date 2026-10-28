// Radio LoRa SX1262 encaixado no M5-Bus.
//
// NAO E MESHTASTIC. Este HAL fala SPI diretamente com o transceptor nu do
// Module 13.2 LoRa-1262; o servico tab5_mesh fala com outro aparelho, ja com
// firmware Meshtastic, por UART/Bluetooth.
//
// O modulo espalha seus sinais por tres bancos de chaves, CONFERIDOS no
// esquematico do modulo (SCH_Module13.2_LoRa1262_V1.0) e cruzados com o
// esquematico do proprio Tab5 (M5_BUS, folha "BUS same as CORE2 with old
// GPIO map") - os dois batem pino a pino, nao e mais so o exemplo do CoreS3
// transposto por palpite:
//   SW1 (switches 1-8, DIP-8): posicoes 5-8 sao os 4 candidatos de BUSY,
//     posicoes 1-4 os de NSS - MAS a posicao 1 (o rotulo "ISO_G0" no
//     esquematico do modulo) NAO vai a pino nenhum do M5-Bus: e um sinal
//     interno do modulo. NSS so tem 3 candidatos de verdade (switches 2-4),
//     nao 4.
//   SW2 (2 switches, A/B): endereco I2C do expansor M5IOE1 - 00=0x74,
//     10=0x73, 01=0x72, 11=0x71. O unico banco identificavel so por
//     eletrica (basta variar o endereco sondado).
//   SW3 (switches 9-11, DIP-3): os 3 candidatos de IRQ.
// A numeracao fisica 1-11 (como quem olha a chave DIP de verdade a le)
// segue a ordem do pino 1 de cada chave no esquematico - conferir contra a
// serigrafia do modulo se um dia os dois discordarem.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  TAB5_LORA_PIN_BAIXO = 0,
  TAB5_LORA_PIN_ALTO,
  TAB5_LORA_PIN_FLUTUANTE,
  TAB5_LORA_PIN_I2C_BAIXO,
  TAB5_LORA_PIN_I2C_ALTO,
} tab5_lora_pin_estado_t;

// A qual sinal do SX1262 este pino candidato pertence (nenhuma DIP: fixo).
typedef enum {
  TAB5_LORA_SINAL_I2C = 0,   // SDA/SCL fixos, sempre ligados - sem chave
  TAB5_LORA_SINAL_BUSY,      // SW1, switches 5-8
  TAB5_LORA_SINAL_NSS,       // SW1, switches 2-4 (o 1 e interno, ver acima)
  TAB5_LORA_SINAL_IRQ,       // SW3, switches 9-11
} tab5_lora_sinal_t;

typedef struct {
  uint8_t                 mbus;     // numero fisico do pino no M5-Bus (1-30)
  int8_t                  gpio;     // GPIO do Tab5 nesse pino
  uint8_t                 chave;    // numero da chave DIP (1-11), 0 = fixo
  tab5_lora_sinal_t       sinal;
  tab5_lora_pin_estado_t  estado;
} tab5_lora_pin_t;

typedef struct {
  bool     sondado;
  uint8_t  ioe_endereco;       // zero quando nenhum M5IOE1 respondeu
  uint8_t  ioe_mascara;        // bits 0..3 correspondem a 0x71..0x74
  uint16_t ioe_uid;
  uint8_t  ioe_revisao;
  bool     radio_presente;
  int16_t  erro_radio;         // codigo RadioLib da ultima tentativa
  int16_t  erro_estado;        // resultado do ultimo comando standby
  int8_t   gpio_nss;
  int8_t   gpio_irq;
  int8_t   gpio_busy;
} tab5_lora_status_t;

// Procura o M5IOE1 e mede os pinos do M5-Bus sem ligar o radio.
// Chamar so da tarefa de interface: usa M5.In_I2C, barramento sem mutex.
void tab5_lora_sondar(void);

// Faz a sondagem, a sequencia de energia/reset e radio.begin(). Mesma regra
// de tarefa de tab5_lora_sondar(), por causa do expansor no I2C interno.
bool tab5_lora_iniciar(void);

bool tab5_lora_presente(void);
tab5_lora_status_t tab5_lora_status(void);

size_t tab5_lora_pinos(tab5_lora_pin_t* saida, size_t capacidade);
const char* tab5_lora_pin_estado_nome(tab5_lora_pin_estado_t estado);

