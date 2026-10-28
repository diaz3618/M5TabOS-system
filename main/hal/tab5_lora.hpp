// Radio LoRa SX1262 encaixado no M5-Bus.
//
// NAO E MESHTASTIC. Este HAL fala SPI diretamente com o transceptor nu do
// Module 13.2 LoRa-1262; o servico tab5_mesh fala com outro aparelho, ja com
// firmware Meshtastic, por UART/Bluetooth.
//
// O modulo espalha seus sinais por tres bancos de chaves:
//   SW1 - BUSY e NSS; SW2 - endereco I2C do expansor; SW3 - IRQ.
// So SW2 pode ser identificado sem ambiguidade, procurando 0x71..0x74. Os
// pinos abaixo guardam tambem a sondagem eletrica dos candidatos de SW1/SW3.

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

typedef struct {
  uint8_t                 mbus;
  int8_t                  gpio;
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

