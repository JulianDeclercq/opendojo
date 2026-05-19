#include "render_hook.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <xinput.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

// ImGui's Win32 backend intentionally hides the WndProc handler behind a
// `#if 0` block to keep windows.h out of imgui_impl_win32.h. Forward-
// declare it here so the WH_GETMESSAGE relay below can call it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "MinHook.h"

#include "autosave.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "practice_menu.hpp"
#include "theme.hpp"

namespace opendojo::render_hook {

namespace {

// ===========================================================================
//  Hook state
// ===========================================================================

// Function pointer types matching the COM vtable entries we're hijacking.
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ExecuteCommandListsFn = void (STDMETHODCALLTYPE*)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// Trampolines to the original engine functions, populated by the vtable
// swap. NEVER call the COM method directly inside a hook — that would
// recurse back into our hook.
PresentFn             g_present_orig = nullptr;
ExecuteCommandListsFn g_exec_orig    = nullptr;

std::atomic<bool>          g_installing{false};
std::atomic<bool>          g_hooks_live{false};
std::atomic<bool>          g_imgui_ready{false};
std::atomic<bool>          g_menu_visible{false};

// Captured from the live game. Queue is captured from the first call to
// our ExecuteCommandLists hook; device/swapchain from the first Present.
ID3D12CommandQueue* g_queue      = nullptr;
ID3D12Device*       g_device     = nullptr;
IDXGISwapChain3*    g_swapchain  = nullptr;
HWND                g_hwnd       = nullptr;
DXGI_FORMAT         g_rtv_format = DXGI_FORMAT_UNKNOWN;

constexpr UINT MAX_FRAMES = 8;

struct FrameContext {
    ID3D12CommandAllocator* allocator   = nullptr;
    UINT64                  fence_value = 0;
};

ID3D12DescriptorHeap*       g_rtv_heap   = nullptr;
ID3D12DescriptorHeap*       g_srv_heap   = nullptr;
ID3D12GraphicsCommandList*  g_cmd_list   = nullptr;
FrameContext                g_frames[MAX_FRAMES]{};
UINT                        g_buffer_count = 0;
// Single RTV descriptor slot, reused each frame. We acquire the back
// buffer fresh per Present (no held reference), bind an RTV to it in this
// slot, render, release the back buffer. The game retains exclusive
// ownership of its swapchain back buffers between our hook calls — so
// ResizeBuffers always sees zero outstanding refs from us, even though we
// don't hook ResizeBuffers itself.
D3D12_CPU_DESCRIPTOR_HANDLE g_rtv_cpu{};

// Single fence used to gate allocator reuse. Per-frame fence_value lets us
// tell whether a given allocator's previously-submitted work has finished
// on the GPU before we Reset it.
ID3D12Fence*  g_fence       = nullptr;
HANDLE        g_fence_event = nullptr;
UINT64        g_fence_next  = 0;

// ===========================================================================
//  Vtable utilities
// ===========================================================================

void* hook_vtable_entry(void** vtable, std::size_t index, void* new_fn) {
    DWORD old_protect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return nullptr;
    }
    void* original = vtable[index];
    vtable[index] = new_fn;
    DWORD tmp = 0;
    VirtualProtect(&vtable[index], sizeof(void*), old_protect, &tmp);
    return original;
}

// ===========================================================================
//  WndProc subclass (input routing)
// ===========================================================================
//
// Install a WndProc subclass via SetWindowLongPtrW. Every input message
// the game receives first runs through our proc; we forward to ImGui's
// backend handler, then check io.WantCaptureMouse / WantCaptureKeyboard
// to decide whether to swallow (return 1, message consumed) or let it
// through to the original WndProc.
//
// Why this works on UE5 despite the earlier "integrity check" worry:
// Irony ships this exact pattern on Tekken 8 (see Irony/src/sdk/os/
// window_procedure.zig). Tekken doesn't actually integrity-check its
// WndProc pointer.
//
// Why this beats WH_GETMESSAGE: that hook fires BEFORE TranslateMessage,
// so swallowing WM_KEYDOWN there prevents WM_CHAR from ever being
// generated — text input is broken. WndProc fires AFTER TranslateMessage,
// so both WM_KEYDOWN and the resulting WM_CHAR reach us independently
// and we can route both to ImGui without losing text input.

WNDPROC g_orig_wndproc = nullptr;
HWND    g_subclassed_hwnd = nullptr;

LRESULT CALLBACK opendojo_wndproc(HWND hwnd, UINT msg,
                                  WPARAM wparam, LPARAM lparam) {
    if (g_imgui_ready.load() && g_menu_visible.load()) {
        // Feed ImGui first. The backend handler does its own filtering
        // on which messages it cares about.
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

        ImGuiIO& io = ImGui::GetIO();
        const bool is_mouse_msg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
        const bool is_kbd_msg   = (msg >= WM_KEYFIRST   && msg <= WM_KEYLAST);
        if ((is_mouse_msg && io.WantCaptureMouse)
            || (is_kbd_msg && io.WantCaptureKeyboard)) {
            return 1;  // consumed — game's WndProc never sees it
        }
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wparam, lparam);
}

bool install_message_hook(HWND hwnd) {
    if (!hwnd) return false;
    SetLastError(0);
    auto prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(&opendojo_wndproc));
    if (prev == 0 && GetLastError() != 0) {
        OPENDOJO_LOG("render_hook: SetWindowLongPtrW(GWLP_WNDPROC) failed "
                    "(GLE=%lu) — input swallow disabled", GetLastError());
        return false;
    }
    g_orig_wndproc    = reinterpret_cast<WNDPROC>(prev);
    g_subclassed_hwnd = hwnd;
    OPENDOJO_LOG("render_hook: WndProc subclassed (hwnd=0x%p prev=0x%p)",
                hwnd, g_orig_wndproc);
    return true;
}

