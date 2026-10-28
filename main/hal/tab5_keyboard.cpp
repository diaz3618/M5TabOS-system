#include "tab5_keyboard.hpp"

#include <string.h>
#include <stdio.h>
#include <M5Unified.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "tab5_time.hpp"   // tab5_now_ms

static const char *TAG = "keyboard";

// Registradores, copiados of fonte oficial (M5Unit-KEYBOARD,
// src/unit/unit_Tab5Keyboard.hpp). Mantidos com os nomes of la para que a
// comparacao com o upstream seja direta se algo mudar.
#define REG_INT_CFG            0x00
#define REG_INT_STAT           0x01
#define REG_EVENT_NUM          0x02
#define REG_BRIGHTNESS         0x03
#define REG_MODE_KEYBOARD      0x10
#define REG_HID_EVENT          0x30   // 2 bytes: modificador + code HID
#define REG_FIRMWARE_VERSION   0xFE

// MODO HID, e nao o mode caractere.
//
// No mode caractere o STM32 devolve TEXTO: a letra pronta, mas as teclas
// especiais como o NOME delas ("enter", cinco letras). Os nomes nao estao
// documentados em lugar nenhum - nem o firmware of keyboard nem a biblioteca
// oficial os listam - entao cada key especial tinha que ser descoberta
// apertando e lendo o log, e as setas, o Del e o Tab nunca chegaram a ser.
//
// No mode HID cada key vem como o code of uso of default USB HID (page
// 0x07), mais o byte of modificadores. A biblioteca oficial traz a matriz
// 5x14 inteira capturada of aparelho real nesse mode (KEY_MATRIX_HID_BASE e
// KEY_MATRIX_HID_SYM em unit_Tab5Keyboard.cpp):
//
//   line 0: Esc 1 2 3 4 5 6 7 8 9 0 - + Del
//   line 1: ` ! @ # $ % ^ & * ( ) [ ] \
//   line 2: Tab q w e r t y u i o p ; ' Backspace
//   line 3: Sym Aa a s d f g h j k l (top) _ Enter
//   line 4: Ctrl Alt z x c v b n m . (esq) (bottom) (dir) Espaco
//
// Sym e Aa sao resolvidos PELO FIRMWARE: com Sym segurado a key ja chega
// com o code of camada Sym (! vira ?, * vira /, ( vira <, . vira ,), e o
// Aa chega como o bit of shift. Do ours lado basta a tabela US of HID.
#define MODO_HID               1

#define FREQ  400000

static bool     s_presente = false;
static uint8_t  s_versao   = 0;
static bool     s_bus_ok   = false;   // o barramento externo ja aponta para o ExtPort1
static uint32_t s_next_busca = 0;  // ms; when tentar achar o keyboard of novo

// SE O TECLADO ESTA HABILITADO - vontade of user, empurrada of fora (o
// kernel no boot, o app Ajustes when muda), nunca lida daqui.
// este file chamava settings_keyboard() direto, o que e HAL (a camada mais
// low) perguntando a one service - o sentido errado of dependencia. Nasce
// FALSO, igual on default of settings.cpp: o sistema nega por default, e who
// liga first e who chama tab5_keyboard_enable() com o value real.
static bool s_habilitado = false;

void tab5_keyboard_enable(bool on) { s_habilitado = on; }

// De quanto em quanto time search one keyboard ausente.
//
// 2s e o compromisso: encaixar e ver funcionar dentro of dois segundos parece
// instantaneo para who esta usando, e uma transacao I2C a cada 2s e ruido
// desprezivel perto das 50 por segundo que a interface ja faz. Buscar a cada
// ciclo (20ms) seria desperdicio puro num aparelho que passa a maior parte of
// vida sem keyboard encaixado.
#define BUSCA_MS  2000


bool tab5_keyboard_present(void) { return s_presente; }
uint8_t tab5_keyboard_version(void) { return s_versao; }

// ---------------------------------------------------------------------------
// TRADUCAO: code HID -> text ou key especial
// ---------------------------------------------------------------------------
// Tabela US (a of keyboard). Indice = code HID a partir of 0x1E; os of 0x04 a
// 0x1D sao as letras, tratadas a parte. 0 = code sem caractere.
static const char US_NORMAL[] = "1234567890" "\0\0\0\0" " -=[]\\\0;'`,./";
static const char US_SHIFT[]  = "!@#$%^&*()" "\0\0\0\0" " _+{}|\0:\"~<>?";
#define US_PRIMEIRO  0x1E
#define US_ULTIMO    0x38

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

