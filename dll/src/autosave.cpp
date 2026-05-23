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
#include "memory.hpp"
#include "player_hook.hpp"
#include "players.hpp"
#include "slot.hpp"
#include "subsystems.hpp"

namespace opendojo::autosave {

namespace {

struct State {
    bool initialized = false;
    bool enabled = false;
    bool prev_detected = false;
    // character_id is the canonical change-detection key: cheap uint32
    // compare, no allocation per frame. The matching name string is only
    // refreshed when the id actually changes — most frames touch neither.
    std::uint32_t prev_character_id = 0;
    std::string prev_character_name;

    // When non-empty, an autoload is pending for this character. Cleared
    // when load_drill returns ok, when no autosave file exists, or when
    // we've given up after too many real load_drill failures.
    std::string pending_load;
    // `failures` counts only attempts where load_drill itself returned
    // !ok — drives the give-up threshold. pool1-not-allocated and
    // file-missing don't count; those retry indefinitely.
    int failures = 0;
    int frames_until_retry = 0;
    // Frames spent waiting for the round-active gate (player1.frames_since_round_start >= 1).
    // Until this fires, writing recording-flag state during the round
    // intro freezes character input — the singleton +0x002 = 0x40 etc.
    // writes in set_recorded_flag look like "playback armed, awaiting
    // trigger" to the game, and the user can't move until they manually
    // re-evaluate state (open the pause menu, or Select+A reset).
    int round_wait_frames = 0;

    // Practice-mode gate. We tick only while we're inside a practice scene.
    // A small grace window keeps us live for a few frames after the
    // subsystem clears so the exit-from-practice save still fires.
    int frames_outside_practice = 9999;  // start firmly outside

    // Periodic-save counter. Ticks while in practice; when it passes
    // PERIODIC_SAVE_FRAMES we re-snapshot the current character. This
    // is the safety net for the case where the user exits practice
    // without changing characters — at exit the gameplay subsystem may
    // already be cleared so save_for can't read slots, but a periodic
    // save from a few seconds prior captured the data.
    int frames_since_periodic_save = 0;

    // One-shot "this is a fresh practice entry" flag. Set by
    // on_practice_entered (the practice-slot 0→nonzero hook), cleared
    // after we successfully queue an autoload. Currently kept around
    // for set_enabled semantics but no longer load-bearing for the
    // tick-side gate.
    bool autoload_armed = false;

