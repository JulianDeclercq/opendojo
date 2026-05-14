#include "autosave.hpp"

#include <chrono>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "commands.hpp"
#include "drill.hpp"
#include "log.hpp"
#include "players.hpp"
#include "slot.hpp"
#include "subsystems.hpp"

namespace opendojo::autosave {

namespace {

struct State {
    bool          initialized         = false;
    bool          enabled             = false;
    bool          prev_detected       = false;
    // character_id is the canonical change-detection key: cheap uint32
    // compare, no allocation per frame. The matching name string is only
    // refreshed when the id actually changes — most frames touch neither.
    std::uint32_t prev_character_id   = 0;
    std::string   prev_character_name;

    // When non-empty, an autoload is pending for this character. Cleared
    // when load_drill returns ok, when no autosave file exists, or when
    // we've given up after too many real load_drill failures.
    std::string   pending_load;
    // `failures` counts only attempts where load_drill itself returned
    // !ok — drives the give-up threshold. pool1-not-allocated and
    // file-missing don't count; those retry indefinitely.
    int           failures            = 0;
    int           frames_until_retry  = 0;

    // Practice-mode gate. We tick only when subsystems::lookup(KEY_GAMEPLAY)
    // is non-zero (we're in a practice scene). A small grace window keeps
    // us live for a few frames after the subsystem clears so the
    // exit-from-practice save still fires.
    int           frames_outside_practice = 9999;  // start firmly outside
};
State g_s;

constexpr int MAX_FAILURES         = 3;     // give up after this many load_drill !ok
constexpr int RETRY_INTERVAL       = 60;    // poll once per second between retries
constexpr int EXIT_GRACE_FRAMES    = 5;     // keep ticking briefly after leaving practice
constexpr int ENTRY_SETTLE_FRAMES  = 60;    // wait ~1s after entering practice before first autoload attempt

std::filesystem::path autosave_path(std::string_view character) {
    // Character names are pure ASCII (lowercase a-z + digits + underscore)
    // per players::character_name. Safe to embed directly.
    std::string fname;
    fname.reserve(16 + character.size());
    fname += "_autosave_";
    fname.append(character.data(), character.size());
    fname += ".drill";
    return commands::drills_dir() / fname;
}

std::filesystem::path marker_path() {
    return commands::drills_dir() / "_autosave_enabled";
}

bool read_marker() {
    std::error_code ec;
    return std::filesystem::exists(marker_path(), ec);
}

bool write_marker(bool on) {
    std::error_code ec;
    if (on) {
        std::filesystem::create_directories(commands::drills_dir(), ec);
        if (ec) return false;
        std::ofstream f(marker_path(), std::ios::trunc);
        if (!f) return false;
        f << "autosave enabled\n";
        return f.good();
    }
    std::filesystem::remove(marker_path(), ec);
    return !ec;
}

void ensure_initialized() {
    if (g_s.initialized) return;
    g_s.initialized = true;
    g_s.enabled = read_marker();
    OPENDOJO_LOG("autosave: initialized (enabled=%d)", g_s.enabled ? 1 : 0);
}

// Snapshot every populated slot into a Drill and write it to disk.
// Returns false only on a real I/O / encode error; an empty pool1 (no
// recordings to save) is treated as a success no-op so we don't spam
// retries.
bool save_for(std::string_view character) {
    if (!subsystems::pool1()) return false;

    std::size_t total_events = 0;
    for (std::size_t i = 0; i < slot::USER_SLOTS; ++i) {
        total_events += slot::event_count(i);
    }
    if (total_events == 0) {
        // Nothing to save — but don't overwrite an existing autosave file
        // with an empty one. Just leave whatever was there.
        return true;
    }

    char timebuf[32];
    {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
    }

    drill::Drill d;
    d.name        = "[autosave] " + std::string(character) + "  " + timebuf;
    d.description = "Auto-saved scratch drill. Overwritten the next time the CPU "
                    "character changes or you leave practice with this character.";
    d.character   = std::string(character);

    for (std::size_t i = 0; i < slot::USER_SLOTS; ++i) {
        if (slot::event_count(i) == 0) continue;
        std::uint8_t buf[slot::SLOT_PITCH];
        if (!slot::read(i, buf)) continue;
        char rn[32];
        std::snprintf(rn, sizeof(rn), "slot %zu", i + 1);
        d.recordings.push_back(drill::make_recording(rn, buf));
    }
    if (d.recordings.empty()) return true;

    std::error_code ec;
    std::filesystem::create_directories(commands::drills_dir(), ec);
    if (ec) {
        OPENDOJO_LOG("autosave: couldn't create drills dir: %s", ec.message().c_str());
        return false;
    }

    auto path = autosave_path(character);
    auto text = drill::encode_text(d);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        OPENDOJO_LOG("autosave: open-for-write failed: %ls", path.c_str());
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f.good()) {
        OPENDOJO_LOG("autosave: write failed: %ls", path.c_str());
        return false;
    }
    OPENDOJO_LOG("autosave: saved %zu recordings for %s -> %ls",
                 d.recordings.size(),
                 std::string(character).c_str(),
                 path.c_str());
    return true;
}

enum class LoadResult {
    Ok,         // either loaded successfully or no autosave file exists
    NotReady,   // pool1 not allocated yet — retry, don't count as failure
    Failed,     // load_drill returned !ok — counts toward give-up threshold
};

LoadResult try_load_once(std::string_view character) {
    auto path = autosave_path(character);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        OPENDOJO_LOG("autosave: no scratch drill for %s — fresh start",
                     std::string(character).c_str());
        return LoadResult::Ok;
    }
    if (!subsystems::pool1()) return LoadResult::NotReady;