// ===========================================================================
//  ImGui DX12 init / teardown
// ===========================================================================

bool init_imgui_resources(IDXGISwapChain3* swapchain) {
    OPENDOJO_LOG("render_hook: init step 1/8 — GetDesc");
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc))) {
        OPENDOJO_LOG("render_hook: swapchain->GetDesc failed");
        return false;
    }
    g_buffer_count = desc.BufferCount;
    OPENDOJO_LOG("render_hook:  buffer_count=%u format=%u hwnd=0x%p w=%u h=%u",
                g_buffer_count,
                static_cast<unsigned>(desc.BufferDesc.Format),
                desc.OutputWindow,
                desc.BufferDesc.Width, desc.BufferDesc.Height);
    if (g_buffer_count == 0 || g_buffer_count > MAX_FRAMES) {
        OPENDOJO_LOG("render_hook: unsupported buffer count %u", g_buffer_count);
        return false;
    }
    if (!desc.OutputWindow) {
        OPENDOJO_LOG("render_hook: swapchain has NULL HWND — "
                    "non-windowed swapchain (CoreWindow / composition) unsupported");
        return false;
    }
    g_rtv_format = desc.BufferDesc.Format;
    g_hwnd       = desc.OutputWindow;

    OPENDOJO_LOG("render_hook: init step 2/8 — GetDevice");
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_device)))) {
        OPENDOJO_LOG("render_hook: swapchain->GetDevice failed");
        return false;
    }

    OPENDOJO_LOG("render_hook: init step 3/8 — RTV heap (single slot)");
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        d.NumDescriptors = 1;  // we reuse one slot each frame
        d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_rtv_heap)))) {
            OPENDOJO_LOG("render_hook: RTV heap creation failed");
            return false;
        }
    }
    g_rtv_cpu = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();

    OPENDOJO_LOG("render_hook: init step 4/8 — per-frame allocators");
    for (UINT i = 0; i < g_buffer_count; ++i) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_frames[i].allocator)))) {
            OPENDOJO_LOG("render_hook: CreateCommandAllocator(%u) failed", i);
            return false;
        }
    }

    OPENDOJO_LOG("render_hook: init step 5/8 — SRV heap");
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = 1;
        d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_srv_heap)))) {
            OPENDOJO_LOG("render_hook: SRV heap creation failed");
            return false;
        }
    }

    OPENDOJO_LOG("render_hook: init step 6/8 — command list");
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_frames[0].allocator, nullptr,
                                           IID_PPV_ARGS(&g_cmd_list)))) {
        OPENDOJO_LOG("render_hook: CreateCommandList failed");
        return false;
    }
    g_cmd_list->Close();

    OPENDOJO_LOG("render_hook: init step 7/8 — fence");
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        OPENDOJO_LOG("render_hook: CreateFence failed");
        return false;
    }
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fence_event) {
        OPENDOJO_LOG("render_hook: CreateEvent failed (GLE=%lu)", GetLastError());
        return false;
    }

    OPENDOJO_LOG("render_hook: init step 8/8 — D3D12 resources ready");
    return true;
}

