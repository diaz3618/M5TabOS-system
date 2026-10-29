#include "tab5_keyboard.hpp"

#include <string.h>
#include <stdio.h>
#include <M5Unified.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "tab5_time.hpp"   // tab5_now_ms

static const char *TAG = "keyboard";

// Registers, copied from official source (M5Unit-KEYBOARD,
// src/unit/unit_Tab5Keyboard.hpp). Kept with the names of la so that the
// comparison with upstream be direct if something changes.
#define REG_INT_CFG            0x00
#define REG_INT_STAT           0x01
#define REG_EVENT_NUM          0x02
#define REG_BRIGHTNESS         0x03
#define REG_MODE_KEYBOARD      0x10
#define REG_HID_EVENT          0x30   // 2 bytes: modifier + HID code
#define REG_FIRMWARE_VERSION   0xFE

// HID MODE, not character mode.
//
// In character mode, the STM32 returns TEXT: the letter ready, but the keys
// special ones like their NAME ("enter", five letters). The names are not
// documented nowhere - neither the firmware of keyboard nor the library
// official lists them - so each special key had to be discovered
// pressing and reading the log, and the arrows, Del and Tab never came to be.
//
// In HID mode, each key comes with the code of use of default USB HID (page
// 0x07), plus the byte of modifiers. The official library brings the matrix
// 5x14 entire captured of real device in this mode (KEY_MATRIX_HID_BASE and
// KEY_MATRIX_HID_SYM in unit_Tab5Keyboard.cpp):
//
// line 0: Esc 1 2 3 4 5 6 7 8 9 0 - + Del
// line 1: ` ! @ # $ % ^ & * ( ) [ ] \
// line 2: Tab q w e r t y u i o p ; 'Backspace
// line 3: Sym Aa a s d f g h j k l (top) _ Enter
// line 4: Ctrl Alt z x c v b n m . (left) (bottom) (right) Space
//
// Sym and Aa are resolved BY FIRMWARE: with Sym held, the key is enough
// with the code of layer Sym (! turn ?, * turn /, ( turn <, . turn ,), and the
// Aa arrives as the bit of shift. On our side, the US of HID table is enough.
#define MODO_HID               1

#define FREQ  400000

static bool     s_presente = false;
static uint8_t  s_firmware_version   = 0;
static bool     s_bus_ok   = false;   // the external bus already points to ExtPort1
static uint32_t s_next_probe_ms = 0;  // ms; when trying to find the new keyboard

// IF THE KEYBOARD IS ENABLED - user will, pushed out (the
// kernel at boot, the app Settings when changes), never reads from here.
// this file called settings_keyboard() directly, which is HAL (the most
// low) asking one service - the wrong sense of dependence. Born
// FALSE, equal to default of settings.cpp: the system denies it by default, and who
// calls first and who calls tab5_keyboard_enable() with the real value.
static bool s_enabled = false;

void tab5_keyboard_enable(bool on) { s_enabled = on; }

// From time to time search one keyboard absent.
//
// 2s and the commitment: plug it in and see it work within two seconds it seems
// instantaneous for whoever is using it, and an I2C transaction every 2s is noise
// negligible compared to the 50 per second that the interface already does. Search every
// cycle (20ms) would be pure waste in a device that spends most of its energy
// life without a keyboard attached.
#define PROBE_INTERVAL_MS  2000


bool tab5_keyboard_present(void) { return s_presente; }
uint8_t tab5_keyboard_version(void) { return s_firmware_version; }

// -------------------------------------------------------------------------------
// TRANSLATION: HID code -> text or special key
// -------------------------------------------------------------------------------
// US table (a of keyboard). Index = HID code from 0x1E; os of 0x04 a
// 0x1D are the letters, treated separately. 0 = code without character.
static const char US_NORMAL[] = "1234567890" "\0\0\0\0" " -=[]\\\0;'`,./";
static const char US_SHIFT[]  = "!@#$%^&*()" "\0\0\0\0" " _+{}|\0:\"~<>?";
#define US_FIRST_KEY  0x1E
#define US_LAST_KEY    0x38

static uint8_t code_especial(uint8_t hid)
{
  switch (hid) {
    case 0x28: return TAB5_KEY_ENTER;
    case 0x29: return TAB5_KEY_ESC;
    case 0x2A: return TAB5_KEY_BACKSPACE;     // Backspace
    case 0x2B: return TAB5_KEY_TAB;
    case 0x4C: return TAB5_KEY_DELETE;        // Delete
    case 0x4F: return TAB5_KEY_RIGHT;
    case 0x50: return TAB5_KEY_LEFT;
    case 0x51: return TAB5_KEY_DOWN;
    case 0x52: return TAB5_KEY_UP;
    default:   return TAB5_KEY_NONE;
  }
}

