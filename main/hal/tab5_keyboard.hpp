// Tab5 Keyboard (M5Stack Tab5 Keyboard), on the ExtPort1 side connector.
//
// Specification (docs.m5stack.com/en/tab5/Tab5_Keyboard is the source of
// M5Unit-KEYBOARD): STM32F030C8T6 controller, 14x5 matrix (70 keys),
// I2C at address 0x6D.
//
// ExtPort1: G0 = SDA | G1 = SCL | G50 = INT | EXT 5V | GND
// (the G49 of connector is not used by the keyboard)
//
// We DO NOT use the M5Unit-KEYBOARD library: it is from the M5UnitUnified framework,
// which would bring one world of dependencies to read three registers. The protocol
// is simple and is fully documented below.
//
// ------------------------------------------------------------------------
// WHICH BUS, AND WHY
// ------------------------------------------------------------------------
// The P4 has 2 high-power and 1 low-power I2C controllers. AND ATTENTION TO
// NUMERATION, which is the opposite of what intuition suggests and cost me a
// regression would be:
//
// I2C_NUM_1 = INTERNAL bus (GPIO 32/31). Loads the INA226, the
// expanders AND THE TOUCH (M5GFX.cpp: cfg.i2c_port = 1 for the
// GT911). M5Unified.cpp:2095 - "in_port = I2C_NUM_1". UNTOUCHABLE.
// I2C_NUM_0 = EXTERNAL bus, Grove (54/53).
// M5Unified.cpp:2080 - "ex_port = I2C_NUM_0".
//
// Pointing the keyboard at port 1 thinking it was the external one has already happened. Result: the
// device went to sleep and NEVER responded on touch again, because the
// controller of touch had lost the bus. If one day the touch, the battery
// or the expanders start to fail together, suspect this file first.
//
// We repoint the EXTERNAL to the pins of keyboard. It costs nothing today: nothing in this
// system uses Grove as I2C - Azimutal uses it as UART of GPS. And resolve
// of toast the conflict that costs us time, because with Ex_I2C out of 53/54
// no one reserves those pins anymore.
//
// If one day you need one Unit I2C on the Grove, the keyboard migrates to the
// LP controller (LP_I2C_NUM_0): GPIO 0 and 1 are within the range 0-15 that
// is the only LP/RTC on the P4, so it is viable - it just isn't necessary now.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "driver/i2c_types.h"

#define TAB5_KEYBOARD_PORT      I2C_NUM_0   // EXTERNAL - see warning above
#define TAB5_KEYBOARD_ADDRESS   0x6D
#define TAB5_KEYBOARD_SDA        0      // ExtPort1 G0
#define TAB5_KEYBOARD_SCL        1      // ExtPort1 G1
#define TAB5_KEYBOARD_INT        50     // ExtPort1 G50: low at each event (wakes up the screen, kernel/core)

// SPECIAL KEYS, already translated.
//
// The driver translates each key once and delivers the ready event (see ao_teclar
// in app.h): text for what you type, code for the rest. Each app
// recognizing the keyboard on its own would be the same table copied throughout
// place, with the new app forgetting one case.
#define TAB5_KEY_NONE   0   // the event and TEXT
#define TAB5_KEY_ENTER     1
#define TAB5_KEY_BACKSPACE    2   // Backspace: deletes BEFORE cursor
#define TAB5_KEY_ESC       3
#define TAB5_KEY_TAB       4
#define TAB5_KEY_LEFT  5   // arrows
#define TAB5_KEY_RIGHT   6
#define TAB5_KEY_UP      7
#define TAB5_KEY_DOWN     8
#define TAB5_KEY_DELETE       9   // Del: delete AFTER cursor

// KEY HOLD REPEATS - the same times on both keyboards (of touch,
// kernel/virtual_keyboard, and the physical one): after REPETE_INICIO_MS held,
// once every REPETE_MS. In the physical one, they repeat the erase (Backspace and Del) and
// the arrows; letter no.
#define TAB5_KEY_REPEAT_START_MS  450
#define TAB5_KEY_REPEAT_MS          70

// Modifiers (field 'mod' of event).
#define TAB5_KEY_CTRL   0x01
#define TAB5_KEY_SHIFT  0x02
#define TAB5_KEY_ALT    0x04

// A key, translated. 'text' and UTF-8 with terminator (can be 2 bytes:
// accented letter composed of the dead key) and comes EMPTY when 'code' says
// what is. With Ctrl or Alt held down the text appears like this - who doesn't use it
// shortcut ignores (conversation ignores).
typedef struct {
  char    text[8];
  uint8_t code;   // TAB5_KEY_*
  uint8_t mod;      // TAB5_KEY_CTRL/SHIFT/ALT
} tab5_key_t;

// Will of user (Settings), pushed from outside - HAL does not ask
// no service alone (see the big warning about s_enabled in the .cpp).
// Call BEFORE of tab5_keyboard_init() at boot, and of new whenever the
// Settings change the setting at run time.
void tab5_keyboard_enable(bool on);

// Resets the external I2C and puts the keyboard in HID mode. Returns false if
// no one responds at 0x6D - without keyboard the system continues normally.
// Call of task of interface (and she's the one in charge of I2C).
bool tab5_keyboard_init(void);

// Is the keyboard docked now? (comes with fitting and removing with the device on)
bool tab5_keyboard_present(void);

// Read ONE key, if any, already translated. false = none.
// OS of task of interface.
bool tab5_keyboard_read(tab5_key_t* ev);

// Is there a key that repeats (erase, arrow) held now? As long as there is,
// who needs to read at the rate of repetition, not at the rate of typing.
bool tab5_keyboard_holding(void);

// TO WAKE UP FROM SLEEP: is there a key waiting in queue? (an I2C transaction,
// consumes nothing). false without keyboard.
bool tab5_keyboard_pending(void);

// Throws away the entire queue and what was in the storage (dead key, key
// insured). For the key that WAKES UP the screen, which cannot become a letter.
void tab5_keyboard_discard(void);

// FOR SO-ASCII FIELD (password of Wi-Fi, name of no): undoes the letter that the
// dead key composed, returning the two pressed keys ("á" -> "'a"). One
// password with an apostrophe followed by a vowel cannot become an accented letter and
// disappear. Text without composition exit equal.
void tab5_keyboard_ascii(const char* text, char* output, size_t size);

// Brightness of backlight of keyboard, 0..255.
void tab5_keyboard_brightness(uint8_t v);

// Version of firmware of STM32 of keyboard (0 if you haven't read it).
uint8_t tab5_keyboard_version(void);
