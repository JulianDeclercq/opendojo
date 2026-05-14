#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Live read of P1/P2 character ids and human-vs-CPU side, used at drill
// export time to auto-fill the drill's character/cpu_side header.
//
// Implementation reaches the live Player structs via two AOB patterns in
// Polaris's .text section, then dereferences a stable two-level pointer
// chain (the GlobalPlayerHolder) to read character_id at Player+0x168 and
// main_player_info.player_id (which slot the human is controlling).
//
// Patterns and offsets verified live on Tekken 8 v3.00.02. Originally
// reverse-engineered by Irony (github.com/tomislav-ivankovic/Irony).
//
// Detection works *only inside a practice/match scene* — outside a match
// the holder is null. detect_cpu() returns detected=false in that case.

namespace opendojo::players {

enum class Side : std::uint8_t { p1 = 0, p2 = 1 };

struct CpuInfo {
    bool          detected     = false;       // false => not in a match / pattern miss
    std::uint32_t character_id = 0;            // raw u32 from the Player struct
    std::string   character_name;              // "jin" or "unknown_<id>"
    Side          cpu_side     = Side::p2;     // which game slot the CPU occupies
};

// One-shot resolution: re-walks the pointer chain on every call (the
// holder and Player addresses are not stable across scenes). Cheap — just
// a handful of memory reads after the first-call pattern scan caches the
// holder-pointer slot address.
CpuInfo detect_cpu();

// Stringify / parse Side for drill file headers.
const char* side_to_string(Side s);
bool        parse_side(std::string_view s, Side& out);

// Character id -> lowercase name (e.g. 6 -> "jin"). Returns nullptr for
// ids outside the known roster; callers should format "unknown_<id>".
const char* character_name(std::uint32_t id);

}  // namespace opendojo::players