// Try to load a custom font for the ImGui atlas. Looks for a TTF file in
// the mod's asset directory; if not found, falls back to ImGui's bundled
// ProggyClean. Mod path layout matches the Lua mod's:
//   <game>/Polaris/Binaries/Win64/Mods/OpenDojo/font/opendojo.ttf
//
// Workflow for the user: extract a Tekken UI font (e.g. via FModel or
// retoc) and drop it at that path. See docs/STYLING.md.
void try_load_custom_font(ImGuiIO& io) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    auto font_path = std::filesystem::path(exe).parent_path()
        .append(L"Mods").append(L"OpenDojo").append(L"font")
        .append(L"opendojo.ttf");

    if (!std::filesystem::exists(font_path)) {
        OPENDOJO_LOG("render_hook: no custom font at %ls — using ImGui default",
                    font_path.c_str());
        return;
    }

    // Width depends on display DPI; 18px reads well on 1080p+. ImGui
    // accepts a UTF-8 path so convert the wide string here.
    auto path_str = font_path.string();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = true;
    ImFont* fnt = io.Fonts->AddFontFromFileTTF(
        path_str.c_str(), 18.0f, &cfg,
        io.Fonts->GetGlyphRangesDefault());
    if (fnt) {
        OPENDOJO_LOG("render_hook: loaded custom font %s", path_str.c_str());
    } else {
        OPENDOJO_LOG("render_hook: AddFontFromFileTTF failed for %s — "
                    "falling back to ImGui default", path_str.c_str());
    }
}

bool init_imgui_runtime() {
    OPENDOJO_LOG("render_hook: imgui step 1/4 — context + theme");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // Don't pollute the game folder.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // BackendFlags::HasGamepad lets ImGui's nav code know the gamepad
    // events we're feeding via AddKeyEvent / AddKeyAnalogEvent are real.
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    try_load_custom_font(io);

    opendojo::theme::apply();

    OPENDOJO_LOG("render_hook: imgui step 2/4 — Win32 backend (hwnd=0x%p)", g_hwnd);
    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        OPENDOJO_LOG("render_hook: ImGui_ImplWin32_Init failed");
        return false;
    }

    OPENDOJO_LOG("render_hook: imgui step 3/4 — DX12 backend");
    if (!ImGui_ImplDX12_Init(
            g_device,
            static_cast<int>(g_buffer_count),
            g_rtv_format,
            g_srv_heap,
            g_srv_heap->GetCPUDescriptorHandleForHeapStart(),
            g_srv_heap->GetGPUDescriptorHandleForHeapStart())) {
        OPENDOJO_LOG("render_hook: ImGui_ImplDX12_Init failed");
        return false;
    }

    OPENDOJO_LOG("render_hook: imgui step 4/4 — installing WH_GETMESSAGE hook "
                "for keyboard input");
    // Non-fatal: if this fails, mouse + F12 + gamepad still work, only
    // text-input (the Export tab's name/description fields) is broken.
    install_message_hook(g_hwnd);

    OPENDOJO_LOG("render_hook: ImGui initialized (hwnd=0x%p, buffers=%u, fmt=%u)",
                g_hwnd, g_buffer_count, static_cast<unsigned>(g_rtv_format));
    return true;
}

// Wraps the init pipeline in Windows SEH so an access violation in ImGui or
// the D3D12 runtime doesn't take the game down. With /EHsc (our default),
// __try/__except catches structured exceptions but not C++ exceptions —
// which is what we want here since we're guarding against AV-style crashes
// originating in third-party code.
bool safe_init(IDXGISwapChain3* sc) {
    __try {
        return init_imgui_resources(sc) && init_imgui_runtime();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("render_hook: SEH caught exception 0x%08X during init "
                    "— menu disabled this session",
                    static_cast<unsigned>(GetExceptionCode()));
        return false;
    }
}

// Lua mod IPC. Path to the flag file the Lua mod touches when the user
// clicks the inserted "OpenDojo" row in the practice pause menu.
// Cached on first use.
const wchar_t* lua_menu_flag_path() {
    static std::wstring cached;
    if (cached.empty()) {
        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            cached = std::filesystem::path(exe).parent_path()
                         .append(L"opendojo_open_menu.flag").wstring();
        }
    }
    return cached.c_str();
}

void poll_lua_menu_flag() {
    const wchar_t* path = lua_menu_flag_path();
    if (!path || !*path) return;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(path);
    g_menu_visible.store(!g_menu_visible.load());
    if (g_menu_visible.load()) opendojo::menu::invalidate();
    OPENDOJO_LOG("render_hook: menu toggled via Lua IPC flag");
}

