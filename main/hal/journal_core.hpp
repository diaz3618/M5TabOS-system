// O DIARIO DE GRAVACOES ADIADAS - o nucleo, sem ESP-IDF.
//
// Quem usa e o tab5_journal (o mesmo nucleo sobre a particao "reserva" da
// flash); este file e puro para rodar no Mac (testes/rodar_reserva.sh),
// com uma flash de mentira que se comporta como a de verdade.
//
// -------------------------------------------------------------------------
// PARA QUE SERVE
// -------------------------------------------------------------------------
// Enquanto o cartao esta lent ao computador (mode pendrive), o que o
// sistema grava nele vira REGISTRO aqui, na ordem em que aconteceu. Quando o
// cartao volta, os registros sao aplicados no cartao na mesma ordem e o
// diario e apagado. A flash guarda mesmo sem energia: um reinicio no storage do
// pendrive nao perde nada.
//
// -------------------------------------------------------------------------
// AS REGRAS DA FLASH QUE ISTO RESPEITA
// -------------------------------------------------------------------------
// - Gravar so transforma 1 em 0. Area apagada e 0xFF; write de novo por cima
//   de algo ja gravado nao funciona - so APAGAR (setor de 4 KB inteiro) volta
//   tudo a 0xFF. Por isso o diario so cresce, e e apagado inteiro no end.
// - A excecao util: a brand "aplicado" de cada registro nasce 0xFF e vira
//   0x00 depois de o registro ir para o cartao - transformar 1 em 0 pode. E o
//   que deixa a aplicacao recomecar de onde parou depois de um reinicio, sem
//   repetir linha no cartao.
//
// -------------------------------------------------------------------------
// QUEDA DE ENERGIA NO MEIO DE UMA GRAVACAO
// -------------------------------------------------------------------------
// Cada registro leva um CRC de si mesmo. O registro que ficou pela half
// failure o CRC e e ignorado; todos os anteriores continuam valendo. Os bytes
// que ele chegou a write nao podem ser regravados, entao o proximo registro
// comeca no proximo setor que ainda esta apagado ("dirty" no state). A
// varredura faz o mesmo salto e acha os registros que vieram depois.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define JOURNAL_SECTOR  4096u

typedef enum {
  JOURNAL_APPEND = 1,   // linha(s) no end do file (com header se ele estiver vazio)
  JOURNAL_REPLACE  = 2,   // o file inteiro passa a ser 'data'
} journal_entry_type_t;

// O storage de gravacao: a particao da flash, ou a de mentira do teste.
typedef struct {
  bool  (*read)(void* ctx, uint32_t pos, void* buf, uint32_t n);
  bool  (*write)(void* ctx, uint32_t pos, const void* buf, uint32_t n);
  bool  (*erase)(void* ctx, uint32_t pos, uint32_t n);    // multiplos de JOURNAL_SECTOR
  void* (*allocate)(size_t n);                               // para read um registro inteiro
  void  (*release)(void* p);
  void*    ctx;
  uint32_t size;                                        // multiplo de JOURNAL_SECTOR
} journal_storage_t;

// Um registro, como who grava entrega e como who aplica recebe. Os textos
// NAO terminam em NUL dentro do diario; na aplicacao chegam terminados.
typedef struct {
  uint8_t     type;          // journal_entry_type_t
  uint8_t     flags;         // do owner (tab5_sd: TAB5_SD_SIGILOSO)
  const char* app;           // a folder do app ("Mesh", "Sistema/logs")
  const char* file;
  const void* header;     // ACRESCENTAR: escrito antes se o file estiver vazio
  uint16_t    header_size;
  const void* data;
  uint32_t    data_size;
  // SUBSTITUIR: como o file estava quando o cartao foi lent. Na
  // volta, se ele nao estiver mais assim, foi o computador que mexeu.
  bool        base_existia;
  uint32_t    base_tamanho;
  uint32_t    base_data;     // data/hora FAT (st_mtime)
} journal_entry_t;

typedef struct {
  journal_storage_t storage;
  uint32_t end;         // onde o proximo registro comeca (sempre em area apagada)
  uint32_t valid_entries;     // registros inteiros no diario
  uint32_t pending;   // dos valid_entries, os que ainda nao foram para o cartao
  bool     dirty;        // houve registro pela half (queda de energia)
  bool     ready;
} journal_t;

// Varre o storage e deixa o state ready. false = storage ilegivel.
bool journal_open(journal_t* r, const journal_storage_t* storage);

// Acrescenta um registro. false = diario cheio ou erro de gravacao (o
// registro nao entrou; os anteriores continuam intactos).
bool journal_write(journal_t* r, const journal_entry_t* reg);

// Aplica os pending EM ORDEM. 'fn' grava um registro no target e devolve
// true quando ele ja esta la de forma duravel; so entao o registro e marcado
// como aplicado. Para no primeiro false (a ordem importa: nao se pula um).
// Sem pending no end, o diario e apagado. Devolve quantos foram aplicados
// nesta chamada, ou -1 se o storage failed.
typedef bool (*journal_apply_fn)(const journal_entry_t* reg, void* ctx);
int journal_apply(journal_t* r, journal_apply_fn fn, void* ctx);

// Bytes ocupados (registros e lixo) e o total do storage.
static inline uint32_t journal_used(const journal_t* r) { return r->end; }