    auto r = commands::load_drill(path, commands::LoadMode::ReplaceAll);
    if (r.ok) {
        OPENDOJO_LOG("autosave: loaded for %s — %s",
                     std::string(character).c_str(), r.message.c_str());
        return LoadResult::Ok;
    }
    return LoadResult::Failed;
}

void clear_pending() {
    g_s.pending_load.clear();
    g_s.failures           = 0;
    g_s.frames_until_retry = 0;
}

}  // anonymous namespace

bool is_enabled() {
    ensure_initialized();
    return g_s.enabled;
}

void set_enabled(bool on) {
    ensure_initialized();
    if (on == g_s.enabled) return;
    if (!write_marker(on)) {
        OPENDOJO_LOG("autosave: failed to persist toggle (target %d)", on ? 1 : 0);
        return;
    }
    g_s.enabled = on;
    OPENDOJO_LOG("autosave: %s", on ? "enabled" : "disabled");

    // Treat the toggle as a "reset point": snapshot the current state into
    // prev_* so we don't immediately fire a transition save/load on the
    // very next tick.
    auto cpu = players::detect_cpu();
    g_s.prev_detected       = cpu.detected;
    g_s.prev_character_id   = cpu.character_id;
    g_s.prev_character_name = cpu.character_name;
    clear_pending();
}

void tick() {
    ensure_initialized();
    if (!g_s.enabled) return;

    // Practice-mode gate. KEY_GAMEPLAY's bound pointer is non-zero only
    // while a practice scene is fully resolved — it clears on every scene
    // transition out of practice (return to menu, switch to ranked, etc.)
    // and on character-switch transitions briefly within practice.
    //
    // We keep ticking for EXIT_GRACE_FRAMES after the subsystem clears so
    // the exit-save can still fire: when the user leaves practice, the
    // subsystem clears within ~1 frame but the Player-struct holder may
    // take a few more frames to null out; we want to catch that
    // detected→undetected transition and save the previous character's
    // pool1 snapshot. Five frames (~83ms at 60fps) is enough.
    const bool in_practice = subsystems::lookup(subsystems::KEY_GAMEPLAY) != 0;
    if (in_practice) {
        g_s.frames_outside_practice = 0;
    } else {
        ++g_s.frames_outside_practice;
    }
    if (g_s.frames_outside_practice > EXIT_GRACE_FRAMES) return;

    // While firmly in practice (not the exit-grace tail), force pool1+pool2
    // allocation so autoload doesn't have to wait for the user to record
    // first. Idempotent and cheap once allocated.
    if (in_practice) {
        subsystems::ensure_pool_allocated();
    }

    auto cpu = players::detect_cpu();

    // Change detection: cheap uint32 compare. The matching name is only
    // refreshed below when this flag is true, so the no-change steady
    // state allocates nothing.
    const bool changed = cpu.detected != g_s.prev_detected
                      || (cpu.detected && cpu.character_id != g_s.prev_character_id);

    // Save the OLD state when we leave practice or switch character.
    if (changed && g_s.prev_detected) {
        save_for(g_s.prev_character_name);
    }

    // Queue a load when we enter practice or switch to a new character.
    // Settle delay: when we just entered practice from outside (the
    // detected=false→true case), wait a beat before the first load
    // attempt. The game's own practice-mode init runs in the few frames
    // after KEY_GAMEPLAY appears; if we autoload too early our state
    // writes get clobbered by the game's setup pass — the in-game UI
    // then never shows the recordings even though pool1 is correct.
    // Within-practice character switches don't need the delay.
    if (changed && cpu.detected) {
        g_s.pending_load = cpu.character_name;
        g_s.failures           = 0;
        g_s.frames_until_retry = g_s.prev_detected ? 0 : ENTRY_SETTLE_FRAMES;
    }

    // Retry pending load at a flat one-per-second cadence. pool1-not-yet-
    // allocated polls forever at this rate; real load_drill failures count
    // toward MAX_FAILURES (3) and then give up.
    if (!g_s.pending_load.empty()) {
        if (g_s.frames_until_retry > 0) {
            --g_s.frames_until_retry;
        } else {
            auto rs = try_load_once(g_s.pending_load);
            if (rs == LoadResult::Ok) {
                clear_pending();
            } else if (rs == LoadResult::Failed && ++g_s.failures >= MAX_FAILURES) {
                OPENDOJO_LOG("autosave: giving up on autoload for %s "
                             "after %d failed load_drill attempts",
                             g_s.pending_load.c_str(), g_s.failures);
                clear_pending();
            } else {
                g_s.frames_until_retry = RETRY_INTERVAL;
            }
        }
    }

    // Only refresh prev_character_name on actual change — saves the per-
    // frame string assignment when the character hasn't moved.
    if (changed) {
        g_s.prev_character_name = cpu.character_name;
    }
    g_s.prev_character_id = cpu.character_id;
    g_s.prev_detected     = cpu.detected;
}

}  // namespace opendojo::autosave