// XInput is dynamic-loaded so we don't add a hard dependency on a
// specific xinput*.dll version. xinput1_4 ships with Windows 8+; we
// fall back to 1_3 / 9_1_0 for paranoia.
//
// We MinHook XInputGetState so that while the menu is visible the game
// sees an idle controller — otherwise every DPad press also feeds the
// in-game UI. Our own polls call the trampoline directly so they always
// see real hardware state.
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetState_t g_xinput_orig = nullptr;  // trampoline (real impl)
void*            g_xinput_addr = nullptr;  // hooked address

DWORD WINAPI xinput_get_state_hook(DWORD user_index, XINPUT_STATE* state) {
    DWORD r = g_xinput_orig(user_index, state);
    if (g_menu_visible.load() && r == ERROR_SUCCESS && state) {
        // Mask state to caller. We keep dwPacketNumber non-zero so the
        // game doesn't infer "controller just unplugged"; just zero the
        // gamepad fields so all buttons read released and sticks centered.
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    }
    return r;
}

XInputGetState_t resolve_xinput_get_state() {
    static bool installed = false;
    if (installed) return g_xinput_orig;
    installed = true;

    const wchar_t* candidates[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"
    };
    for (const wchar_t* name : candidates) {
        HMODULE mod = GetModuleHandleW(name);
        if (!mod) mod = LoadLibraryW(name);
        if (!mod) continue;
        auto fn = reinterpret_cast<XInputGetState_t>(
            GetProcAddress(mod, "XInputGetState"));
        if (!fn) continue;
        g_xinput_addr = reinterpret_cast<void*>(fn);

        // MinHook is initialized by do_install() before this point.
        MH_STATUS s = MH_CreateHook(
            g_xinput_addr,
            reinterpret_cast<LPVOID>(&xinput_get_state_hook),
            reinterpret_cast<LPVOID*>(&g_xinput_orig));
        if (s == MH_OK && MH_EnableHook(g_xinput_addr) == MH_OK) {
            OPENDOJO_LOG("render_hook: XInputGetState hooked from %ls "
                        "(orig=0x%p, hook=0x%p)",
                        name, g_xinput_orig, &xinput_get_state_hook);
            return g_xinput_orig;
        }
        // Hook install failed — fall back to direct call (no suppression).
        OPENDOJO_LOG("render_hook: MH_CreateHook(XInputGetState) failed: %d "
                    "— using direct pointer, no input suppression", s);
        g_xinput_orig = fn;
        return g_xinput_orig;
    }
    OPENDOJO_LOG("render_hook: XInputGetState unavailable — gamepad nav disabled");
    return nullptr;
}