// The ASCII character of key, or 0.
static char caractere(uint8_t hid, bool shift)
{
  if (hid >= 0x04 && hid <= 0x1D) { return (char)((shift ? 'A' : 'a') + (hid - 0x04)); }
  if (hid >= US_FIRST_KEY && hid <= US_LAST_KEY) {
    return (shift ? US_SHIFT : US_NORMAL)[hid - US_FIRST_KEY];
  }
  return 0;
}

// -------------------------------------------------------------------------------
// DEAD KEY ACCENTS (the "international US" of those who write Portuguese)
// -------------------------------------------------------------------------------
// The keyboard is US: it has no accent key or cedilla. As in the layout
// world of computers, four keys become DEAD - they don't write in
// time, wait for the following letter:
//
// ' + a e i o u c -> á é í ó ú ç (and capital letters)
// ` + a -> à
// ~ + a o -> ã õ (~ e Sym + `)
// ^ + a e o -> â ê ô (^ e shift + 6)
//
// Followed by space, exit or own signal (' + space = '). Followed by another
// any letter, both come out (' + x = 'x) - nothing is lost. The double quote
// (shift + ') is NOT dead.
//
// For turn_off: KEYBOARD_ACENTOS 0.
#define KEYBOARD_ACCENTS_ENABLED 1

typedef struct { char base; const char* utf8; } composta_t;
static const composta_t AGUDO[] = {
  {'a',"\xC3\xA1"},{'e',"\xC3\xA9"},{'i',"\xC3\xAD"},{'o',"\xC3\xB3"},{'u',"\xC3\xBA"},{'c',"\xC3\xA7"},
  {'A',"\xC3\x81"},{'E',"\xC3\x89"},{'I',"\xC3\x8D"},{'O',"\xC3\x93"},{'U',"\xC3\x9A"},{'C',"\xC3\x87"},{0,0} };
static const composta_t CRASE[] = { {'a',"\xC3\xA0"},{'A',"\xC3\x80"},{0,0} };
static const composta_t TIL[]   = { {'a',"\xC3\xA3"},{'o',"\xC3\xB5"},{'A',"\xC3\x83"},{'O',"\xC3\x95"},{0,0} };
static const composta_t CIRC[]  = {
  {'a',"\xC3\xA2"},{'e',"\xC3\xAA"},{'o',"\xC3\xB4"},{'A',"\xC3\x82"},{'E',"\xC3\x8A"},{'O',"\xC3\x94"},{0,0} };

static const composta_t* dead_key_table(char ch)
{
  switch (ch) {
    case '\'': return AGUDO;
    case '`':  return CRASE;
    case '~':  return TIL;
    case '^':  return CIRC;
    default:   return nullptr;
  }
}

void tab5_keyboard_ascii(const char* text, char* output, size_t size)
{
  if (!output || size == 0) { return; }
  snprintf(output, size, "%s", text ? text : "");
  if (!text || !text[0]) { return; }
  static const char MORTAS[] = { '\'', '`', '~', '^' };
  for (char m : MORTAS) {
    for (const composta_t* c = dead_key_table(m); c->base; ++c) {
      if (strcmp(text, c->utf8) == 0) { snprintf(output, size, "%c%c", m, c->base); return; }
    }
  }
}

// -------------------------------------------------------------------------------
// KEY HOLD: REPEAT
// -------------------------------------------------------------------------------
// In HID mode, the firmware sends one event on PRESS and another, with code 0, on
// RELEASE (unit_Tab5Keyboard.hpp: "keycode ... 0 on key release"). Among the
// two the key is held, and the ones that repeat (erase, arrows) repeat here,
// with the times of keyboard of touch.
//
// TWO LOCKS, because erasing alone is the worst possible failure (add the text):
// - only repeats after having already seen a release since the keyboard
// fit. If one firmware does not release, the repeat never turns on.
// key held simply does not repeat;
// - if the OWN firmware repeats (the same new code without releasing the
// storage), ours turns off: two repetitions combined would turn off twice as much.
// And one ceiling of 10 s insured, against a missed release on the bus.
#define SEGURADA_MAX_MS  10000

static uint8_t      s_seg_hid = 0;        // insured HID code that repeats (0 = none)
static tab5_key_t s_seg_ev;             // the event it repeats
static uint32_t     s_seg_since = 0, s_last_repeat_ms = 0;
static bool         s_seg_repetiu = false;
static bool         s_soltura_vista = false;
static bool         s_fw_repete = false;

