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
inline constexpr std::uintptr_t CTX_PTR_OFFSET   = 0x9537300;
inline constexpr std::uintptr_t POOL1_PTR_OFFSET = 0x986AC70;
inline constexpr std::uintptr_t POOL2_PTR_OFFSET = 0x986AC78;

// Subsystem key offsets (each names a 4-byte hash key for the locator).
inline constexpr std::uintptr_t KEY_GAMEPLAY  = 0x9537314;
inline constexpr std::uintptr_t KEY_SINGLETON = 0x95371B0;
inline constexpr std::uintptr_t KEY_RECORDING = 0x95371A4;   // the `this` that pool_init expects naturally
inline constexpr std::uintptr_t KEY_SUBB      = 0x953707C;
inline constexpr std::uintptr_t KEY_SUBC      = 0x9537080;
inline constexpr std::uintptr_t KEY_SUBD      = 0x9537084;

// Walk the service-locator hash map and return the bound pointer for
// the subsystem whose key lives at module+key_offset. Returns 0 if the
// upstream context isn't initialized yet, or if the key isn't found.
//
// IMPORTANT: subsystem addresses are NOT stable. They get reinstantiated
// at every scene transition (character select, match load, return-to-menu)
// and clear to 0 when the user exits practice mode. Always re-resolve at
// the point of use — never cache the returned pointer across calls.
std::uintptr_t lookup(std::uintptr_t key_offset);

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

}  // namespace opendojo::subsystems