// Poll the first connected XInput controller and feed its state into
// ImGui as virtual key + analog events. Cheap when no controller is
// connected (XInputGetState returns ERROR_DEVICE_NOT_CONNECTED).
//
// Tekken normally consumes gamepad input directly via XInput too, so
// while the menu is open the player sees the menu respond AND the game
// might also see button presses. We don't suppress at the XInput layer
// because Tekken pauses the match while the practice pause menu is up
// (which is where the user opens OpenDojo from), so input leak is mild.
// Revisit if we later allow opening the menu mid-match.
//
// Also handles the Back+Start chord as a gamepad-side "toggle menu" so
// pure-gamepad users can close without alt-tabbing or pressing F12.
void poll_gamepad_into_imgui() {
    auto get_state = resolve_xinput_get_state();
    if (!get_state) return;

    XINPUT_STATE state{};
    if (get_state(0, &state) != ERROR_SUCCESS) {
        // No controller. Make sure ImGui's nav state doesn't think a
        // gamepad is present (otherwise stale presses persist).
        return;
    }

    const XINPUT_GAMEPAD& gp = state.Gamepad;
    ImGuiIO& io = ImGui::GetIO();

    // Edge-detect button events. ImGui's gamepad nav has hardcoded
    // repeat (0.32s delay → 0.08s rate) once a key is "held", which
    // makes a casual DPad press jump multiple tabs. By only emitting
    // events on rising/falling edges, ImGui never sees a held state
    // for the digital buttons — each press is exactly one nav move.
    static WORD last_buttons = 0;
    const WORD curr_buttons = gp.wButtons;
    const WORD pressed_mask  = curr_buttons & ~last_buttons;
    const WORD released_mask = ~curr_buttons & last_buttons;
    last_buttons = curr_buttons;

    auto edge = [&](WORD mask, ImGuiKey key) {
        if (pressed_mask  & mask) io.AddKeyEvent(key, true);
        if (released_mask & mask) io.AddKeyEvent(key, false);
    };

    // Standard Xbox-layout mapping (A=down, B=right, X=left, Y=up).
    edge(XINPUT_GAMEPAD_A,              ImGuiKey_GamepadFaceDown);
    edge(XINPUT_GAMEPAD_B,              ImGuiKey_GamepadFaceRight);
    edge(XINPUT_GAMEPAD_X,              ImGuiKey_GamepadFaceLeft);
    edge(XINPUT_GAMEPAD_Y,              ImGuiKey_GamepadFaceUp);
    edge(XINPUT_GAMEPAD_DPAD_LEFT,      ImGuiKey_GamepadDpadLeft);
    edge(XINPUT_GAMEPAD_DPAD_RIGHT,     ImGuiKey_GamepadDpadRight);
    edge(XINPUT_GAMEPAD_DPAD_UP,        ImGuiKey_GamepadDpadUp);
    edge(XINPUT_GAMEPAD_DPAD_DOWN,      ImGuiKey_GamepadDpadDown);
    edge(XINPUT_GAMEPAD_LEFT_SHOULDER,  ImGuiKey_GamepadL1);
    edge(XINPUT_GAMEPAD_RIGHT_SHOULDER, ImGuiKey_GamepadR1);
    edge(XINPUT_GAMEPAD_LEFT_THUMB,     ImGuiKey_GamepadL3);
    edge(XINPUT_GAMEPAD_RIGHT_THUMB,    ImGuiKey_GamepadR3);
    edge(XINPUT_GAMEPAD_START,          ImGuiKey_GamepadStart);
    edge(XINPUT_GAMEPAD_BACK,           ImGuiKey_GamepadBack);

    // Triggers as analog L2/R2. ImGui expects [0,1].
    const float lt = static_cast<float>(gp.bLeftTrigger)  / 255.0f;
    const float rt = static_cast<float>(gp.bRightTrigger) / 255.0f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, lt > 0.10f, lt);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, rt > 0.10f, rt);

    // Left stick as analog nav. Apply a deadzone roughly matching the
    // recommended XInput threshold so resting drift doesn't navigate.
    constexpr float kStickDz = 8689.0f / 32767.0f;
    auto stick = [&](SHORT raw, ImGuiKey neg, ImGuiKey pos) {
        const float v = static_cast<float>(raw) / 32767.0f;
        const float a = v < 0 ? -v : v;
        const float n = a > kStickDz
            ? (a - kStickDz) / (1.0f - kStickDz) : 0.0f;
        io.AddKeyAnalogEvent(v < 0 ? neg : pos, n > 0.10f, n);
        // Make sure the opposite-axis key clears when the stick crosses zero.
        io.AddKeyAnalogEvent(v < 0 ? pos : neg, false, 0.0f);
    };
    stick(gp.sThumbLX, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight);
    stick(gp.sThumbLY, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp);
    stick(gp.sThumbRX, ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight);
    stick(gp.sThumbRY, ImGuiKey_GamepadRStickDown, ImGuiKey_GamepadRStickUp);
}

