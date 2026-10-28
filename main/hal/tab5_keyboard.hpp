// Teclado Tab5 (M5Stack Tab5 Keyboard), no conector lateral ExtPort1.
//
// Especificacao (docs.m5stack.com/en/tab5/Tab5_Keyboard e o fonte da
// M5Unit-KEYBOARD): controlador STM32F030C8T6, matriz 14x5 (70 teclas),
// I2C no endereco 0x6D.
//
//   ExtPort1:  G0 = SDA | G1 = SCL | G50 = INT | EXT 5V | GND
//              (o G49 do conector nao e usado pelo teclado)
//
// NAO usamos a biblioteca M5Unit-KEYBOARD: ela e do framework M5UnitUnified,
// que traria um mundo de dependencias para ler tres registradores. O protocolo
// e simples e esta todo documentado aqui embaixo.
//
// -------------------------------------------------------------------------
// QUAL BARRAMENTO, E POR QUE
// -------------------------------------------------------------------------
// O P4 tem 2 controladores I2C de alta potencia e 1 de baixa. E ATENCAO A
// NUMERACAO, que e o contrario do que a intuicao sugere e me custou uma
// regressao seria:
//
//   I2C_NUM_1 = barramento INTERNO (GPIO 32/31). Carrega o INA226, os
//               expansores E O TOUCH (M5GFX.cpp: cfg.i2c_port = 1 para o
//               GT911). M5Unified.cpp:2095 - "in_port = I2C_NUM_1". INTOCAVEL.
//   I2C_NUM_0 = barramento EXTERNO, o Grove (54/53).
//               M5Unified.cpp:2080 - "ex_port = I2C_NUM_0".
//
// Apontar o teclado para a porta 1 achando que era a externa ja aconteceu. Resultado: o
// aparelho entrou em repouso e NUNCA MAIS respondeu ao toque, porque o
// controlador de toque tinha perdido o barramento. Se um dia o touch, a bateria
// ou os expansores comecarem a falhar juntos, suspeite deste arquivo primeiro.
//
// Reapontamos o EXTERNO para os pinos do teclado. Custa nada hoje: nada neste
// sistema usa o Grove como I2C - o Azimutal o usa como UART do GPS. E resolve
// de brinde o conflito que nos custou tempo, porque com o Ex_I2C fora do 53/54
// ninguem mais reserva aqueles pinos.
//
// Se um dia voce precisar de um Unit I2C no Grove, o teclado migra para o
// controlador LP (LP_I2C_NUM_0): os GPIO 0 e 1 estao dentro da faixa 0-15 que
// e a unica LP/RTC no P4, entao e viavel - so nao era necessario agora.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "driver/i2c_types.h"

#define TAB5_TECLADO_PORTA      I2C_NUM_0   // o EXTERNO - ver o aviso acima
#define TAB5_TECLADO_ENDERECO   0x6D
#define TAB5_TECLADO_SDA        0      // ExtPort1 G0
#define TAB5_TECLADO_SCL        1      // ExtPort1 G1
#define TAB5_TECLADO_INT        50     // ExtPort1 G50: baixa a cada evento (acorda a tela, kernel/core)

// TECLAS ESPECIAIS, ja traduzidas.
//
// O driver traduz cada tecla uma vez e entrega o evento pronto (ver ao_teclar
// em app.h): texto para o que se digita, codigo para o resto. Cada app
// reconhecer o teclado por conta propria seria a mesma tabela copiada em todo
// lugar, com o app novo esquecendo um caso.
#define TAB5_TECLA_NENHUMA   0   // o evento e TEXTO
#define TAB5_TECLA_ENTER     1
#define TAB5_TECLA_APAGAR    2   // Backspace: apaga ANTES do cursor
#define TAB5_TECLA_ESC       3
#define TAB5_TECLA_TAB       4
#define TAB5_TECLA_ESQUERDA  5   // setas
#define TAB5_TECLA_DIREITA   6
#define TAB5_TECLA_CIMA      7
#define TAB5_TECLA_BAIXO     8
#define TAB5_TECLA_DEL       9   // Del: apaga DEPOIS do cursor

// TECLA SEGURADA REPETE - os mesmos tempos nos dois teclados (o de toque,
// kernel/virtual_keyboard, e o fisico): depois de REPETE_INICIO_MS segurada,
// uma vez a cada REPETE_MS. No fisico repetem o apagar (Backspace e Del) e
// as setas; letra nao.
#define TAB5_TECLA_REPETE_INICIO_MS  450
#define TAB5_TECLA_REPETE_MS          70

// Modificadores (campo 'mod' do evento).
#define TAB5_TECLA_CTRL   0x01
#define TAB5_TECLA_SHIFT  0x02
#define TAB5_TECLA_ALT    0x04

// Uma tecla, traduzida. 'texto' e UTF-8 com terminador (pode ter 2 bytes:
// letra acentuada composta pela tecla morta) e vem VAZIO quando 'codigo' diz
// o que e. Com Ctrl ou Alt segurados o texto vem assim mesmo - quem nao usa
// atalho ignora (a conversa ignora).
typedef struct {
  char    texto[8];
  uint8_t codigo;   // TAB5_TECLA_*
  uint8_t mod;      // TAB5_TECLA_CTRL/SHIFT/ALT
} tab5_tecla_t;

// Vontade do usuario (Ajustes), empurrada de fora - HAL nao pergunta a
// nenhum servico sozinho (ver o aviso grande sobre s_habilitado no .cpp).
// Chamar ANTES de tab5_teclado_iniciar() no boot, e de novo sempre que o
// Ajustes mudar o ajuste em tempo de execucao.
void tab5_teclado_habilitar(bool ligado);

// Reaponta o I2C externo e coloca o teclado em modo HID. Devolve false se
// ninguem responder em 0x6D - sem teclado o sistema segue normal.
// Chamar da tarefa de interface (e ela quem manda no I2C).
bool tab5_teclado_iniciar(void);

// Ha teclado encaixado agora? (acompanha encaixar e tirar com o aparelho ligado)
bool tab5_teclado_presente(void);

// Le UMA tecla, se houver, ja traduzida. false = nenhuma.
// SO da tarefa de interface.
bool tab5_teclado_ler(tab5_tecla_t* ev);

// Ha uma tecla que repete (apagar, seta) segurada agora? Enquanto houver,
// quem le precisa ler no ritmo da repeticao, nao no de digitacao.
bool tab5_teclado_segurando(void);

// PARA ACORDAR DO REPOUSO: ha tecla esperando na fila? (uma transacao I2C,
// nao consome nada). false sem teclado.
bool tab5_teclado_pendente(void);

// Joga fora a fila inteira e o que estava a meio (tecla morta, tecla
// segurada). Para a tecla que ACORDOU a tela, que nao pode virar letra.
void tab5_teclado_descartar(void);

// PARA CAMPO SO-ASCII (senha do Wi-Fi, nome do no): desfaz a letra que a
// tecla morta compos, devolvendo as duas teclas apertadas ("á" -> "'a"). Uma
// senha com apostrofo seguido de vogal nao pode virar letra acentuada e
// sumir. Texto sem composicao sai igual.
void tab5_teclado_ascii(const char* texto, char* saida, size_t tam);

// Brilho da luz de fundo do teclado, 0..255.
void tab5_teclado_brilho(uint8_t v);

// Versao de firmware do STM32 do teclado (0 se nao leu).
uint8_t tab5_teclado_versao(void);