static bool repete(uint8_t code)
{
  return code == TAB5_KEY_BACKSPACE || code == TAB5_KEY_DELETE
      || code == TAB5_KEY_LEFT || code == TAB5_KEY_RIGHT
      || code == TAB5_KEY_UP || code == TAB5_KEY_DOWN;
}

bool tab5_keyboard_holding(void) { return s_presente && s_seg_hid != 0 && s_soltura_vista && !s_fw_repete; }

static char s_morta = 0;            // the dead key waiting for the letter (0 = none)
static tab5_key_t s_guardada;     // event that arrived together with morta and exit in next reading
static bool s_has_pending_event = false;

// State of pin of INT (see 'THE PIN BEFORE THE BUS', in ler_hid).
static bool     s_int_confiavel = false;   // the G50 has already proven that it warns
static bool     s_int_traiu     = false;   // ...or you've already proven that you don't warn
static bool     s_pode_ter_mais = true;    // latest reading found event
static uint32_t s_conferido_ms  = 0;

// -------------------------------------------------------------------------------

// Try to find and configure the keyboard. Call on boot E periodically
// while it is away - and which allows it to dock with the device on.
static bool search(bool falar)
{
  if (!M5.Ex_I2C.scanID(TAB5_KEYBOARD_ADDRESS, FREQ))
  {
    // It's not an error: the keyboard is an accessory, and the absence is the normal state.
    if (falar) {
      ESP_LOGW(TAG, "no keyboard at 0x%02X - will keep searching every %d ms",
               TAB5_KEYBOARD_ADDRESS, PROBE_INTERVAL_MS);
    }
    s_presente = false;
    return false;
  }

  s_firmware_version = M5.Ex_I2C.readRegister8(TAB5_KEYBOARD_ADDRESS, REG_FIRMWARE_VERSION, FREQ);

  // HID mode. Changing mode leaves events in queue in the OLD format, which
  // would be read wrong: like the official source, we empty the queue (write 0
  // in the counter clears the queue and released the INT).
  if (!M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_MODE_KEYBOARD, MODO_HID, FREQ))
  {
    ESP_LOGE(TAG, "found the keyboard but could not switch it to HID mode");
    s_presente = false;
    return false;
  }
  M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_EVENT_NUM, 0x00, FREQ);
  s_presente = true;
  s_morta = 0;
  s_has_pending_event = false;
  s_seg_hid = 0;
  s_soltura_vista = false;
  s_fw_repete = false;
  s_pode_ter_mais = true;   // first reading after fitting: without looking at the pin

  ESP_LOGI(TAG, "keyboard ENCAIXADO em 0x%02X (firmware %u), mode HID",
           TAB5_KEYBOARD_ADDRESS, (unsigned)s_firmware_version);
  return true;
}

bool tab5_keyboard_init(void)
{
  // The bus is ALWAYS repointed, even with the keyboard disabled: and it
  // takes Ex_I2C off the top of the pins of Grove, and Azimutal counts on it.

  // Repoints the external bus to ExtPort1. See why in the header.
  // This is done ONCE; Just search for the keyboard and it repeats itself.
  M5.Ex_I2C.release();
  if (!M5.Ex_I2C.begin(TAB5_KEYBOARD_PORT, TAB5_KEYBOARD_SDA, TAB5_KEYBOARD_SCL))
  {
    ESP_LOGE(TAG, "could not open I2C on SDA=%d SCL=%d",
             TAB5_KEYBOARD_SDA, TAB5_KEYBOARD_SCL);
    return false;
  }
  s_bus_ok = true;
  s_next_probe_ms = tab5_now_ms() + PROBE_INTERVAL_MS;
  // The G50 as input with pull-up (the kernel already configures it with the
  // interruption; This only guarantees the readable level if that fails). Don't move
  // no type of interruption.
  gpio_set_direction((gpio_num_t)TAB5_KEYBOARD_INT, GPIO_MODE_INPUT);
  gpio_pullup_en((gpio_num_t)TAB5_KEYBOARD_INT);

  if (!s_enabled)
  {
    ESP_LOGI(TAG, "keyboard disabled in Settings - will not search");
    return false;
  }
  return search(true);
}