// Gamepad-side toggle chord: Back + Start held simultaneously. Polled
// outside of poll_gamepad_into_imgui because the chord should fire even
// when the menu is hidden (so the user can open it). Edge-triggered.
void poll_gamepad_toggle_chord() {
    auto get_state = resolve_xinput_get_state();
    if (!get_state) return;

    XINPUT_STATE state{};
    if (get_state(0, &state) != ERROR_SUCCESS) return;

    const bool chord = (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0
                    && (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;
    static bool last_chord = false;
    if (chord && !last_chord) {
        g_menu_visible.store(!g_menu_visible.load());
        if (g_menu_visible.load()) opendojo::menu::invalidate();
        OPENDOJO_LOG("render_hook: menu toggled via gamepad Back+Start");
    }
    last_chord = chord;
}

// Per-frame rendering. Pulled into its own function so we can wrap it in
// SEH without dragging C++ unwind frames through __try. Acquires a fresh
// back buffer reference for this frame and releases it before returning
// so the game's ResizeBuffers never sees outstanding refs from us.
void render_frame() {
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    poll_gamepad_into_imgui();
    ImGui::NewFrame();
    opendojo::menu::draw();
    ImGui::Render();

    const UINT idx = g_swapchain->GetCurrentBackBufferIndex();
    if (idx >= g_buffer_count) { ImGui::EndFrame(); return; }
    FrameContext& fc = g_frames[idx];
    if (!fc.allocator) return;

    ID3D12Resource* back_buffer = nullptr;
    if (FAILED(g_swapchain->GetBuffer(idx, IID_PPV_ARGS(&back_buffer))) || !back_buffer) {
        return;
    }
    g_device->CreateRenderTargetView(back_buffer, nullptr, g_rtv_cpu);

    if (fc.fence_value > 0 && g_fence->GetCompletedValue() < fc.fence_value) {
        g_fence->SetEventOnCompletion(fc.fence_value, g_fence_event);
        WaitForSingleObject(g_fence_event, INFINITE);
    }
    fc.allocator->Reset();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = back_buffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;

    g_cmd_list->Reset(fc.allocator, nullptr);
    g_cmd_list->ResourceBarrier(1, &barrier);
    g_cmd_list->OMSetRenderTargets(1, &g_rtv_cpu, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g_srv_heap };
    g_cmd_list->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd_list);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_cmd_list->ResourceBarrier(1, &barrier);
    g_cmd_list->Close();

    ID3D12CommandList* lists[] = { g_cmd_list };
    g_queue->ExecuteCommandLists(1, lists);

    fc.fence_value = ++g_fence_next;
    g_queue->Signal(g_fence, fc.fence_value);

    // CRITICAL: release the back buffer ref before the next ResizeBuffers
    // sees us. The submitted command list still references the resource
    // via the GPU, but D3D12 tracks that internally — CPU-side Release
    // just drops our refcount.
    back_buffer->Release();
}

void safe_render_frame() {
    __try {
        render_frame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Don't spam the log — a crash here on every frame would flood
        // the file. Disable rendering for the rest of the session.
        static bool logged = false;
        if (!logged) {
            OPENDOJO_LOG("render_hook: SEH caught exception 0x%08X during "
                        "per-frame render — overlay disabled",
                        static_cast<unsigned>(GetExceptionCode()));
            logged = true;
        }
        g_menu_visible.store(false);
        g_device = nullptr;  // forces fast-path passthrough from now on
    }
}

// ===========================================================================
//  Hook bodies
// ===========================================================================

void STDMETHODCALLTYPE hook_execute_command_lists(
        ID3D12CommandQueue* self, UINT num, ID3D12CommandList* const* lists) {
    if (!g_queue) {
        D3D12_COMMAND_QUEUE_DESC desc = self->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            // No AddRef — we hold the pointer for use during render but
            // don't extend its lifetime. The game's main graphics queue
            // outlives every meaningful interaction the menu has with it.
            g_queue = self;
            OPENDOJO_LOG("render_hook: captured command queue 0x%p", g_queue);
        }
    }
    g_exec_orig(self, num, lists);
}

