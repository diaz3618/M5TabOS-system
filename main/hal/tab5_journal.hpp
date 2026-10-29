// The diary of postponed recordings in internal flash - see hal/journal_core.hpp
// for the format and rules, and hal/tab5_sd.hpp for WHEN it is used (the
// card slow on computer). The only person talking about this is tab5_sd.
//
// Lives in "reserve" partition (partitions.csv): 2 MB after app. Stay
// empty almost always - it only has records while the card is out, and e
// erased as soon as they go to the card. Wear is negligible: some
// KB per use of pendrive, against ~100 thousand erases per sector.
//
// Safe for any task (a lock on the inside).
#pragma once

#include "journal_core.hpp"

// Find the partition and scan what became of before (one restart in the storage of
// flash drive). Call once at boot, before any recording.
bool tab5_journal_init(void);

bool tab5_journal_write(const journal_entry_t* reg);

// There are records waiting to go on the card.
bool tab5_journal_pending(void);

// Applies the pending ones in order (see journal_apply). Return how many.
int  tab5_journal_apply(journal_apply_fn fn, void* ctx);

// Occupancy, for screen: bytes used, size, pending records.
void tab5_journal_usage(uint32_t* usado, uint32_t* total, uint32_t* pending);