// THE PIN BEFORE THE BUS. The INT of keyboard (G50, active at bottom) drops
// when there is an event in queue and it only goes up when it is read. With the screen on
// interface reads the keyboard every 60 ms: there were 16 I2C transactions per second with
// the keyboard stopped on the table. Now, with the pin at top, there is nothing to read - and
// reading the level of pin costs nothing. Be careful, because losing the key and
// worse than wasting bus:
// - the pin only becomes valid after VISTO works (one value_read event with
// he at the bottom); one value_read event with it at the top turns off the trust instead
// (firmware that does not affect INT, thread released) and iteration the reading of always;
// - after one event le of new without looking at the pin (the queue may have more than
// one, and the INT can start on the first reading), until it sees the empty queue;
// - with the pin at top, one true reading per second just like that: and
// she is the one who notices the keyboard TORN OUT (the pull-up leaves the pin at the top).
#define KEYBOARD_CHECK_INTERVAL_MS 1000   // (the variables of pino live there in top, next to state)

static bool interrupt_is_low(void) { return gpio_get_level((gpio_num_t)TAB5_KEYBOARD_INT) == 0; }

// Read the next HID raw of queue event. false = empty queue (or keyboard exited).
static bool read_hid(uint8_t* mod, uint8_t* hid)
{
  const uint32_t t = tab5_now_ms();
  const bool int_bottom = interrupt_is_low();
  if (s_int_confiavel && !int_bottom && !s_pode_ter_mais && s_seg_hid == 0
   && (t - s_conferido_ms) < KEYBOARD_CHECK_INTERVAL_MS) {
    return false;   // nothing in queue, and the pin says it for free
  }
  s_conferido_ms = t;

  // One transaction per cycle when there is no key: read only the counter of events.
  //
  // We read with readRegister (which returns success) and not readRegister8 (which
  // returns the value): without knowing if the transaction failed, one keyboard STARTED
  // would look like one keyboard with no keypresses, forever.
  uint8_t n = 0;
  if (!M5.Ex_I2C.readRegister(TAB5_KEYBOARD_ADDRESS, REG_EVENT_NUM, &n, 1, FREQ))
  {
    ESP_LOGW(TAG, "keyboard removed (stopped responding)");
    s_presente = false;
    s_next_probe_ms = t + PROBE_INTERVAL_MS;
    return false;
  }
  if (n == 0) { s_pode_ter_mais = false; return false; }
  // Learns the pin in first reading of a burst (with s_pode_ter_mais o
  // INT may have already risen from the previous reading - this one does not count).
  if (!s_pode_ter_mais && !s_int_traiu) {
    if (int_bottom) {
      if (!s_int_confiavel) { ESP_LOGI(TAG, "keyboard INT (G%d) is reliable: read only when a key is pending", TAB5_KEYBOARD_INT); }
      s_int_confiavel = true;
    } else {
      s_int_confiavel = false;
      s_int_traiu = true;
      ESP_LOGW(TAG, "event arrived with INT (G%d) high - pin is unreliable, using periodic reads", TAB5_KEYBOARD_INT);
    }
  }
  s_pode_ter_mais = true;

  uint8_t buf[2] = { 0xFF, 0xFF };
  if (!M5.Ex_I2C.readRegister(TAB5_KEYBOARD_ADDRESS, REG_HID_EVENT, buf, 2, FREQ)) { return false; }

  // Releases the signal of INT. We do this even without using the pin yet: without the
  // cleaning the STM32 keeps the interrupt active and, on the day we turn_on the
  // G50, she would already be born stuck.
  M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_INT_STAT, 0x00, FREQ);

  if (buf[0] == 0xFF && buf[1] == 0xFF) { return false; }   // empty queue (sentinel of firmware)
  *mod = buf[0];
  *hid = buf[1];
  return true;
}