HRESULT STDMETHODCALLTYPE hook_present(
        IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    // First-call init. Wait until exec hook has captured the queue AND
    // we've let the game's renderer settle for a few frames before we
    // start poking at descriptors.
    //
    // Processes can have multiple swapchains (game main + overlays
    // from NVIDIA/Discord/OBS/Game Bar). The hook fires for whichever
    // one Presents on a given frame. We retry across swapchains until
    // we find one that's actually D3D12-backed — bailing on the first
    // failure was the previous bug ("init failed — menu disabled").
    if (!g_imgui_ready.load()) {
        static std::atomic<unsigned> present_count{0};
        const unsigned n = present_count.fetch_add(1);
        if (n < 6) {
            OPENDOJO_LOG("render_hook: hook_present #%u (queue=0x%p)",
                        n, g_queue);
        }
        constexpr unsigned WARMUP_FRAMES = 60;
        if (!g_queue || n < WARMUP_FRAMES) {
            return g_present_orig(self, sync_interval, flags);
        }

        // Reject set: swapchains we've already tried that don't work.
        // Static so it persists across calls without owning anything.
        // Cap size at a few entries (overlays don't churn pointers).
        static IDXGISwapChain* rejected[8] = {};
        static int rejected_count = 0;
        static unsigned attempts = 0;
        static unsigned first_attempt_present = 0;

        for (int i = 0; i < rejected_count; ++i) {
            if (rejected[i] == self) {
                return g_present_orig(self, sync_interval, flags);
            }
        }

        if (attempts == 0) first_attempt_present = n;
        ++attempts;

        constexpr unsigned MAX_ATTEMPTS = 300;
        if (attempts > MAX_ATTEMPTS) {
            // Couldn't find a working D3D12 swapchain after ~5 seconds
            // of trying. Stop spamming the log; menu's done for this run.
            if (!g_imgui_ready.exchange(true)) {
                OPENDOJO_LOG("render_hook: gave up after %u init attempts "
                            "— no D3D12 swapchain found", attempts);
            }
            return g_present_orig(self, sync_interval, flags);
        }

        OPENDOJO_LOG("render_hook: init attempt #%u on swapchain 0x%p "
                    "(present #%u, %u frames since first attempt)",
                    attempts, self, n, n - first_attempt_present);

        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(self->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            OPENDOJO_LOG("  rejected: not IDXGISwapChain3 "
                        "(non-flip-model or wrapped)");
            if (rejected_count < 8) rejected[rejected_count++] = self;
            return g_present_orig(self, sync_interval, flags);
        }

        // Pre-flight: does this swapchain have an actual D3D12 device?
        // If not, init_imgui_resources's GetDevice will fail. Reject
        // here before allocating anything else.
        ID3D12Device* probe_dev = nullptr;
        if (FAILED(sc3->GetDevice(IID_PPV_ARGS(&probe_dev)))) {
            OPENDOJO_LOG("  rejected: GetDevice(ID3D12Device) failed "
                        "— swapchain isn't D3D12-backed");
            sc3->Release();
            if (rejected_count < 8) rejected[rejected_count++] = self;
            return g_present_orig(self, sync_interval, flags);
        }
        probe_dev->Release();   // init_imgui_resources will GetDevice again

        g_swapchain = sc3;
        const bool ok = safe_init(g_swapchain);
        if (ok) {
            g_imgui_ready.store(true);
            OPENDOJO_LOG("render_hook: menu ready on swapchain 0x%p "
                        "after %u attempts — press F12 to toggle",
                        self, attempts);
        } else {
            OPENDOJO_LOG("  rejected: init_imgui_resources failed mid-way");
            sc3->Release();
            g_swapchain = nullptr;
            if (rejected_count < 8) rejected[rejected_count++] = self;
        }
        return g_present_orig(self, sync_interval, flags);
    }

    // F12 toggle is polled every frame (no WndProc hook).
    {
        static bool last_f12 = false;
        const bool curr = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (curr && !last_f12) {
            g_menu_visible.store(!g_menu_visible.load());
            if (g_menu_visible.load()) opendojo::menu::invalidate();
        }
        last_f12 = curr;
    }

    // Practice-menu integration (DLL-native). Auto-inserts the
    // "OpenDojo" row, applies SetRawText each frame to bypass
    // Gryphon, and ProcessEvent-hooks OnDecideButton1 to toggle
    // ImGui on row click. Cheap when practice menu not live.
    opendojo::practice_menu::tick();

    // Legacy Lua-mod IPC fallback. Kept in parallel for this
    // iteration in case the DLL-native path hits an offset/resolve
    // edge case. Remove once practice_menu::tick is confirmed
    // working end-to-end.
    poll_lua_menu_flag();

    // Gamepad chord (Back+Start) toggles the menu — gamepad-only users
    // need a way to dismiss without F12.
    poll_gamepad_toggle_chord();

    // Autosave runs every frame regardless of menu visibility — it watches
    // for character/scene transitions and snapshots pool1 accordingly.
    opendojo::autosave::tick();

    if (!g_menu_visible.load()) {
        return g_present_orig(self, sync_interval, flags);
    }
    if (!g_device || !g_swapchain) {
        return g_present_orig(self, sync_interval, flags);
    }
    if (self != static_cast<IDXGISwapChain*>(g_swapchain)) {
        return g_present_orig(self, sync_interval, flags);
    }

    safe_render_frame();
    return g_present_orig(self, sync_interval, flags);
}

// ===========================================================================
//  Installation: create dummies, grab vtables, swap entries
// ===========================================================================