    // Frames since the current pending_load was queued. We always
    // wait at least MIN_WAIT_AFTER_QUEUE frames before processing,
    // regardless of round_active. Reason: on an intra-practice
    // character switch, round_active reads stale-true immediately
    // (new player struct's frames_since_round_start is already
    // non-zero — Tekken seems to seed it rather than zero it). Writing
    // the slot flags before Tekken's switch-init completes gets the
    // flags stomped, leaving the menu showing Empty even though the
    // drill loaded successfully into the pool.
    int frames_since_queue = 0;
};
State g_s;

constexpr int MAX_FAILURES = 3;                // give up after this many load_drill !ok
constexpr int RETRY_INTERVAL = 60;             // poll once per second between retries
constexpr int EXIT_GRACE_FRAMES = 5;           // keep ticking briefly after leaving practice
constexpr int MAX_ROUND_WAIT_FRAMES = 1800;    // 30s safety timeout if round-active never fires
constexpr int MIN_WAIT_AFTER_QUEUE = 60;       // 1s minimum settle window before processing load
constexpr int PERIODIC_SAVE_FRAMES = 30 * 60;  // re-snapshot every 30s of practice

std::filesystem::path autosave_path(std::string_view character) {
    // Character names are pure ASCII (lowercase a-z + digits + underscore)
    // per players::character_name. Safe to embed directly.
    std::string fname;
    fname.reserve(16 + character.size());
    fname += "_autosave_";
    fname.append(character.data(), character.size());
    fname += ".drill.txt";
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
// Returns false only on a real I/O / encode error. Always overwrites
// the existing autosave file for this character. If no slots are
// populated, the file is left untouched (see comment below).
bool save_for(std::string_view character) {
    auto path = autosave_path(character);

    std::size_t populated_slots = 0;
    for (std::size_t i = 0; i < slot::USER_SLOTS; ++i) {
        if (slot::is_populated(i)) ++populated_slots;
    }
    if (populated_slots == 0) {
        // Don't touch the file — preserve whatever was last saved. The
        // most common "0 populated" case is the leave-practice transition
        // where the gameplay subsystem has already cleared and we can't
        // read slot state anymore. Wiping the file there would erase the
        // user's session. (Manually clearing an autosave is a UI action
        // we can add later if needed.)
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
    d.name = "[autosave] " + std::string(character) + "  " + timebuf;
    d.description =
        "Auto-saved scratch drill. Overwritten the next time the CPU "
        "character changes or you leave practice with this character.";
    d.character = std::string(character);

    for (std::size_t i = 0; i < slot::USER_SLOTS; ++i) {
        auto slot_kind = slot::kind(i);
        if (slot_kind == slot::Kind::Empty) continue;
        char rn[32];
        std::snprintf(rn, sizeof(rn), "slot %zu", i + 1);
        if (slot_kind == slot::Kind::MoveList) {
            d.recordings.push_back(drill::make_movelist_recording(rn, slot::movelist_move_id(i)));
        } else {
            std::uint8_t buf[slot::SLOT_PITCH];
            if (!slot::read(i, buf)) continue;
            d.recordings.push_back(drill::make_live_recording(rn, buf));
        }
    }
    if (d.recordings.empty()) return true;

    std::error_code ec;
    std::filesystem::create_directories(commands::drills_dir(), ec);
    if (ec) {
        OPENDOJO_LOG("autosave: couldn't create drills dir: %s", ec.message().c_str());
        return false;
    }

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
    OPENDOJO_LOG("autosave: saved %zu recordings for %s -> %ls", d.recordings.size(),
                 std::string(character).c_str(), path.c_str());
    return true;
}

enum class LoadResult {
    Ok,        // either loaded successfully or no autosave file exists
    NotReady,  // pool1 not allocated yet — retry, don't count as failure
    Failed,    // load_drill returned !ok — counts toward give-up threshold
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
        OPENDOJO_LOG("autosave: loaded for %s — %s", std::string(character).c_str(),
                     r.message.c_str());
        return LoadResult::Ok;
    }
    return LoadResult::Failed;
}

void clear_pending() {
    g_s.pending_load.clear();
    g_s.failures = 0;
    g_s.frames_until_retry = 0;
    g_s.round_wait_frames = 0;
    g_s.frames_since_queue = 0;
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

    // Treat the toggle as a reset point: snapshot current state into
    // prev_* so we don't immediately fire a transition save/load on the
    // next tick. Don't arm autoload here — toggling on mid-practice
    // hits the same unsafe write path as an intra-practice character
    // switch. The user can manually load from the menu.
    auto cached = player_hook::current_cpu();
    g_s.prev_detected = cached.detected;
    g_s.prev_character_id = cached.cpu_character_id;
    if (cached.detected) {
        auto n = players::character_name(cached.cpu_character_id);
        g_s.prev_character_name = n ? n : "";
    } else {
        g_s.prev_character_name.clear();
    }
    g_s.autoload_armed = false;
    clear_pending();
}

void flush_now() {
    ensure_initialized();
    if (!g_s.enabled) return;
    // Use the latest tracked character. prev_* is updated each tick to
    // reflect whatever's live. The practice-controller dtor hook calls
    // this BEFORE the controller tears down so all gameplay subsystems
    // are still readable.
    if (!g_s.prev_detected) return;
    if (g_s.prev_character_name.empty()) return;
    OPENDOJO_LOG("autosave: flush_now for %s", g_s.prev_character_name.c_str());
    save_for(g_s.prev_character_name);
}

void on_practice_entered() {
    ensure_initialized();
    if (!g_s.enabled) return;
    OPENDOJO_LOG("autosave: practice entered — autoload will fire when round is ready");
    // Clear prev-tick state so the next tick treats the upcoming
    // character as a fresh detection and queues an autoload via the
    // normal change-detection path.
    g_s.prev_detected = false;
    g_s.prev_character_id = 0;
    g_s.prev_character_name.clear();
    clear_pending();
    g_s.frames_since_periodic_save = 0;
    // Arm autoload for exactly this practice entry. The tick-side gate
    // consumes the flag the first time it queues a load.
    g_s.autoload_armed = true;
}

void tick() {
    ensure_initialized();
    if (!g_s.enabled) return;

    // Practice-mode gate (matches subsystems::in_practice — the
    // GlobalPlayerHolder chain, not the gameplay-subsystem hash) with an
    // exit grace so we still observe the leave-practice transition for a
    // few frames after it fires.
    const bool in_practice = subsystems::in_practice();
    if (in_practice) {
        g_s.frames_outside_practice = 0;
    } else {
        ++g_s.frames_outside_practice;
    }
    if (g_s.frames_outside_practice > EXIT_GRACE_FRAMES) return;

    // Initial-load fallback: FUN_145E70B40 (the function our detour
    // hooks) doesn't fire on the very first player population — only
    // on subsequent refreshes. So during the brief window between
    // "practice slot non-zero" and "first character refresh", the
    // cache stays detected=false. ensure_fresh() walks the chain
    // once during that window to prime the cache; once detected,
    // it's a single atomic load and the detour takes over.
    player_hook::ensure_fresh();

    // Read the cache populated by player_hook (the MinHook detour on
    // Tekken's "refresh player pointers" function). This replaces the
    // previous per-tick players::detect_cpu chain walk — that walk
    // dereferenced GlobalPlayerHolder which transiently points at
    // freed memory during a character swap, causing AVs that took
    // the game down. The cache is updated synchronously inside the
    // detour, so it always reflects the post-refresh state.
    auto cached = player_hook::current_cpu();
    const char* cached_name = cached.detected ? players::character_name(cached.cpu_character_id)
                                              : nullptr;

    // Change detection: cheap uint32 compare. The matching name is only
    // looked up below when this flag is true, so the no-change steady
    // state allocates nothing.
    const bool changed = cached.detected != g_s.prev_detected ||
                         (cached.detected && cached.cpu_character_id != g_s.prev_character_id);

    // Save the OLD state when we leave practice or switch character.
    if (changed && g_s.prev_detected) { save_for(g_s.prev_character_name); }

    // Periodic safety-net save while in practice. The exit-practice save
    // above can miss data if the gameplay subsystem clears before we get
    // to read slots; with periodic save, the file already reflects a
    // recent snapshot, so at worst we lose the last PERIODIC_SAVE_FRAMES
    // / 60 ≈ 30 seconds of mid-practice work.
    if (cached.detected) {
        ++g_s.frames_since_periodic_save;
        if (g_s.frames_since_periodic_save >= PERIODIC_SAVE_FRAMES) {
            if (cached_name) save_for(cached_name);
            g_s.frames_since_periodic_save = 0;
        }
    } else {
        // Reset so re-entering practice doesn't immediately fire.
        g_s.frames_since_periodic_save = 0;
    }

    // Queue an autoload on EVERY change-to-detected event (initial
    // practice entry OR intra-practice character switch). The
    // round_active gate downstream prevents writing into subsystem
    // state during the round-intro window — and with player_hook
    // driving the cache, the cache only flips AFTER Tekken's
    // FUN_145E70B40 has updated holder.p1/p2 to the NEW player, so
    // round_active() reads a stable frames_since_round_start = 0
    // and waits for the new round to actually start. (Previously
    // this was gated on a fresh-entry flag because the per-tick
    // detect_cpu chain walk read stale data mid-swap and round_active
    // returned true immediately — leading to writes during the
    // transition and a delayed crash. The chain walk is gone now.)
    if (changed && cached.detected && cached_name) {
        g_s.autoload_armed = false;  // consume; harmless if not set
        g_s.pending_load = cached_name;
        g_s.failures = 0;
        g_s.frames_until_retry = 0;
        g_s.round_wait_frames = 0;
        g_s.frames_since_queue = 0;
    }

    // Pending load processing — gated on round-active for the reason above.
    if (!g_s.pending_load.empty()) {
        ++g_s.frames_since_queue;
        const bool min_wait_done = g_s.frames_since_queue >= MIN_WAIT_AFTER_QUEUE;

        if (!min_wait_done || !players::round_active()) {
            ++g_s.round_wait_frames;
            if (g_s.round_wait_frames > MAX_ROUND_WAIT_FRAMES) {
                OPENDOJO_LOG(
                    "autosave: round-active gate didn't fire in %d frames; "
                    "aborting autoload for %s",
                    MAX_ROUND_WAIT_FRAMES, g_s.pending_load.c_str());
                clear_pending();
            }
            // else: keep waiting
        } else if (g_s.frames_until_retry > 0) {
            --g_s.frames_until_retry;
        } else {
            subsystems::ensure_pool_allocated();
            LoadResult rs = try_load_once(g_s.pending_load);

            if (rs == LoadResult::Ok) {
                if (!subsystems::mark_session_loaded(true)) {
                    OPENDOJO_LOG(
                        "autosave: mark_session_loaded returned false "
                        "(see prior log for which chain link)");
                }
                OPENDOJO_LOG(
                    "autosave: autoload complete for %s "
                    "(round-wait was %d frames)",
                    g_s.pending_load.c_str(), g_s.round_wait_frames);
                clear_pending();
            } else if (rs == LoadResult::Failed && ++g_s.failures >= MAX_FAILURES) {
                OPENDOJO_LOG(
                    "autosave: giving up on autoload for %s "
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
    if (changed) { g_s.prev_character_name = cached_name ? cached_name : ""; }
    g_s.prev_character_id = cached.cpu_character_id;
    g_s.prev_detected = cached.detected;
}

}  // namespace opendojo::autosave
