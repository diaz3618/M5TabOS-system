// O DIARIO DE GRAVACOES ADIADAS - o nucleo, sem ESP-IDF.
//
// Quem usa e o tab5_journal (o mesmo nucleo sobre a particao "reserva" da
// flash); este arquivo e puro para rodar no Mac (testes/rodar_reserva.sh),
// com uma flash de mentira que se comporta como a de verdade.
//
// -------------------------------------------------------------------------
// PARA QUE SERVE
// -------------------------------------------------------------------------
// Enquanto o cartao esta emprestado ao computador (modo pendrive), o que o
// sistema grava nele vira REGISTRO aqui, na ordem em que aconteceu. Quando o
// cartao volta, os registros sao aplicados no cartao na mesma ordem e o
// diario e apagado. A flash guarda mesmo sem energia: um reinicio no meio do
// pendrive nao perde nada.
//
// -------------------------------------------------------------------------
// AS REGRAS DA FLASH QUE ISTO RESPEITA
// -------------------------------------------------------------------------
// - Gravar so transforma 1 em 0. Area apagada e 0xFF; gravar de novo por cima
//   de algo ja gravado nao funciona - so APAGAR (setor de 4 KB inteiro) volta
//   tudo a 0xFF. Por isso o diario so cresce, e e apagado inteiro no fim.
// - A excecao util: a marca "aplicado" de cada registro nasce 0xFF e vira
//   0x00 depois de o registro ir para o cartao - transformar 1 em 0 pode. E o
//   que deixa a aplicacao recomecar de onde parou depois de um reinicio, sem
//   repetir linha no cartao.
//
// -------------------------------------------------------------------------
// QUEDA DE ENERGIA NO MEIO DE UMA GRAVACAO
// -------------------------------------------------------------------------
// Cada registro leva um CRC de si mesmo. O registro que ficou pela metade
// falha o CRC e e ignorado; todos os anteriores continuam valendo. Os bytes
// que ele chegou a gravar nao podem ser regravados, entao o proximo registro
// comeca no proximo setor que ainda esta apagado ("sujo" no estado). A
// varredura faz o mesmo salto e acha os registros que vieram depois.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RESERVA_SETOR  4096u

typedef enum {
  RESERVA_ACRESCENTAR = 1,   // linha(s) no fim do arquivo (com cabecalho se ele estiver vazio)
  RESERVA_SUBSTITUIR  = 2,   // o arquivo inteiro passa a ser 'dados'
} reserva_tipo_t;

// O meio de gravacao: a particao da flash, ou a de mentira do teste.
typedef struct {
  bool  (*ler)(void* ctx, uint32_t pos, void* buf, uint32_t n);
  bool  (*gravar)(void* ctx, uint32_t pos, const void* buf, uint32_t n);
  bool  (*apagar)(void* ctx, uint32_t pos, uint32_t n);    // multiplos de RESERVA_SETOR
  void* (*alocar)(size_t n);                               // para ler um registro inteiro
  void  (*liberar)(void* p);
  void*    ctx;
  uint32_t tamanho;                                        // multiplo de RESERVA_SETOR
} reserva_meio_t;

// Um registro, como quem grava entrega e como quem aplica recebe. Os textos
// NAO terminam em NUL dentro do diario; na aplicacao chegam terminados.
typedef struct {
  uint8_t     tipo;          // reserva_tipo_t
  uint8_t     flags;         // do dono (tab5_sd: TAB5_SD_SIGILOSO)
  const char* app;           // a pasta do app ("Mesh", "Sistema/logs")
  const char* arquivo;
  const void* cabecalho;     // ACRESCENTAR: escrito antes se o arquivo estiver vazio
  uint16_t    n_cabecalho;
  const void* dados;
  uint32_t    n_dados;
  // SUBSTITUIR: como o arquivo estava quando o cartao foi emprestado. Na
  // volta, se ele nao estiver mais assim, foi o computador que mexeu.
  bool        base_existia;
  uint32_t    base_tamanho;
  uint32_t    base_data;     // data/hora FAT (st_mtime)
} reserva_reg_t;

typedef struct {
  reserva_meio_t meio;
  uint32_t fim;         // onde o proximo registro comeca (sempre em area apagada)
  uint32_t validos;     // registros inteiros no diario
  uint32_t pendentes;   // dos validos, os que ainda nao foram para o cartao
  bool     sujo;        // houve registro pela metade (queda de energia)
  bool     pronto;
} reserva_t;

// Varre o meio e deixa o estado pronto. false = meio ilegivel.
bool reserva_abrir(reserva_t* r, const reserva_meio_t* meio);

// Acrescenta um registro. false = diario cheio ou erro de gravacao (o
// registro nao entrou; os anteriores continuam intactos).
bool reserva_gravar(reserva_t* r, const reserva_reg_t* reg);

// Aplica os pendentes EM ORDEM. 'fn' grava um registro no destino e devolve
// true quando ele ja esta la de forma duravel; so entao o registro e marcado
// como aplicado. Para no primeiro false (a ordem importa: nao se pula um).
// Sem pendentes no fim, o diario e apagado. Devolve quantos foram aplicados
// nesta chamada, ou -1 se o meio falhou.
typedef bool (*reserva_aplicar_fn)(const reserva_reg_t* reg, void* ctx);
int reserva_aplicar(reserva_t* r, reserva_aplicar_fn fn, void* ctx);

// Bytes ocupados (registros e lixo) e o total do meio.
static inline uint32_t reserva_usado(const reserva_t* r) { return r->fim; }