// O caractere ASCII of key, ou 0.
static char caractere(uint8_t hid, bool shift)
{
  if (hid >= 0x04 && hid <= 0x1D) { return (char)((shift ? 'A' : 'a') + (hid - 0x04)); }
  if (hid >= US_PRIMEIRO && hid <= US_ULTIMO) {
    return (shift ? US_SHIFT : US_NORMAL)[hid - US_PRIMEIRO];
  }
  return 0;
}

// ---------------------------------------------------------------------------
// ACENTOS POR TECLA MORTA (o "US internacional" of who escreve portugues)
// ---------------------------------------------------------------------------
// O keyboard e US: nao tem key of acento nem cedilha. Como no layout
// internacional dos computadores, quatro teclas viram MORTAS - nao escrevem in
// hora, esperam a letra seguinte:
//
//   '  + a e i o u c  ->  á é í ó ú ç     (e maiusculas)
//   `  + a            ->  à
//   ~  + a o          ->  ã õ             (~ e Sym + `)
//   ^  + a e o        ->  â ê ô           (^ e shift + 6)
//
// Seguida of space, exit o proprio signal (' + space = '). Seguida of outra
// letra qualquer, saem os dois (' + x = 'x) - nada se perde. A aspa dupla
// (shift + ') NAO e morta.
//
// Para turn_off: TECLADO_ACENTOS 0.
#define TECLADO_ACENTOS 1

typedef struct { char base; const char* utf8; } composta_t;
static const composta_t AGUDO[] = {
  {'a',"\xC3\xA1"},{'e',"\xC3\xA9"},{'i',"\xC3\xAD"},{'o',"\xC3\xB3"},{'u',"\xC3\xBA"},{'c',"\xC3\xA7"},
  {'A',"\xC3\x81"},{'E',"\xC3\x89"},{'I',"\xC3\x8D"},{'O',"\xC3\x93"},{'U',"\xC3\x9A"},{'C',"\xC3\x87"},{0,0} };
static const composta_t CRASE[] = { {'a',"\xC3\xA0"},{'A',"\xC3\x80"},{0,0} };
static const composta_t TIL[]   = { {'a',"\xC3\xA3"},{'o',"\xC3\xB5"},{'A',"\xC3\x83"},{'O',"\xC3\x95"},{0,0} };
static const composta_t CIRC[]  = {
  {'a',"\xC3\xA2"},{'e',"\xC3\xAA"},{'o',"\xC3\xB4"},{'A',"\xC3\x82"},{'E',"\xC3\x8A"},{'O',"\xC3\x94"},{0,0} };

static const composta_t* tabela_morta(char ch)
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
    for (const composta_t* c = tabela_morta(m); c->base; ++c) {
      if (strcmp(text, c->utf8) == 0) { snprintf(output, size, "%c%c", m, c->base); return; }
    }
  }
}

// ---------------------------------------------------------------------------
// TECLA SEGURADA: REPETICAO
// ---------------------------------------------------------------------------
// No mode HID o firmware manda one evento on APERTAR e outro, com code 0, on
// SOLTAR (unit_Tab5Keyboard.hpp: "keycode ... 0 on key release"). Entre os
// dois a key esta segurada, e as que repetem (erase, setas) repetem aqui,
// com os tempos of keyboard of touch.
//
// DUAS TRAVAS, porque erase sozinho e a pior failure possivel (some o text):
//  - so repete depois of ja ter seen UMA soltura since que o keyboard
//    encaixou. Se one firmware nao mandar soltura, a repeticao nunca liga - a
//    key segurada simplesmente nao repete;
//  - se o PROPRIO firmware repetir (o mesmo code of novo sem soltura no
//    storage), a nossa desliga: duas repeticoes somadas apagariam em dobro.
// E one ceiling of 10 s segurada, contra uma soltura perdida no barramento.
#define SEGURADA_MAX_MS  10000

