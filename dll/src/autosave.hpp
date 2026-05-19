#pragma once

// Per-character autosave / autoload of practice-mode recordings.
//
// When enabled, the contents of pool1 are persisted to a per-character
// "scratch" drill file every time:
//   - the user leaves practice (detected -> not detected), or
//   - the CPU character changes mid-session.
// On entering practice (or on a character change), if a scratch drill
// exists for the new character, it's loaded back into pool1 via
// commands::load_drill(..., ReplaceAll).
//
// Scratch drills live at opendojo/_autosave_<character>.drill.
// The leading underscore hides them from the Drills tab listing.
// The toggle is persisted to opendojo/_autosave_enabled — a
// marker file whose mere existence means "on".
//
// Limitations:
//   - pool1 is allocated lazily by the game on the FIRST practice
//     recording per process launch. Until that happens, autoload retries
//     each frame and reports nothing. After a single user-initiated
//     recording, pool1 is alive for the rest of the session.

namespace opendojo::autosave {

bool is_enabled();
void set_enabled(bool on);

// Call from the render hook once per frame. Cheap when disabled (early
// return). When enabled, runs detect_cpu() and a handful of memory
// reads, and triggers file I/O only on state transitions.
void tick();

}  // namespace opendojo::autosave
