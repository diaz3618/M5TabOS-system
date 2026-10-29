// SD card of Tab5, treated as "system disk".
//
// The idea: each application has its folder in /sdcard/apps/<Name>/ and does not
// needs to know nothing about mounting, pins or FAT. It asks for one file inside
// of own folder and writes with normal fopen/fprintf/fwrite - the VFS of IDF
// does the rest.
//
// FIACAO (of own M5Unified, table _pin_table_sd):
// CLK=43 CMD=44 D0=39 D1=40 D2=41 D3=42
// Six pins = SDMMC of 4 bits, not SPI. Much faster, and the P4 allows
// remap because it has GPIO matrix for SDMMC.
//
// MOUNTING BY REFERENCE COUNTING: mount and unmount takes ~100ms and wears out
// nothing, but mounted card consumes. So who needs to call mount(), who
// ends up calling unmount(), and the actual mounting only happens first and then
// disassembly in latest. The system does this on its own for apps that have declared
// usa_sd - the app does not need to call either.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "sd_protocol_types.h"   // sdmmc_card_t (the raw card of pendrive)

// Where the card appears in the file system.
#define TAB5_SD_ROOT   "/sdcard"
#define TAB5_SD_APPS   TAB5_SD_ROOT "/apps"

// Mounts the card (or just increments the count, if it is already mounted).
// Returns false if there is no card or if the assembly failed - in which case the
// count is NOT incremented.
// Prepares the module. Call ONCE, before any task exists.
void tab5_sd_init(void);

bool tab5_sd_mount(void);

// Decreases the count; dismounts from truth when it reaches zero.
void tab5_sd_unmount(void);

bool tab5_sd_mounted(void);

// Diagnosis: uptime (us) of latest assembly and disassembly of true (0 = never).
void tab5_sd_last_transitions(int64_t* montou_us, int64_t* desmontou_us);

// Path of folder of app, CREATED if it does not exist.
// tab5_sd_folder("Azimutal", buf, sizeof buf) -> "/sdcard/apps/Azimutal"
// Problematic characters in name (space, slash, accent) become '_'.
bool tab5_sd_folder(const char* app, char* out, size_t out_size);

// Open one file INSIDE folder of app, creating the folder if necessary.
// mode and the of fopen ("w", "a", "r"...). Returns NULL on any failure.
// FILE* f = tab5_sd_open("Azimutal", "satelites.csv", "a");
FILE* tab5_sd_open(const char* app, const char* file, const char* mode);

// "app" accepts subfolder: "System/logs" -> /sdcard/apps/System/logs (each
// level and created if missing). The subfolders of system:
#define TAB5_SD_LOGS    "Sistema/logs"      // log_YYYY-MM-DD.txt
#define TAB5_SD_POWER "Sistema/energia"   // energy-YYYY-MM-DD.csv

// -------------------------------------------------------------------------------
// THE RECORDING PORTS - all data that the system writes passes through one of them
// -------------------------------------------------------------------------------
// Three ways of writing, and there are only three:
//
// tab5_sd_enqueue_append append line, NO wait (goes in batch)
// tab5_sd_append append one block NOW (who already joins in batch: the log)
// tab5_sd_replace switch the entire file, without ever leaving storage file
//
// Because it's just for them: the card can be BORROWED on the computer (mode
// pendrive, see below). Then whatever is recorded by a door goes to the
// diary in internal flash (hal/tab5_journal) and arrives on card in iteration, in
// order in which it happened. Whoever records with fopen() directly loses the data in this
// time - and, if you wrote on the card with the computer using it, it would corrupt it.
//
// tab5_sd_open() stays for READ, and for new BIG file (photo, firmware
// received by the console): these require the true card, and the system does not
// leaves open who produces them with the slow card (app_t.exige_sd).
//
// ORDER is guaranteed: the three doors, lending and the application of diary
// pass through the same lock. Replacing writes what was supposed to before
// append. The same file must not receive through BOTH ports of
// append (the batch of enqueue_append can exit later).
//
// READING ALWAYS SEES THE LAST WRITE: tab5_sd_open() writes the pending and
// applies the diary of one pendrive BEFORE opening. Who reads with fopen() directly
// (the Files) calls tab5_sd_flush_pending() first.
//
// ---- enqueue_append (the recorder) -----------------------------------------------------
// Previously, SEVEN modules recorded each one on the card in their own way. Now who wants
// append a DELIVERY line and follow: tab5_sd_enqueue_append copies to one buffer
// in PSRAM and iteration in time, without touching the card. A single task ("card")
// writes in batch: mounts ONCE, opens each file ONCE, writes everything what
// it has pending, in order in which it arrived. Records:
// - every 5 s, if there is something pending;
// - in hora, for what was request with 'urgent' (message of conversation);
// - when the buffer goes beyond half.
//
// 'header' (optional): written once, if the file is empty on open
// - a line of title of one CSV.
//
// Failed (no card, buffer full): the line is lost and counts as 'lost' -
// same deal as before, when each module simply didn't record.
bool tab5_sd_enqueue_append(const char* app, const char* file, const char* text,
                    const char* header = nullptr, bool urgent = false);