static uint8_t      s_seg_hid = 0;        // code HID segurado que repete (0 = nenhum)
static tab5_key_t s_seg_ev;             // o evento que ele repete
static uint32_t     s_seg_since = 0, s_seg_ultimo = 0;
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

static char s_morta = 0;            // a key morta esperando a letra (0 = none)
static tab5_key_t s_guardada;     // evento que chegou junto of morta e exit in next leitura
static bool s_tem_guardada = false;

// Estado of pino of INT (ver 'O PINO ANTES DO BARRAMENTO', em ler_hid).
static bool     s_int_confiavel = false;   // o G50 ja provou que avisa
static bool     s_int_traiu     = false;   // ...ou ja provou que nao avisa
static bool     s_pode_ter_mais = true;    // a latest leitura achou evento
static uint32_t s_conferido_ms  = 0;

// ---------------------------------------------------------------------------

// Tenta encontrar e configurar o keyboard. Chamada no boot E periodicamente
// enquanto ele estiver ausente - e o que permite encaixar com o aparelho on.
static bool search(bool falar)
{
  if (!M5.Ex_I2C.scanID(TAB5_KEYBOARD_ADDRESS, FREQ))
  {
    // Nao e erro: o keyboard e acessorio, e a ausencia e o state normal.
    if (falar) {
      ESP_LOGW(TAG, "no keyboard at 0x%02X - will keep searching every %d ms",
               TAB5_KEYBOARD_ADDRESS, BUSCA_MS);
    }
    s_presente = false;
    return false;
  }

  s_versao = M5.Ex_I2C.readRegister8(TAB5_KEYBOARD_ADDRESS, REG_FIRMWARE_VERSION, FREQ);

  // Modo HID. Trocar of mode deixa in queue eventos no formato ANTIGO, que
  // seriam lidos errado: como o fonte oficial, esvaziamos a queue (write 0
  // no contador limpa a queue e released o INT).
  if (!M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_MODE_KEYBOARD, MODO_HID, FREQ))
  {
    ESP_LOGE(TAG, "found the keyboard but could not switch it to HID mode");
    s_presente = false;
    return false;
  }
  M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_EVENT_NUM, 0x00, FREQ);
  s_presente = true;
  s_morta = 0;
  s_tem_guardada = false;
  s_seg_hid = 0;
  s_soltura_vista = false;
  s_fw_repete = false;
  s_pode_ter_mais = true;   // primeira leitura depois of encaixar: sem olhar o pino

  ESP_LOGI(TAG, "keyboard ENCAIXADO em 0x%02X (firmware %u), mode HID",
           TAB5_KEYBOARD_ADDRESS, (unsigned)s_versao);
  return true;
}

bool tab5_keyboard_init(void)
{
  // O barramento e reapontado SEMPRE, mesmo com o keyboard desativado: e ele que
  // tira o Ex_I2C of top dos pins of Grove, e o Azimutal conta com isso.

  // Reaponta o barramento externo para o ExtPort1. Ver o porque no header.
  // Isto e feito UMA vez; so a procura pelo keyboard e que se repete.
  M5.Ex_I2C.release();
  if (!M5.Ex_I2C.begin(TAB5_KEYBOARD_PORT, TAB5_KEYBOARD_SDA, TAB5_KEYBOARD_SCL))
  {
    ESP_LOGE(TAG, "could not open I2C on SDA=%d SCL=%d",
             TAB5_KEYBOARD_SDA, TAB5_KEYBOARD_SCL);
    return false;
  }
  s_bus_ok = true;
  s_next_busca = tab5_now_ms() + BUSCA_MS;
  // O G50 como input com pull-up (o kernel ja o configura com a
  // interrupcao; isto so garante o level legivel se aquilo falhar). Nao mexe
  // no type of interrupcao.
  gpio_set_direction((gpio_num_t)TAB5_KEYBOARD_INT, GPIO_MODE_INPUT);
  gpio_pullup_en((gpio_num_t)TAB5_KEYBOARD_INT);

  if (!s_habilitado)
  {
    ESP_LOGI(TAG, "keyboard disabled in Settings - will not search");
    return false;
  }
  return search(true);
}

