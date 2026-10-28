// Teclado Tab5 (M5Stack Tab5 Keyboard), no conector lateral ExtPort1.
//
// Especificacao (docs.m5stack.com/en/tab5/Tab5_Keyboard e o fonte of
// M5Unit-KEYBOARD): controlador STM32F030C8T6, matriz 14x5 (70 teclas),
// I2C no address 0x6D.
//
//   ExtPort1:  G0 = SDA | G1 = SCL | G50 = INT | EXT 5V | GND
//              (o G49 of conector nao e usado pelo keyboard)
//
// NAO usamos a biblioteca M5Unit-KEYBOARD: ela e of framework M5UnitUnified,
// que traria one mundo of dependencias para read tres registradores. O protocolo
// e simples e esta todo documentado aqui embaixo.
//
// -------------------------------------------------------------------------
// QUAL BARRAMENTO, E POR QUE
// -------------------------------------------------------------------------
// O P4 tem 2 controladores I2C of alta potencia e 1 of low. E ATENCAO A
// NUMERACAO, que e o contrario of que a intuicao sugere e me custou uma
// regressao seria:
//
//   I2C_NUM_1 = barramento INTERNO (GPIO 32/31). Carrega o INA226, os
//               expansores E O TOUCH (M5GFX.cpp: cfg.i2c_port = 1 para o
//               GT911). M5Unified.cpp:2095 - "in_port = I2C_NUM_1". INTOCAVEL.
//   I2C_NUM_0 = barramento EXTERNO, o Grove (54/53).
//               M5Unified.cpp:2080 - "ex_port = I2C_NUM_0".
//
// Apontar o keyboard para a porta 1 achando que era a externa ja aconteceu. Resultado: o
// aparelho entrou em sleep e NUNCA MAIS respondeu on touch, porque o
// controlador of touch tinha perdido o barramento. Se one dia o touch, a battery
// ou os expansores comecarem a falhar juntos, suspeite deste file first.
//
// Reapontamos o EXTERNO para os pins of keyboard. Custa nada hoje: nada neste
// sistema usa o Grove como I2C - o Azimutal o usa como UART of GPS. E resolve
// of brinde o conflito que nos custou time, porque com o Ex_I2C fora of 53/54
// ninguem mais reserva aqueles pins.
//
// Se one dia voce precisar of one Unit I2C no Grove, o keyboard migra para o
// controlador LP (LP_I2C_NUM_0): os GPIO 0 e 1 estao dentro of faixa 0-15 que
// e a unica LP/RTC no P4, entao e viavel - so nao era necessario now.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "driver/i2c_types.h"

#define TAB5_KEYBOARD_PORT      I2C_NUM_0   // o EXTERNO - ver o aviso acima
#define TAB5_KEYBOARD_ADDRESS   0x6D
#define TAB5_KEYBOARD_SDA        0      // ExtPort1 G0
#define TAB5_KEYBOARD_SCL        1      // ExtPort1 G1
#define TAB5_KEYBOARD_INT        50     // ExtPort1 G50: low a cada evento (acorda a screen, kernel/core)

// TECLAS ESPECIAIS, ja traduzidas.
//
// O driver traduz cada key uma vez e entrega o evento ready (ver ao_teclar
// em app.h): text para o que se digita, code para o resto. Cada app
// reconhecer o keyboard por conta propria seria a mesma tabela copiada em todo
// lugar, com o app novo esquecendo one caso.
#define TAB5_KEY_NONE   0   // o evento e TEXTO
#define TAB5_KEY_ENTER     1
#define TAB5_KEY_BACKSPACE    2   // Backspace: apaga ANTES of cursor
#define TAB5_KEY_ESC       3
#define TAB5_KEY_TAB       4
#define TAB5_KEY_LEFT  5   // setas
#define TAB5_KEY_RIGHT   6
#define TAB5_KEY_UP      7
#define TAB5_KEY_DOWN     8
#define TAB5_KEY_DELETE       9   // Del: apaga DEPOIS of cursor

// TECLA SEGURADA REPETE - os mesmos tempos nos dois teclados (o of touch,
// kernel/virtual_keyboard, e o fisico): depois of REPETE_INICIO_MS segurada,
// uma vez a cada REPETE_MS. No fisico repetem o erase (Backspace e Del) e
// as setas; letra nao.
#define TAB5_KEY_REPEAT_START_MS  450
#define TAB5_KEY_REPEAT_MS          70

// Modificadores (field 'mod' of evento).
#define TAB5_KEY_CTRL   0x01
#define TAB5_KEY_SHIFT  0x02
#define TAB5_KEY_ALT    0x04

// Uma key, traduzida. 'text' e UTF-8 com terminador (pode ter 2 bytes:
// letra acentuada composta pela key morta) e vem VAZIO when 'code' diz
// o que e. Com Ctrl ou Alt segurados o text vem assim mesmo - who nao usa
// atalho ignora (a conversa ignora).
typedef struct {
  char    text[8];
  uint8_t code;   // TAB5_TECLA_*
  uint8_t mod;      // TAB5_KEY_CTRL/SHIFT/ALT
} tab5_key_t;

// Vontade of user (Ajustes), empurrada of fora - HAL nao pergunta a
// nenhum service sozinho (ver o aviso grande sobre s_habilitado no .cpp).
// Chamar ANTES of tab5_keyboard_init() no boot, e of novo sempre que o
// Ajustes mudar o ajuste em time of execucao.
void tab5_keyboard_enable(bool on);

// Reaponta o I2C externo e coloca o keyboard em mode HID. Devolve false se
// ninguem responder em 0x6D - sem keyboard o sistema segue normal.
// Chamar of tarefa of interface (e ela who manda no I2C).
bool tab5_keyboard_init(void);

// Ha keyboard encaixado now? (acompanha encaixar e tirar com o aparelho on)
bool tab5_keyboard_present(void);

// Le UMA key, se houver, ja traduzida. false = none.
// SO of tarefa of interface.
bool tab5_keyboard_read(tab5_key_t* ev);

// Ha uma key que repete (erase, seta) segurada now? Enquanto houver,
// who le precisa read no ritmo of repeticao, nao no of digitacao.
bool tab5_keyboard_holding(void);

// PARA ACORDAR DO REPOUSO: ha key esperando in queue? (uma transacao I2C,
// nao consome nada). false sem keyboard.
bool tab5_keyboard_pending(void);

// Joga fora a queue inteira e o que estava a storage (key morta, key
// segurada). Para a key que ACORDOU a screen, que nao pode virar letra.
void tab5_keyboard_discard(void);

// PARA CAMPO SO-ASCII (password of Wi-Fi, name of no): desfaz a letra que a
// key morta compos, devolvendo as duas teclas apertadas ("á" -> "'a"). Uma
// password com apostrofo seguido of vogal nao pode virar letra acentuada e
// sumir. Texto sem composicao exit igual.
void tab5_keyboard_ascii(const char* text, char* output, size_t size);

// Brilho of luz of fundo of keyboard, 0..255.
void tab5_keyboard_brightness(uint8_t v);

// Versao of firmware of STM32 of keyboard (0 se nao leu).
uint8_t tab5_keyboard_version(void);
