#pragma once

#include <cstdint>

// Game-side singletons & recording pools.
//
// Polaris stores most gameplay subsystems behind a service-locator hash
// map (FUN_1418db8f0). The map lives at *(polaris + CTX_PTR_OFFSET) + 0x10,
// and entries are keyed by a 4-byte hash. The hash for each subsystem is
// itself stored at a known module offset (KEY_*). To resolve a subsystem
// we read the key, look it up in the map, and return the bound pointer.
//
// pool1 / pool2 are the recording-buffer pools holding the slot data.
// They're allocated lazily on first practice-mode recording — until then,
// the pointer reads as 0.

namespace opendojo::subsystems {

// Pointer-storage offsets within Polaris's image.
inline constexpr std::uintptr_t CTX_PTR_OFFSET = 0x9537300;
inline constexpr std::uintptr_t POOL1_PTR_OFFSET = 0x986AC70;
inline constexpr std::uintptr_t POOL2_PTR_OFFSET = 0x986AC78;

// Subsystem key offsets — each names a 4-byte hash key for the locator.
// Comments are best-guess based on bisects + Ghidra; precise semantics
// unconfirmed for most fields.
inline constexpr std::uintptr_t KEY_GAMEPLAY =
    0x9537314;  // practice gameplay state: per-slot recorded flags at +0x480, human-side index at +0x47C
inline constexpr std::uintptr_t KEY_SINGLETON =
    0x95371B0;  // top-level recording session config: side-gate +0x002, "session exists" bit 22 of word0
inline constexpr std::uintptr_t KEY_RECORDING =
    0x95371A4;  // recording subsystem; `this` arg pool_init expects
inline constexpr std::uintptr_t KEY_PLAYERS_SUB =
    0x9537078;  // per-side Player* array natural finalize uses; not always resolved in our context
inline constexpr std::uintptr_t KEY_RECORDPOOL =
    0x9537308;  // TArray of per-CPU-side 0x140-byte objects; holds move-list slot payloads.
                // Resolve via lookup(), then element[cpu_side] (each elem is 0x140 B), then
                // (slot+1)*8*4 byte rows of 8 uint32 channels at +0x44.
inline constexpr std::uintptr_t KEY_SUBB =
    0x953707C;  // playback-session-armed flag at +0x065; writing 0 mid-intro freezes input
inline constexpr std::uintptr_t KEY_SUBC =
    0x9537080;  // global recording-state counter at +0x25C: -1 == none, 1 == ≥1 slot recorded; game writes -1 at end of round-start setup pass
inline constexpr std::uintptr_t KEY_SUBD = 0x9537084;  // purpose unknown

// Walk the service-locator hash map and return the bound pointer for
// the subsystem whose key lives at module+key_offset. Returns 0 if the
// upstream context isn't initialized yet, or if the key isn't found.
//
// IMPORTANT: subsystem addresses are NOT stable. They get reinstantiated
// at every scene transition (character select, match load, return-to-menu)
// and clear to 0 when the user exits practice mode. Always re-resolve at
// the point of use — never cache the returned pointer across calls.
std::uintptr_t lookup(std::uintptr_t key_offset);

// Fast "are we currently in practice mode?" check. Used as the per-frame
// gate for everything OpenDojo runs — menu render, gamepad poll, autosave
// (which has its own grace). Returns true iff KEY_GAMEPLAY resolves
// (the gameplay subsystem only exists in practice). One hash lookup
// (~10 memory reads); cheap enough to call every frame.
bool in_practice();

// Recording-buffer pool bases. 0 until the game allocates them. Naturally
// the game allocates both on the user's first practice recording per
// session via the function at Polaris+0x18E8E00; once allocated they
// persist for the process lifetime.
std::uintptr_t pool1();
std::uintptr_t pool2();

// Force-allocate pool1 and pool2 by calling the same Polaris-side init
// function the game runs on first record. No-op once both pools are
// non-null. Caller is responsible for gating to a sane moment — we only
// invoke this from inside the practice-mode gate so we never touch
// allocation in other game modes. See feedback_opendojo_practice_gate.
void ensure_pool_allocated();

// Sets the "opponent has a recording session loaded" flag on the CPU's
// Player struct (Player+0x39C0). The natural post-save finalize
// (FUN_141911380) writes this; without it the in-game practice UI shows
// "no recordings" until the user opens/closes the pause menu enough
// times to trigger a code path that flips it. `loaded`==true writes 1
// (matching the natural finalize), false writes 0 (matching the
// natural recording-session begin).
//
// Chain: gameplay[0x47C]^1 → side index → sub078_array[side] →
//   opponent_player → +0x39C0.
//
// Returns true if the write landed. Returns false (and writes nothing)
// if any link in the chain is null — typically means we're not in a
// fully-resolved practice match yet.
bool mark_session_loaded(bool loaded);

}  // namespace opendojo::subsystems
