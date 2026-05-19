#pragma once

// RmlUi DX12 + Win32 backend.
//
// One translation unit owns the RenderInterface, SystemInterface, and
// FileInterface implementations. The render hook calls init() once it's
// captured the game's D3D12 device + command queue + swapchain, then calls
// begin_frame()/end_frame() around each rendered frame and forwards Win32
// input events via process_*().
//
// All RmlUi state (context, document) is owned here too. The menu.cpp
// layer drives high-level interactions (section switching, drill list
// population) via rml::context() and data-binding helpers.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace Rml { class Context; class ElementDocument; }

namespace opendojo::rml_backend {

// One-time init. Returns false on any failure; caller should mark the menu
// disabled in that case (same fallback path as the previous ImGui init).
//
// `assets_dir` is the directory containing main.rml, main.rcss, font/*.ttf.
// Usually Mods/OpenDojo/ui under the game's Win64 folder.
bool init(ID3D12Device* device,
          ID3D12CommandQueue* queue,
          IDXGISwapChain3* swapchain,
          HWND hwnd,
          DXGI_FORMAT rtv_format,
          unsigned buffer_count,
          const wchar_t* assets_dir);

void shutdown();

// Per-frame entry points. begin_frame() updates layout + animations and
// processes any queued input; end_frame() records draw commands into the
// supplied command list. The caller is responsible for transitioning the
// back buffer to RENDER_TARGET and back, binding the RTV, and submitting
// the command list — identical contract to the previous ImGui backend.
void begin_frame();
void end_frame(ID3D12GraphicsCommandList* cmd_list,
               D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle,
               unsigned viewport_width,
               unsigned viewport_height);

// Notify the backend of swapchain resize. Recreates internal viewport-
// dependent state (none currently; Rml handles layout on its own).
void resize(unsigned width, unsigned height);

// Input plumbing. Mirrors the subset of Win32 messages we care about,
// translated to RmlUi's input model. Returns true if RmlUi consumed the
// event and the game's WndProc should NOT see it.
bool process_win32_message(UINT msg, WPARAM wparam, LPARAM lparam);

// Gamepad/keyboard navigation helpers used by render_hook for d-pad nav.
// Each call moves the focused element one step in the requested direction
// or activates the focused element. No-op when no document is loaded.
enum class NavDir { Up, Down, Left, Right };
void nav_move(NavDir dir);
void nav_activate();
void nav_back();

// Direct accessors so menu.cpp can build & rebuild the document content.
Rml::Context*         context();
Rml::ElementDocument* document();

}  // namespace opendojo::rml_backend
