#pragma once

#include <cstddef>

// Top-level OpenDojo commands. All operations are self-contained: they
// resolve game state, read/write slots, encode/decode drills, do file I/O,
// and write results to the log. Each command is safe to call from any
// thread (the underlying log is thread-safe; game memory access is
// effectively single-threaded since the user is one human pressing one
// hotkey at a time).
//
// Drill files live in <game>\Polaris\Binaries\Win64\opendojo_drills\.
// Each slot has up to two files:
//   slot_N.drill   v1 text format (canonical, hand-editable)
//   slot_N.bin     legacy OLAB binary container (byte-clone safety net)
// Export writes both. Import prefers .drill, falls back to .bin.

namespace opendojo::commands {

// Read slot N from pool1, encode as text + binary, and write both files.
// Logs progress / errors. slot_idx is 0-based.
void export_slot(std::size_t slot_idx);

// Load slot_N.drill (preferring text; falling back to legacy binary in
// .drill, then to .bin), decode, and write the bytes back into pool1 with
// the recorded flag set. Logs progress / errors.
void import_slot(std::size_t slot_idx);

// Print module base, pool1 status, and per-slot event counts to the log.
void show_status();

}  // namespace opendojo::commands
