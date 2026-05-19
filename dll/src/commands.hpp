#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Top-level OpenDojo operations. The menu is the primary consumer; hotkeys
// are deferred until we lock down the menu UX.
//
// All filesystem state lives under <game>\Polaris\Binaries\Win64\opendojo\.
// Each file is one v2 drill (one or more recordings); the filename is a
// slug of the drill's `name` field with a `_2`/`_3` collision suffix.

namespace opendojo::commands {

// Absolute path of the opendojo/ data directory next to the game exe.
// Returned for every call — no caching, in case the game gets moved.
std::filesystem::path drills_dir();

// One drill on disk, summarized from its header lines. Cheap to populate —
// the menu uses this to list available drills without parsing event data.
struct DrillHeader {
    std::filesystem::path  path;
    std::string            name;
    std::string            description;
    std::string            character;          // lowercase id; "unknown" if unset
    std::string            cpu_side;           // "p1" / "p2" / "" (unset)
    std::size_t            recording_count = 0;
};

// Scan opendojo/ and return one entry per .drill file. Errors per
// file are silently skipped (the menu shouldn't disappear because one file
// is malformed). Sorted by name.
std::vector<DrillHeader> list_drills();

// How an import places its recordings into the 8 user slots.
enum class LoadMode {
    AppendToFree,   // Fill the lowest-index empty slots in order. If the
                    // drill needs more slots than are free, the load
                    // refuses without touching any slot.
    ReplaceAll,     // Clear all 8 slots, then load the drill's recordings
                    // into slots 1..N starting from slot 1.
};

struct LoadResult {
    bool         ok = false;
    std::string  message;   // user-facing toast string
};

LoadResult load_drill(const std::filesystem::path& path, LoadMode mode);

struct ExportResult {
    bool                   ok = false;
    std::filesystem::path  path;       // saved path (empty on failure)
    std::string            message;
};

// Snapshot every currently-occupied slot (event_count > 0) into one drill
// file. `drill_name` becomes both the `name:` field and the basis for the
// filename slug. Empty `drill_name` -> "drill_YYYYmmdd_HHMMSS".
//
// `description` is written into the header verbatim. `character` and
// `cpu_side` are auto-detected from the live game state at export time
// — pass empty strings to let detection fill them; non-empty values
// override the detection.
ExportResult export_current_slots(std::string_view drill_name,
                                  std::string_view description,
                                  std::string_view character,
                                  std::string_view cpu_side);

// Diagnostic: print module base, pool1 state, and per-slot event counts to
// the log. Useful from the menu's "Show status" button.
void show_status();

}  // namespace opendojo::commands
