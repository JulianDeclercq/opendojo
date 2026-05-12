#pragma once

#include <cstddef>
#include <cstdint>

// Practice-mode slot read/write. Pool1 stores 9 fixed-size slots, of which 8
// are user-facing (indices 0..7) and slot 8 is an in-engine scratch buffer
// for the in-progress recording. We only operate on the 8 user slots.
//
// Slot layout (7202 bytes / 0x1C22 each):
//   +0x00..+0x01   uint16  event_count (little-endian)
//   +0x02..+...    event_count * 4-byte input transition records
//   +...           zero-padding to fill the slot pitch
//
// Writing a slot involves two memory regions:
//   1. The slot bytes in pool1.
//   2. The "this slot has a recording" flag in the gameplay subsystem.
//
// The flag is critical — without it the in-game practice menu shows "Not Set"
// and playback refuses to fire, even with valid bytes in pool1.

namespace opendojo::slot {

inline constexpr std::size_t   SLOT_PITCH = 0x1C22;  // 7202 bytes per slot
inline constexpr std::size_t   USER_SLOTS = 8;

// Per-slot "is recorded" flag inside the gameplay subsystem.
// Each entry is 8 bytes; the flag uint32 sits at +4 within its entry.
inline constexpr std::uintptr_t GAMEPLAY_SLOT_BASE   = 0x480;
inline constexpr std::uintptr_t GAMEPLAY_SLOT_STRIDE = 0x08;
inline constexpr std::uintptr_t GAMEPLAY_SLOT_FLAG   = 0x04;

// Result codes for any operation that touches subsystems. Read-only ops
// (read, event_count, address) don't return this — they just return 0 / false
// since they only need pool1 to be allocated.
enum class WriteStatus {
    Ok,
    InvalidSlot,         // slot_idx out of range
    PoolNotAllocated,    // pool1 ptr still 0 — record once in practice first
    NotInPracticeMode,   // a subsystem lookup returned 0 — user left the scene
};

// Human-readable rendering for log lines.
const char* describe(WriteStatus s);

// Absolute address of slot N's first byte within pool1. Returns 0 if pool1
// isn't allocated yet or slot_idx is out of range.
std::uintptr_t address(std::size_t slot_idx);

// uint16 event count at the start of slot N. 0 if pool not allocated.
std::uint16_t event_count(std::size_t slot_idx);

// Copy the full 7202-byte slot payload into `out`. Caller owns the buffer.
// Returns false if pool not allocated or slot out of range.
bool read(std::size_t slot_idx, std::uint8_t* out);

// Write 7202 bytes into the slot and set the per-slot "recorded" flag.
// Requires subsystems to be alive (practice mode active).
WriteStatus write(std::size_t slot_idx, const std::uint8_t* data);

// Flip just the recorded flag for a slot (4 writes across gameplay /
// singleton / subB / subC). Exposed for diagnostics & tests.
WriteStatus set_recorded_flag(std::size_t slot_idx, bool recorded);

}  // namespace opendojo::slot
