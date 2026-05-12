#pragma once

// Global Win32 hotkey loop. Spawns a dedicated thread that registers the
// OpenLab key bindings via RegisterHotKey and dispatches WM_HOTKEY messages
// to openlab::commands::*.
//
// Bindings (match the CE Lua script for muscle-memory continuity):
//   F1..F8         export user slot 1..8 to slot_N.drill + slot_N.bin
//   Ctrl+1..8      import slot_N.drill into user slot 1..8
//   F9             write a status dump to openlab.log
//
// We don't expose stop() — the thread lives for the process lifetime and
// gets torn down by the OS at exit. Trying to stop it from DllMain would
// hit the loader lock and risk a deadlock.

namespace openlab::hotkeys {

// Start the hotkey thread once. Subsequent calls are no-ops.
bool start();

}  // namespace openlab::hotkeys
