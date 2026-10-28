// SX1262 LoRa radio seated on the M5-Bus.
//
// NOT MESHTASTIC. This HAL talks SPI directly to the bare transceiver on
// Module 13.2 LoRa-1262; the tab5_mesh service talks to a different device,
// one already running Meshtastic firmware, over UART/Bluetooth.
//
// The module spreads its signals across three banks of DIP switches,
// CONFIRMED against the module's own schematic (SCH_Module13.2_LoRa1262_V1.0)
// and cross-checked against the Tab5's own schematic (M5_BUS sheet, "BUS same
// as CORE2 with old GPIO map") - the two match pin for pin, this is no longer
// just the CoreS3 example carried over by guesswork:
//   SW1 (switches 1-8, DIP-8): positions 5-8 are the 4 BUSY candidates,
//     positions 1-4 the NSS candidates - BUT position 1 (labeled "ISO_G0" on
//     the module's schematic) does NOT reach any M5-Bus pin: it's a signal
//     internal to the module. NSS only has 3 real candidates (switches 2-4),
//     not 4.
//   SW2 (2 switches, A/B): I2C address of the M5IOE1 expander - 00=0x74,
//     10=0x73, 01=0x72, 11=0x71. The only bank identifiable purely
//     electrically (just vary the probed address).
//   SW3 (switches 9-11, DIP-3): the 3 IRQ candidates.
// The physical 1-11 numbering (as read off the real DIP switch) follows the
// pin-1 order of each switch on the module's schematic - cross-check against
// the module's silkscreen if the two ever disagree.

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

// Which SX1262 signal this candidate pin belongs to (none: fixed, no DIP).
typedef enum {
  TAB5_LORA_SIGNAL_I2C = 0,   // SDA/SCL, fixed and always connected - no switch
  TAB5_LORA_SIGNAL_BUSY,      // SW1, switches 5-8
  TAB5_LORA_SIGNAL_NSS,       // SW1, switches 2-4 (switch 1 is internal, see above)
  TAB5_LORA_SIGNAL_IRQ,       // SW3, switches 9-11
} tab5_lora_signal_t;

typedef struct {
  uint8_t                 mbus;     // physical pin number on the M5-Bus (1-30)
  int8_t                  gpio;     // Tab5 GPIO on that pin
  uint8_t                 switch_number;    // DIP switch number (1-11), 0 = fixed
  tab5_lora_signal_t       signal;
  tab5_lora_pin_state_t  state;
} tab5_lora_pin_t;

typedef struct {
  bool     probed;
  uint8_t  ioe_address;       // zero when no M5IOE1 answered
  uint8_t  ioe_mask;        // bits 0..3 correspond to 0x71..0x74
  uint16_t ioe_uid;
  uint8_t  ioe_revision;
  bool     radio_present;
  int16_t  radio_error;         // RadioLib code from the latest attempt
  int16_t  state_error;        // result of the last standby command
  int8_t   gpio_nss;
  int8_t   gpio_irq;
  int8_t   gpio_busy;
} tab5_lora_status_t;

// Looks for the M5IOE1 and measures the M5-Bus pins without powering on the
// radio. Call only from the interface task: uses M5.In_I2C, a mutex-free bus.
void tab5_lora_probe(void);

// Runs the probe, the power/reset sequence, and radio.begin(). Same task
// rule as tab5_lora_probe(), because of the expander on the internal I2C.
bool tab5_lora_start(void);

bool tab5_lora_present(void);
tab5_lora_status_t tab5_lora_status(void);

size_t tab5_lora_pins(tab5_lora_pin_t* output, size_t capacity);
const char* tab5_lora_pin_state_name(tab5_lora_pin_state_t state);
