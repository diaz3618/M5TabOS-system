// THE DIARY OF POSTPONED RECORDINGS - the nucleus, without ESP-IDF.
//
// Who uses it is tab5_journal (the same core on the "reserve" partition of
// flash); this file is pure to run on Mac (testes/rodar_reserva.sh),
// with a flash of lie that behaves like the flash of truth.
//
// ------------------------------------------------------------------------
// WHAT IS IT FOR
// ------------------------------------------------------------------------
// While the card is idling on the computer (pendrive mode), what
// system records in it becomes RECORD here, in order in which it happened. When the
// iteration card, the records are applied to the card in the same order and the
// diary and deleted. The flash saves even without power: one restart in the storage of
// pendrive doesn't lose anything.
//
// ------------------------------------------------------------------------
// THE FLASH RULES THIS RESPECTS
// ------------------------------------------------------------------------
// - Recording only turns 1 into 0. Erased area is 0xFF; write of new by top
// of something already recorded does not work - just DELETE (whole sector of 4 KB) iteration
// everything at 0xFF. That's why the diary only grows, and is completely erased at the end.
// - The useful exception: the "applied" brand of each record is born 0xFF and becomes
// 0x00 after the record goes to the card - turning 1 into 0 can. And the
// which lets the application pick up where it left off after one restart, without
// repeat line on card.
//
// ------------------------------------------------------------------------
// POWER OUTAGE IN THE MIDDLE OF A RECORDING
// ------------------------------------------------------------------------
// Each record takes one CRC of itself. The record that became by half
// failure o CRC is ignored; all of the above remain valid. The bytes
// that it reached write cannot be rewritten, so the next record
// starts in the next sector that is still erased ("dirty" in the state). A
// scan makes the same jump and finds the records that came later.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define JOURNAL_SECTOR  4096u

typedef enum {
  JOURNAL_APPEND = 1,   // line(s) at end of file (with header if empty)
  JOURNAL_REPLACE  = 2,   // the entire file becomes 'data'
} journal_entry_type_t;

// The storage of recording: the partition of flash, or the partition of test.
typedef struct {
  bool  (*read)(void* ctx, uint32_t pos, void* buf, uint32_t n);
  bool  (*write)(void* ctx, uint32_t pos, const void* buf, uint32_t n);
  bool  (*erase)(void* ctx, uint32_t pos, uint32_t n);    // multiples of JOURNAL_SECTOR
  void* (*allocate)(size_t n);                               // to read one entire record
  void  (*release)(void* p);
  void*    ctx;
  uint32_t size;                                        // multiple of JOURNAL_SECTOR
} journal_storage_t;

// A record, such as who records delivers and how who applies receives. The texts
// DO NOT end in NUL within diary; in application they arrive finished.
typedef struct {
  uint8_t     type;          // journal_entry_type_t
  uint8_t     flags;         // of owner (tab5_sd: TAB5_SD_SIGILOSO)
  const char* app;           // a folder of app ("Mesh", "System/logs")
  const char* file;
  const void* header;     // ADD: written before if the file is empty
  uint16_t    header_size;
  const void* data;
  uint32_t    data_size;
  // REPLACE: how the file was when the card was slow. In
  // iteration, if it's not like this anymore, it was the computer that messed up.
  bool        base_existia;
  uint32_t    base_size;
  uint32_t    base_data;     // FAT datetime (st_mtime)
} journal_entry_t;

typedef struct {
  journal_storage_t storage;
  uint32_t end;         // where the next record begins (always in a blank area)
  uint32_t valid_entries;     // entire journal entries
  uint32_t pending;   // of the valid_entries, those that have not yet been added to the card
  bool     dirty;        // there was a record at half (power outage)
  bool     ready;
} journal_t;

// Scans the storage and leaves the state ready. false = unreadable storage.
bool journal_open(journal_t* r, const journal_storage_t* storage);

// Adds a record. false = diary full or recording error (the
// registration did not enter; the previous ones remain intact).
bool journal_write(journal_t* r, const journal_entry_t* reg);

// Apply the pending IN ORDER. 'fn' writes one record to the target and returns
// true when it is already there in a durable way; only then the record is marked
// as applied. Stop at first false (the order matters: you don't skip one).
// Without pending at the end, the journal is deleted. Returns how many were applied
// on this call, or -1 if the storage failed.
typedef bool (*journal_apply_fn)(const journal_entry_t* reg, void* ctx);
int journal_apply(journal_t* r, journal_apply_fn fn, void* ctx);

// Bytes occupied (registers and garbage) and total of storage.
static inline uint32_t journal_used(const journal_t* r) { return r->end; }