// O PINO ANTES DO BARRAMENTO. O INT of keyboard (G50, active em bottom) desce
// when ha evento in queue e so sobe when ela e lida. Com a screen acesa a
// interface le o keyboard a cada 60 ms: eram 16 transacoes I2C por segundo com
// o keyboard parado in mesa. Agora, com o pino em top, nao ha o que read - e
// read o level of pino nao custa nada. Tres cuidados, porque perder key e
// pior que gastar barramento:
//  - o pino so passa a valer depois of VISTO funcionando (one evento value_read com
//    ele em bottom); one evento value_read com ele em top desliga a confianca of vez
//    (firmware que nao mexe no INT, fio released) e iteration a leitura of sempre;
//  - depois of one evento le of novo sem olhar o pino (a queue pode ter mais of
//    one, e o INT pode start ja in primeira leitura), ate ver a queue vazia;
//  - com o pino em top, uma leitura of verdade por segundo assim mesmo: e
//    ela que percebe o keyboard ARRANCADO (o pull-up deixa o pino em top).
#define TECLADO_CONFERIR_MS 1000   // (as variaveis of pino moram la em top, junto of state)

static bool int_em_bottom(void) { return gpio_get_level((gpio_num_t)TAB5_KEYBOARD_INT) == 0; }

// Le o next evento HID raw of queue. false = queue vazia (ou keyboard exited).
static bool read_hid(uint8_t* mod, uint8_t* hid)
{
  const uint32_t t = tab5_now_ms();
  const bool int_bottom = int_em_bottom();
  if (s_int_confiavel && !int_bottom && !s_pode_ter_mais && s_seg_hid == 0
   && (t - s_conferido_ms) < TECLADO_CONFERIR_MS) {
    return false;   // nada in queue, e o pino diz isso of graca
  }
  s_conferido_ms = t;

  // Uma transacao por ciclo when nao ha key: le so o contador of eventos.
  //
  // Lemos com readRegister (que devolve sucesso) e nao readRegister8 (que
  // devolve o value): sem saber se a transacao failed, one keyboard ARRANCADO
  // pareceria one keyboard sem teclas pressionadas, para sempre.
  uint8_t n = 0;
  if (!M5.Ex_I2C.readRegister(TAB5_KEYBOARD_ADDRESS, REG_EVENT_NUM, &n, 1, FREQ))
  {
    ESP_LOGW(TAG, "keyboard removed (stopped responding)");
    s_presente = false;
    s_next_busca = t + BUSCA_MS;
    return false;
  }
  if (n == 0) { s_pode_ter_mais = false; return false; }
  // Aprende o pino in primeira leitura of uma rajada (com s_pode_ter_mais o
  // INT ja pode ter subido pela leitura anterior - essa nao conta).
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

  // Libera o signal of INT. Fazemos isto mesmo sem usar o pino ainda: sem a
  // limpeza o STM32 mantem a interrupcao active e, no dia em que a gente turn_on o
  // G50, ela ja nasceria travada.
  M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_INT_STAT, 0x00, FREQ);

  if (buf[0] == 0xFF && buf[1] == 0xFF) { return false; }   // queue vazia (sentinela of firmware)
  *mod = buf[0];
  *hid = buf[1];
  return true;
}

