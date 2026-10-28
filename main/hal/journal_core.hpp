// O DIARIO DE GRAVACOES ADIADAS - o nucleo, sem ESP-IDF.
//
// Quem usa e o tab5_journal (o mesmo nucleo sobre a particao "reserva" of
// flash); este file e puro para rodar no Mac (testes/rodar_reserva.sh),
// com uma flash of mentira que se comporta como a of verdade.
//
// -------------------------------------------------------------------------
// PARA QUE SERVE
// -------------------------------------------------------------------------
// Enquanto o cartao esta lent on computador (mode pendrive), o que o
// sistema grava nele vira REGISTRO aqui, in order em que aconteceu. Quando o
// cartao iteration, os registros sao aplicados no cartao in mesma order e o
// diario e apagado. A flash guarda mesmo sem energia: one reinicio no storage of
// pendrive nao perde nada.
//
// -------------------------------------------------------------------------
// AS REGRAS DA FLASH QUE ISTO RESPEITA
// -------------------------------------------------------------------------
// - Gravar so transforma 1 em 0. Area apagada e 0xFF; write of novo por top
//   of algo ja gravado nao funciona - so APAGAR (setor of 4 KB inteiro) iteration
//   everything a 0xFF. Por isso o diario so cresce, e e apagado inteiro no end.
// - A excecao util: a brand "aplicado" of cada registro nasce 0xFF e vira
//   0x00 depois of o registro ir para o cartao - transformar 1 em 0 pode. E o
//   que deixa a aplicacao recomecar of onde parou depois of one reinicio, sem
//   repetir line no cartao.
//
// -------------------------------------------------------------------------
// QUEDA DE ENERGIA NO MEIO DE UMA GRAVACAO
// -------------------------------------------------------------------------
// Cada registro leva one CRC of si mesmo. O registro que became pela half
// failure o CRC e e ignorado; todos os anteriores continuam valendo. Os bytes
// que ele chegou a write nao podem ser regravados, entao o next registro
// comeca no next setor que ainda esta apagado ("dirty" no state). A
// varredura faz o mesmo salto e acha os registros que vieram depois.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define JOURNAL_SECTOR  4096u

typedef enum {
  JOURNAL_APPEND = 1,   // line(s) no end of file (com header se ele estiver vazio)
  JOURNAL_REPLACE  = 2,   // o file inteiro passa a ser 'data'
} journal_entry_type_t;

// O storage of gravacao: a particao of flash, ou a of mentira of teste.
typedef struct {
  bool  (*read)(void* ctx, uint32_t pos, void* buf, uint32_t n);
  bool  (*write)(void* ctx, uint32_t pos, const void* buf, uint32_t n);
  bool  (*erase)(void* ctx, uint32_t pos, uint32_t n);    // multiplos of JOURNAL_SECTOR
  void* (*allocate)(size_t n);                               // para read one registro inteiro
  void  (*release)(void* p);
  void*    ctx;
  uint32_t size;                                        // multiplo of JOURNAL_SECTOR
} journal_storage_t;

// Um registro, como who grava entrega e como who aplica recebe. Os textos
// NAO terminam em NUL dentro of diario; in aplicacao chegam terminados.
typedef struct {
  uint8_t     type;          // journal_entry_type_t
  uint8_t     flags;         // of owner (tab5_sd: TAB5_SD_SIGILOSO)
  const char* app;           // a folder of app ("Mesh", "Sistema/logs")
  const char* file;
  const void* header;     // ACRESCENTAR: escrito antes se o file estiver vazio
  uint16_t    header_size;
  const void* data;
  uint32_t    data_size;
  // SUBSTITUIR: como o file estava when o cartao foi lent. Na
  // iteration, se ele nao estiver mais assim, foi o computador que mexeu.
  bool        base_existia;
  uint32_t    base_tamanho;
  uint32_t    base_data;     // data/hora FAT (st_mtime)
} journal_entry_t;

typedef struct {
  journal_storage_t storage;
  uint32_t end;         // onde o next registro comeca (sempre em area apagada)
  uint32_t valid_entries;     // registros inteiros no diario
  uint32_t pending;   // dos valid_entries, os que ainda nao foram para o cartao
  bool     dirty;        // houve registro pela half (queda of energia)
  bool     ready;
} journal_t;

// Varre o storage e deixa o state ready. false = storage ilegivel.
bool journal_open(journal_t* r, const journal_storage_t* storage);

// Acrescenta one registro. false = diario cheio ou erro of gravacao (o
// registro nao entrou; os anteriores continuam intactos).
bool journal_write(journal_t* r, const journal_entry_t* reg);

// Aplica os pending EM ORDEM. 'fn' grava one registro no target e devolve
// true when ele ja esta la of forma duravel; so entao o registro e marcado
// como aplicado. Para no first false (a order importa: nao se pula one).
// Sem pending no end, o diario e apagado. Devolve quantos foram aplicados
// nesta chamada, ou -1 se o storage failed.
typedef bool (*journal_apply_fn)(const journal_entry_t* reg, void* ctx);
int journal_apply(journal_t* r, journal_apply_fn fn, void* ctx);

// Bytes ocupados (registros e lixo) e o total of storage.
static inline uint32_t journal_used(const journal_t* r) { return r->end; }
