#pragma once

#include <cstdint>

// User-customizable settings persisted to opendojo/config.json (next to
// the game's drills). All accessors are thread-safe via atomic loads.

namespace opendojo::config {

// Load config from disk into memory. Called once at DLL init. Safe to
// call before the data directory exists — missing file is treated as
// "use defaults".
void load();

// Persist current in-memory config back to opendojo/config.json.
// Called automatically after each setter mutates state.
void save();

// Toggle hotkey — Win32 virtual-key code (VK_F12 default). The render
// hook polls this each frame to decide whether to toggle the menu.
std::uint32_t toggle_vk();
void          set_toggle_vk(std::uint32_t vk);

// Hotkey rebind capture state. The Settings tab sets capturing=true,
// then the WndProc subclass observes WM_KEYDOWN and stores the next
// VK into `captured_vk`. Polling via GetAsyncKeyState would be broken
// by our keyboard-suppression hook, so capture goes through WndProc.
void          start_capture();
void          cancel_capture();
bool          is_capturing();
std::uint32_t consume_captured_vk();   // returns 0 if nothing pending
void          notify_captured_vk(std::uint32_t vk);   // called by WndProc

}  // namespace opendojo::config