bool tab5_keyboard_read(tab5_key_t* ev)
{
  if (!ev) { return false; }
  memset(ev, 0, sizeof(*ev));
  if (!s_bus_ok) { return false; }

  // Off in Settings: doesn't even search. Exact cost of what we avoid: a
  // I2C transaction every 2s, ~200us of bus at 400kHz - negligible by far
  // of the ~50 readings per second that the interface already makes. But the beginning of this
  // system and deny by default, and who decides what stays active and the user.
  if (!s_enabled)
  {
    if (s_presente) {
      ESP_LOGI(TAG, "keyboard desativado em Ajustes");
      s_presente = false;
    }
    return false;
  }

  if (!s_presente)
  {
    // Absent: looking for from time to time. This is what makes the keyboard work
    // on be docked with the device on, without restarting.
    const uint32_t t = tab5_now_ms();
    if ((int32_t)(t - s_next_probe_ms) < 0) { return false; }
    s_next_probe_ms = t + PROBE_INTERVAL_MS;
    if (!search(false)) { return false; }
  }

  // The event that became back when a dead key was undone by a
  // special key: exit now, in order in which it was pressed.
  if (s_has_pending_event) { *ev = s_guardada; s_has_pending_event = false; return true; }

  const uint32_t now = tab5_now_ms();
  uint8_t m = 0, hid = 0;
  if (!read_hid(&m, &hid)) {
    // Empty queue: the held key repeats if it is its turn.
    if (tab5_keyboard_holding()) {
      if (now - s_seg_since > SEGURADA_MAX_MS) { s_seg_hid = 0; return false; }
      const uint32_t step = s_seg_repetiu ? TAB5_KEY_REPEAT_MS : TAB5_KEY_REPEAT_START_MS;
      if (now - s_last_repeat_ms >= step) {
        s_last_repeat_ms = now;
        s_seg_repetiu = true;
        *ev = s_seg_ev;
        return true;
      }
    }
    return false;
  }
  if (hid == 0) {
    // RELEASE (or just one modifier, Ctrl/Alt alone): nothing to give away, and
    // the held key has stopped being held.
    s_seg_hid = 0;
    if (!s_soltura_vista) { s_soltura_vista = true; ESP_LOGI(TAG, "o keyboard manda a soltura: key segurada repete"); }
    return false;
  }
  if (hid == s_seg_hid && !s_fw_repete) {
    // The same new code, without release on the storage: and the firmware repeating.
    s_fw_repete = true;
    ESP_LOGI(TAG, "keyboard firmware already repeats held keys - local repetition disabled");
  }

  // HID modifiers: left in bits 0-2, right in 4-6.
  const uint8_t mm = (uint8_t)((m | (m >> 4)) & 0x07);
  ev->mod = mm;
  const bool shift = (mm & TAB5_KEY_SHIFT) != 0;

  ev->code = code_especial(hid);
  if (ev->code == TAB5_KEY_NONE) {
    const char ch = caractere(hid, shift);
    if (!ch) {
      ESP_LOGW(TAG, "no translation for key: HID 0x%02X modifiers 0x%02X - ignored", hid, m);
      return false;
    }
    ev->text[0] = ch;
  }

  if (repete(ev->code)) {
    s_seg_hid = hid;
    s_seg_ev = *ev;
    s_seg_since = s_last_repeat_ms = now;
    s_seg_repetiu = false;
  } else {
    s_seg_hid = 0;
  }

#if KEYBOARD_ACCENTS_ENABLED
  // With Ctrl/Alt and shortcut, not text: the dead key is not involved.
  const bool atalho = (mm & (TAB5_KEY_CTRL | TAB5_KEY_ALT)) != 0;
  if (!atalho) {
    if (s_morta) {
      const char morta = s_morta;
      s_morta = 0;
      if (ev->code == TAB5_KEY_NONE) {
        const char ch = ev->text[0];
        if (ch == ' ') { ev->text[0] = morta; return true; }        // ' + space = '
        for (const composta_t* c = dead_key_table(morta); c->base; ++c) {
          if (c->base == ch) { snprintf(ev->text, sizeof(ev->text), "%s", c->utf8); return true; }
        }
        ev->text[0] = morta; ev->text[1] = ch; ev->text[2] = 0;    // ' + x = 'x
        return true;
      }
      // Special after death: the signal exits first, the special in next.
      s_guardada = *ev;
      s_has_pending_event = true;
      memset(ev, 0, sizeof(*ev));
      ev->text[0] = morta;
      return true;
    }
    if (ev->code == TAB5_KEY_NONE && dead_key_table(ev->text[0])) {
      s_morta = ev->text[0];   // wait for the lyrics
      return false;
    }
  }
#endif
  return true;
}

bool tab5_keyboard_pending(void)
{
  if (!s_bus_ok || !s_enabled || !s_presente) { return false; }
  // In sleep too: pin in top and nothing pending = no key, no I2C.
  if (s_int_confiavel && !interrupt_is_low() && !s_pode_ter_mais) { return false; }
  uint8_t n = 0;
  if (!M5.Ex_I2C.readRegister(TAB5_KEYBOARD_ADDRESS, REG_EVENT_NUM, &n, 1, FREQ)) { return false; }
  return n > 0;
}

void tab5_keyboard_discard(void)
{
  s_morta = 0;
  s_has_pending_event = false;
  s_seg_hid = 0;
  if (!s_bus_ok || !s_presente) { return; }
  // Writing 0 to the counter clears the queue and releases the INT (datasheet).
  M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_EVENT_NUM, 0x00, FREQ);
  M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_INT_STAT, 0x00, FREQ);
}

void tab5_keyboard_brightness(uint8_t v)
{
  if (!s_presente) { return; }
  M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_BRIGHTNESS, v, FREQ);
}
