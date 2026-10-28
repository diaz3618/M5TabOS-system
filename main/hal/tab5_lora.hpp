// Radio LoRa SX1262 encaixado no M5-Bus.
//
// NAO E MESHTASTIC. Este HAL fala SPI diretamente com o transceptor nu do
// Module 13.2 LoRa-1262; o service tab5_mesh fala com outro aparelho, ja com
// firmware Meshtastic, por UART/Bluetooth.
//
// O modulo espalha seus sinais por tres bancos de chaves, CONFERIDOS no
// esquematico do modulo (SCH_Module13.2_LoRa1262_V1.0) e cruzados com o
// esquematico do proprio Tab5 (M5_BUS, folha "BUS same as CORE2 with old
// GPIO map") - os dois batem pino a pino, nao e mais so o exemplo do CoreS3
// transposto por palpite:
//   SW1 (switches 1-8, DIP-8): posicoes 5-8 sao os 4 candidatos de BUSY,
//     posicoes 1-4 os de NSS - MAS a position 1 (o rotulo "ISO_G0" no
//     esquematico do modulo) NAO vai a pino nenhum do M5-Bus: e um signal
//     interno do modulo. NSS so tem 3 candidatos de verdade (switches 2-4),
//     nao 4.
//   SW2 (2 switches, A/B): address I2C do expansor M5IOE1 - 00=0x74,
//     10=0x73, 01=0x72, 11=0x71. O unico banco identificavel so por
//     eletrica (basta variar o address probed).
//   SW3 (switches 9-11, DIP-3): os 3 candidatos de IRQ.
// A numeracao fisica 1-11 (como who olha a switch_number DIP de verdade a le)
// segue a ordem do pino 1 de cada switch_number no esquematico - conferir contra a
// serigrafia do modulo se um dia os dois discordarem.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  TAB5_LORA_PIN_LOW = 0,
  TAB5_LORA_PIN_HIGH,
  TAB5_LORA_PIN_FLOATING,
  TAB5_LORA_PIN_I2C_LOW,
  TAB5_LORA_PIN_I2C_HIGH,
} tab5_lora_pin_state_t;

// A qual signal do SX1262 este pino candidato pertence (nenhuma DIP: fixo).
typedef enum {
  TAB5_LORA_SIGNAL_I2C = 0,   // SDA/SCL fixos, sempre ligados - sem switch_number
  TAB5_LORA_SIGNAL_BUSY,      // SW1, switches 5-8
  TAB5_LORA_SIGNAL_NSS,       // SW1, switches 2-4 (o 1 e interno, ver acima)
  TAB5_LORA_SIGNAL_IRQ,       // SW3, switches 9-11
} tab5_lora_signal_t;

typedef struct {
  uint8_t                 mbus;     // numero fisico do pino no M5-Bus (1-30)
  int8_t                  gpio;     // GPIO do Tab5 nesse pino
  uint8_t                 switch_number;    // numero da switch_number DIP (1-11), 0 = fixo
  tab5_lora_signal_t       signal;
  tab5_lora_pin_state_t  state;
} tab5_lora_pin_t;

typedef struct {
  bool     probed;
  uint8_t  ioe_address;       // zero quando nenhum M5IOE1 respondeu
  uint8_t  ioe_mask;        // bits 0..3 correspondem a 0x71..0x74
  uint16_t ioe_uid;
  uint8_t  ioe_revision;
  bool     radio_present;
  int16_t  radio_error;         // code RadioLib da latest tentativa
  int16_t  state_error;        // resultado do ultimo comando standby
  int8_t   gpio_nss;
  int8_t   gpio_irq;
  int8_t   gpio_busy;
} tab5_lora_status_t;

// Procura o M5IOE1 e mede os pins do M5-Bus sem turn_on o radio.
// Chamar so da tarefa de interface: usa M5.In_I2C, barramento sem mutex.
void tab5_lora_probe(void);

// Faz a sondagem, a sequencia de energia/reset e radio.begin(). Mesma regra
// de tarefa de tab5_lora_probe(), por causa do expansor no I2C interno.
bool tab5_lora_iniciar(void);

bool tab5_lora_present(void);
tab5_lora_status_t tab5_lora_status(void);

size_t tab5_lora_pins(tab5_lora_pin_t* output, size_t capacity);
const char* tab5_lora_pin_state_name(tab5_lora_pin_state_t state);