// Write NOW everything that is pending (and the diary of one pendrive, if the
// card has already returned) and waits to finish. For those who will read outside of
// tab5_sd_open.
void tab5_sd_flush_pending(void);

// ---- append and replace -----------------------------------------------
// Block (card or flash): never of interface task - use worker
// (work_system) or a piggyback of task "card". false = did not record (no
// card and without lending, or full diary).
bool tab5_sd_append(const char* app, const char* file, const void* data, size_t n);

// CONFIDENTIAL: the old content and zero before being deleted (passwords) - in FAT,
// erase only the sectors as free.
#define TAB5_SD_SIGILOSO  0x01
bool tab5_sd_replace(const char* app, const char* file, const void* data, size_t n,
                        uint8_t flags = 0);

// Diagnosis: bytes waiting, lines written and lost since boot.
void tab5_sd_append_status(uint32_t* pending, uint32_t* written, uint32_t* lost);

// RIDE ON THE "card" TASK. A service that you only need, from time to time,
// emptying something of yours on the card (the log) does not gain its own task: it records a
// function, and the "card" task calls it every iteration - no maximum 1 s between
// turns, sometimes less (when one urgent attachment wakes her up). Who gets piggyback
// measures the time itself (esp_timer), without counting laps.
// The function runs in stack of "card" (6 KB): it can mount and open file, no
// can wait a long time (all recordings wait together). Up to 8 (CARRONAS_MAX).
typedef void (*tab5_sd_piggyback_t)(void);
bool tab5_sd_piggyback(tab5_sd_piggyback_t fn);

// Card space, in bytes. Any pointer can be NULL.
bool tab5_sd_space(uint64_t* total, uint64_t* free_space);

// Short description of card to show on screen ("SDHC 29.7GB"), or
// "without card". Never returns NULL.
const char* tab5_sd_description(void);

// The name of card (label of volume, the same as the computer shows), or ""
// if it does not have a name or has not yet mounted this boot. Never NULL.
const char* tab5_sd_name(void);

// -------------------------------------------------------------------------------
// LOAN TO THE COMPUTER - the pendrive mode (services/tab5_flashdrive)
// -------------------------------------------------------------------------------
// The card cannot have TWO file system owners. The FAT of Tab5
// stores table and directory in cache; the computer too. If both
// write, each one overwrites what the other thinks is there - and that is
// card corrupted, no file lost. That's why lend is exclusive:
//
// 1. what was to be written (tab5_sd_enqueue_append) goes to the card first;
// 2. mount() starts to refuse, in time and silently - for the rest of the system
// is as if there was no card (everyone already deals with this);
// 3. wait who already had it return_card, until 'wait_ms'; passed that,
// gives up (SD_LENDING_IN_USE) and everything iteration as it was;
// 4. Unmount the truth (without cache of FAT of Tab5) and initialize the card
// RAW, these are sectors - and that's what the computer receives.
//
// While slow, the recording ports (above) go to the JOURNAL in
// internal flash. When returning, the "card" task applies it, in order: append
// goes to the end of file; replace changes the file IF the computer does not
// moved it (compared to the photo of size and date taken at the loan) -
// moved, the of computer stays and the of Tab5 goes to "<name>.tab5". One
// restarting the storage does not lose anything: the diary is in flash and is applied when
// the card appears.
//
// Blocks (until wait_ms + card initialization): never of task of
// interface.
typedef enum {
  SD_LENT = 0,
  SD_LENDING_IN_USE,      // someone held the card beyond the deadline
  SD_LENDING_NO_CARD,  // there is no card, or he didn't respond
  SD_LENDING_NO_JOURNAL,  // the "reserve" partition does not exist (old table)
} tab5_sd_lending_t;
tab5_sd_lending_t tab5_sd_lend(uint32_t espera_ms);

// The raw card (sdmmc_read_sectors/sdmmc_write_sectors), or NULL if not
// is slow. Only for those who requested the loan.
sdmmc_card_t* tab5_sd_raw(void);

// Returns: released the raw card, mount() iteration working and what became
// waiting and recorded. Call OS after the computer has stopped using the
// card (USB disabled).
void tab5_sd_return(void);

bool tab5_sd_lent(void);

// What happened to the diary of this loan (to the screen).
typedef struct {
  uint32_t applied;     // recordings that were already on the card
  uint32_t conflicts;     // files that the computer changed: the of Tab5 became .tab5
  uint32_t discarded;   // records with no way to apply (invalid name)
  uint32_t lost;      // didn't fit in the diary (since the boot)
  uint32_t in_journal;     // still waiting for the card
  uint32_t journal_used, journal_total;   // bytes
  bool     complete;      // the entire diary has already been applied
} tab5_sd_summary_t;
void tab5_sd_summary(tab5_sd_summary_t* r);