bool do_install() {
    HWND tmp_hwnd = CreateWindowExW(0, L"STATIC", L"opendojo_dummy",
                                    WS_POPUP, 0, 0, 100, 100,
                                    nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    if (!tmp_hwnd) {
        OPENDOJO_LOG("render_hook: tmp hwnd creation failed (GLE=%lu)", GetLastError());
        return false;
    }

    ID3D12Device*       dev    = nullptr;
    ID3D12CommandQueue* queue  = nullptr;
    IDXGIFactory4*      factory = nullptr;
    IDXGISwapChain1*    sc1    = nullptr;
    auto cleanup = [&] {
        if (sc1)     sc1->Release();
        if (factory) factory->Release();
        if (queue)   queue->Release();
        if (dev)     dev->Release();
        DestroyWindow(tmp_hwnd);
    };

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        OPENDOJO_LOG("render_hook: D3D12CreateDevice failed — game may be D3D11");
        cleanup();
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
        OPENDOJO_LOG("render_hook: CreateCommandQueue (dummy) failed");
        cleanup();
        return false;
    }

    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        OPENDOJO_LOG("render_hook: CreateDXGIFactory1 failed");
        cleanup();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.BufferCount     = 2;
    scd.Width           = 100;
    scd.Height          = 100;
    scd.Format          = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage     = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect      = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    scd.AlphaMode       = DXGI_ALPHA_MODE_UNSPECIFIED;
    if (FAILED(factory->CreateSwapChainForHwnd(queue, tmp_hwnd, &scd,
                                               nullptr, nullptr, &sc1))) {
        OPENDOJO_LOG("render_hook: CreateSwapChainForHwnd (dummy) failed");
        cleanup();
        return false;
    }

    // COM vtables are class-shared in this process: swapping vtable[N] on
    // the dummy object hooks every IDXGISwapChain / ID3D12CommandQueue
    // instance in this process — including the ones the game already created.
    void** swapchain_vt = *reinterpret_cast<void***>(sc1);
    void** queue_vt     = *reinterpret_cast<void***>(queue);

    void* present_addr = swapchain_vt[8];   // IDXGISwapChain::Present
    void* exec_addr    = queue_vt[10];      // ID3D12CommandQueue::ExecuteCommandLists

    // MinHook detour: patches the function prologue with a JMP rather
    // than touching the COM vtable. Tekken's anti-tamper scans vtables
    // (confirmed via diagnostic build that just patched vtable[8] with
    // an empty body — game crashed after ~6k frames). The dummy
    // swapchain/queue still serves as a vehicle for resolving the
    // function addresses; once we have those, we don't need the dummy
    // anymore.
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        OPENDOJO_LOG("render_hook: MH_Initialize failed");
        cleanup();
        return false;
    }

    MH_STATUS s = MH_CreateHook(present_addr,
                                reinterpret_cast<LPVOID>(&hook_present),
                                reinterpret_cast<LPVOID*>(&g_present_orig));
    if (s != MH_OK) {
        OPENDOJO_LOG("render_hook: MH_CreateHook(Present) failed: %d", s);
        cleanup();
        return false;
    }
    s = MH_CreateHook(exec_addr,
                      reinterpret_cast<LPVOID>(&hook_execute_command_lists),
                      reinterpret_cast<LPVOID*>(&g_exec_orig));
    if (s != MH_OK) {
        OPENDOJO_LOG("render_hook: MH_CreateHook(ExecuteCommandLists) failed: %d", s);
        cleanup();
        return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        OPENDOJO_LOG("render_hook: MH_EnableHook(MH_ALL_HOOKS) failed");
        cleanup();
        return false;
    }

    cleanup();

    g_hooks_live.store(true);
    OPENDOJO_LOG("render_hook: MinHook detours installed (present=0x%p, exec=0x%p)",
                present_addr, exec_addr);
    return true;
}

void install_worker() {
    // Wait for the game's D3D12 / DXGI runtime DLLs to be loaded. They're
    // usually already there by the time our DllMain runs, but be patient
    // since we're racing the game's startup.
    for (int i = 0; i < 120; ++i) {  // up to 60s
        if (GetModuleHandleW(L"d3d12.dll") && GetModuleHandleW(L"dxgi.dll")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!GetModuleHandleW(L"d3d12.dll")) {
        OPENDOJO_LOG("render_hook: d3d12.dll never loaded — menu disabled "
                    "(game is probably D3D11)");
        return;
    }

    if (!do_install()) {
        OPENDOJO_LOG("render_hook: install failed — menu disabled this session");
    }
}

}  // anonymous namespace

// ===========================================================================
//  Public API
// ===========================================================================

void install() {
    bool expected = false;
    if (!g_installing.compare_exchange_strong(expected, true)) return;
    std::thread(install_worker).detach();
}

void toggle_menu() {
    // fetch_xor on an atomic<bool> via int proxy. The previous
    // load/!/store pattern had a theoretical race when the click
    // hook (game thread) and F12 poll (render thread) raced. This
    // is a single atomic RMW.
    const bool was_visible = g_menu_visible.exchange(!g_menu_visible.load());
    const bool now_visible = !was_visible;
    OPENDOJO_LOG("render_hook: toggle_menu — %s -> %s",
                 was_visible ? "VISIBLE" : "hidden",
                 now_visible ? "VISIBLE" : "hidden");
    if (now_visible) opendojo::menu::invalidate();
}

bool menu_visible() { return g_menu_visible.load(); }

}  // namespace opendojo::render_hook