bool tab5_keyboard_read(tab5_key_t* ev)
{
  if (!ev) { return false; }
  memset(ev, 0, sizeof(*ev));
  if (!s_bus_ok) { return false; }

  // Desligado em Ajustes: nem procura. Custo exato of que a gente evita: uma
  // transacao I2C a cada 2s, ~200us of barramento a 400kHz - desprezivel perto
  // das ~50 leituras por segundo que a interface ja faz. Mas o principio deste
  // sistema e negar por default, e who decide o que stays active e o user.
  if (!s_habilitado)
  {
    if (s_presente) {
      ESP_LOGI(TAG, "keyboard desativado em Ajustes");
      s_presente = false;
    }
    return false;
  }

  if (!s_presente)
  {
    // Ausente: procura of tempos em tempos. E isto que faz o keyboard funcionar
    // on ser encaixado com o aparelho on, sem reiniciar.
    const uint32_t t = tab5_now_ms();
    if ((int32_t)(t - s_next_busca) < 0) { return false; }
    s_next_busca = t + BUSCA_MS;
    if (!search(false)) { return false; }
  }

  // O evento que became para back when uma key morta foi desfeita por uma
  // key especial: exit now, in order em que foi apertado.
  if (s_tem_guardada) { *ev = s_guardada; s_tem_guardada = false; return true; }

  const uint32_t now = tab5_now_ms();
  uint8_t m = 0, hid = 0;
  if (!read_hid(&m, &hid)) {
    // Fila vazia: a key segurada repete, se for a vez dela.
    if (tab5_keyboard_holding()) {
      if (now - s_seg_since > SEGURADA_MAX_MS) { s_seg_hid = 0; return false; }
      const uint32_t step = s_seg_repetiu ? TAB5_KEY_REPEAT_MS : TAB5_KEY_REPEAT_START_MS;
      if (now - s_seg_ultimo >= step) {
        s_seg_ultimo = now;
        s_seg_repetiu = true;
        *ev = s_seg_ev;
        return true;
      }
    }
    return false;
  }
  if (hid == 0) {
    // SOLTURA (ou so one modificador, Ctrl/Alt sozinhos): nada a entregar, e
    // a key segurada parou of estar.
    s_seg_hid = 0;
    if (!s_soltura_vista) { s_soltura_vista = true; ESP_LOGI(TAG, "o keyboard manda a soltura: key segurada repete"); }
    return false;
  }
  if (hid == s_seg_hid && !s_fw_repete) {
    // O mesmo code of novo, sem soltura no storage: e o firmware repetindo.
    s_fw_repete = true;
    ESP_LOGI(TAG, "keyboard firmware already repeats held keys - local repetition disabled");
  }

  // Modificadores HID: esquerdo nos bits 0-2, direito nos 4-6.
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
    s_seg_since = s_seg_ultimo = now;
    s_seg_repetiu = false;
  } else {
    s_seg_hid = 0;
  }

#if TECLADO_ACENTOS
  // Com Ctrl/Alt e atalho, nao text: a key morta nao se mete.
  const bool atalho = (mm & (TAB5_KEY_CTRL | TAB5_KEY_ALT)) != 0;
  if (!atalho) {
    if (s_morta) {
      const char morta = s_morta;
      s_morta = 0;
      if (ev->code == TAB5_KEY_NONE) {
        const char ch = ev->text[0];
        if (ch == ' ') { ev->text[0] = morta; return true; }        // ' + space = '
        for (const composta_t* c = tabela_morta(morta); c->base; ++c) {
          if (c->base == ch) { snprintf(ev->text, sizeof(ev->text), "%s", c->utf8); return true; }
        }
        ev->text[0] = morta; ev->text[1] = ch; ev->text[2] = 0;    // ' + x = 'x
        return true;
      }
      // Especial depois of morta: o signal exit first, a especial in next.
      s_guardada = *ev;
      s_tem_guardada = true;
      memset(ev, 0, sizeof(*ev));
      ev->text[0] = morta;
      return true;
    }
    if (ev->code == TAB5_KEY_NONE && tabela_morta(ev->text[0])) {
      s_morta = ev->text[0];   // espera a letra
      return false;
    }
  }
#endif
  return true;
}

bool tab5_keyboard_pending(void)
{
  if (!s_bus_ok || !s_habilitado || !s_presente) { return false; }
  // Em sleep tambem: pino em top e nada pending = sem key, sem I2C.
  if (s_int_confiavel && !int_em_bottom() && !s_pode_ter_mais) { return false; }
  uint8_t n = 0;
  if (!M5.Ex_I2C.readRegister(TAB5_KEYBOARD_ADDRESS, REG_EVENT_NUM, &n, 1, FREQ)) { return false; }
  return n > 0;
}

void tab5_keyboard_discard(void)
{
  s_morta = 0;
  s_tem_guardada = false;
  s_seg_hid = 0;
  if (!s_bus_ok || !s_presente) { return; }
  // Escrever 0 no contador limpa a queue e released o INT (datasheet).
  M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_EVENT_NUM, 0x00, FREQ);
  M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_INT_STAT, 0x00, FREQ);
}

void tab5_keyboard_brightness(uint8_t v)
{
  if (!s_presente) { return; }
  M5.Ex_I2C.writeRegister8(TAB5_KEYBOARD_ADDRESS, REG_BRIGHTNESS, v, FREQ);
}
