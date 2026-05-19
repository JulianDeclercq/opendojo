#include "render_hook.hpp"

#include <windows.h>
#include <windowsx.h>
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

#include "MinHook.h"

#include "autosave.hpp"
#include "config.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "rmlui_backend.hpp"

namespace opendojo::render_hook {

namespace {

using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ExecuteCommandListsFn = void (STDMETHODCALLTYPE*)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PresentFn             g_present_orig = nullptr;
ExecuteCommandListsFn g_exec_orig    = nullptr;

std::atomic<bool>          g_installing{false};
std::atomic<bool>          g_hooks_live{false};
std::atomic<bool>          g_rml_ready{false};
std::atomic<bool>          g_menu_visible{false};

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
ID3D12GraphicsCommandList*  g_cmd_list   = nullptr;
FrameContext                g_frames[MAX_FRAMES]{};
UINT                        g_buffer_count = 0;
D3D12_CPU_DESCRIPTOR_HANDLE g_rtv_cpu{};

ID3D12Fence*  g_fence       = nullptr;
HANDLE        g_fence_event = nullptr;
UINT64        g_fence_next  = 0;

// ===========================================================================
//  WndProc subclass — input routing into the RmlUi backend.
// ===========================================================================

WNDPROC g_orig_wndproc    = nullptr;
HWND    g_subclassed_hwnd = nullptr;

LRESULT CALLBACK opendojo_wndproc(HWND hwnd, UINT msg,
                                  WPARAM wparam, LPARAM lparam) {
    if (g_rml_ready.load()) {
        // ---- Toggle hotkey + capture handling (always-on, even with menu hidden)
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            const bool was_down = (lparam & (1LL << 30)) != 0;
            if (!was_down && opendojo::config::is_capturing()) {
                if (wparam == VK_ESCAPE) {
                    opendojo::config::cancel_capture();
                } else {
                    opendojo::config::notify_captured_vk(
                        static_cast<std::uint32_t>(wparam));
                }
                return 0;
            }
            const auto vk = opendojo::config::toggle_vk();
            if (wparam == vk) {
                if (!was_down) toggle_menu();
                return 0;
            }
        }

        if (g_menu_visible.load()) {
            // Hand the message to RmlUi. It returns true if it has
            // consumed the event; we then swallow it so the game's WndProc
            // never sees it.
            const bool is_mouse_msg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
            const bool is_kbd_msg   = (msg >= WM_KEYFIRST   && msg <= WM_KEYLAST);
            const bool is_text_msg  = (msg == WM_CHAR || msg == WM_UNICHAR);
            const bool is_raw_msg   = (msg == WM_INPUT
                                    || msg == WM_INPUT_DEVICE_CHANGE);

            if (is_mouse_msg || is_kbd_msg || is_text_msg) {
                opendojo::rml_backend::process_win32_message(msg, wparam, lparam);
            }
            if (is_mouse_msg || is_kbd_msg || is_text_msg || is_raw_msg) {
                return 1;  // swallow
            }
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
//  D3D12 init for our overlay (RTV heap, command list, fence, allocators).
// ===========================================================================

bool init_d3d12_overlay(IDXGISwapChain3* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc))) return false;

    g_buffer_count = desc.BufferCount;
    if (g_buffer_count == 0 || g_buffer_count > MAX_FRAMES) return false;
    if (!desc.OutputWindow) return false;
    g_rtv_format = desc.BufferDesc.Format;
    g_hwnd       = desc.OutputWindow;

    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_device)))) return false;

    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        d.NumDescriptors = 1;
        d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_rtv_heap))))
            return false;
    }
    g_rtv_cpu = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < g_buffer_count; ++i) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_frames[i].allocator))))
            return false;
    }

    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_frames[0].allocator, nullptr,
                                           IID_PPV_ARGS(&g_cmd_list))))
        return false;
    g_cmd_list->Close();

    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&g_fence))))
        return false;
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fence_event) return false;

    return true;
}

// Resolve assets directory: <game>\Polaris\Binaries\Win64\opendojo\ui
// Same parent folder as the drill files (commands::drills_dir()), so the
// whole mod lives under a single opendojo/ tree.
std::wstring resolve_assets_dir() {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
    return std::filesystem::path(exe).parent_path()
        .append(L"opendojo").append(L"ui").wstring();
}

bool init_rml(IDXGISwapChain3* swapchain) {
    auto assets = resolve_assets_dir();
    if (assets.empty()) {
        OPENDOJO_LOG("render_hook: could not resolve module path for assets");
        return false;
    }
    OPENDOJO_LOG("render_hook: rml assets dir = %ls", assets.c_str());
    return opendojo::rml_backend::init(
        g_device, g_queue, swapchain, g_hwnd,
        g_rtv_format, g_buffer_count, assets.c_str());
}

bool safe_init(IDXGISwapChain3* sc) {
    __try {
        return init_d3d12_overlay(sc) && init_rml(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("render_hook: SEH caught 0x%08X during init",
                     static_cast<unsigned>(GetExceptionCode()));
        return false;
    }
}

// ===========================================================================
//  XInput suppression + gamepad menu chord.
// ===========================================================================

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_xinput_orig    = nullptr;
XInputGetState_t g_xinput_ex_orig = nullptr;

DWORD WINAPI xinput_get_state_hook(DWORD user_index, XINPUT_STATE* state) {
    DWORD r = g_xinput_orig(user_index, state);
    if (g_menu_visible.load() && r == ERROR_SUCCESS && state) {
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    }
    return r;
}
DWORD WINAPI xinput_get_state_ex_hook(DWORD user_index, XINPUT_STATE* state) {
    DWORD r = g_xinput_ex_orig(user_index, state);
    if (g_menu_visible.load() && r == ERROR_SUCCESS && state) {
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    }
    return r;
}

XInputGetState_t install_one_xinput_hook(void* target, void* shim,
                                         XInputGetState_t* orig_slot,
                                         const wchar_t* dll_name,
                                         const char* label) {
    if (MH_CreateHook(target, shim, reinterpret_cast<LPVOID*>(orig_slot)) != MH_OK)
        return nullptr;
    if (MH_EnableHook(target) != MH_OK) return nullptr;
    OPENDOJO_LOG("render_hook: %s hooked on %ls", label, dll_name);
    return *orig_slot;
}

XInputGetState_t resolve_xinput_get_state() {
    static bool installed = false;
    if (installed) return g_xinput_orig;
    installed = true;

    const wchar_t* candidates[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput1_2.dll",
        L"xinput1_1.dll", L"xinput9_1_0.dll",
    };
    XInputGetState_t fallback = nullptr;
    int count = 0;
    for (const wchar_t* name : candidates) {
        HMODULE mod = GetModuleHandleW(name);
        if (!mod) mod = LoadLibraryW(name);
        if (!mod) continue;
        auto fn    = reinterpret_cast<XInputGetState_t>(GetProcAddress(mod, "XInputGetState"));
        auto fn_ex = reinterpret_cast<XInputGetState_t>(GetProcAddress(mod, MAKEINTRESOURCEA(100)));
        if (fn) {
            XInputGetState_t orig = nullptr;
            install_one_xinput_hook((void*)fn,
                                    (void*)&xinput_get_state_hook,
                                    &orig, name, "XInputGetState");
            if (orig) {
                if (!g_xinput_orig) g_xinput_orig = orig;
                if (!fallback)      fallback      = fn;
                ++count;
            }
        }
        if (fn_ex) {
            XInputGetState_t orig = nullptr;
            install_one_xinput_hook((void*)fn_ex,
                                    (void*)&xinput_get_state_ex_hook,
                                    &orig, name, "XInputGetStateEx");
            if (orig && !g_xinput_ex_orig) g_xinput_ex_orig = orig;
        }
    }
    if (count == 0) return nullptr;
    if (!g_xinput_orig) g_xinput_orig = fallback;
    return g_xinput_orig;
}

void poll_gamepad_nav() {
    auto get_state = resolve_xinput_get_state();
    if (!get_state) return;
    XINPUT_STATE state{};
    if (get_state(0, &state) != ERROR_SUCCESS) return;

    static WORD last_buttons = 0;
    const WORD curr = state.Gamepad.wButtons;
    const WORD pressed = curr & ~last_buttons;
    last_buttons = curr;

    using namespace opendojo::rml_backend;
    if (pressed & XINPUT_GAMEPAD_DPAD_UP)    nav_move(NavDir::Up);
    if (pressed & XINPUT_GAMEPAD_DPAD_DOWN)  nav_move(NavDir::Down);
    if (pressed & XINPUT_GAMEPAD_DPAD_LEFT)  nav_move(NavDir::Left);
    if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) nav_move(NavDir::Right);
    if (pressed & XINPUT_GAMEPAD_A)          nav_activate();
    if (pressed & XINPUT_GAMEPAD_B)          nav_back();
}

void poll_gamepad_toggle_chord() {
    auto get_state = resolve_xinput_get_state();
    if (!get_state) return;
    XINPUT_STATE state{};
    if (get_state(0, &state) != ERROR_SUCCESS) return;

    const bool chord = (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)
                    && (state.Gamepad.wButtons & XINPUT_GAMEPAD_START);
    static bool last = false;
    if (chord && !last) toggle_menu();
    last = chord;
}

// ===========================================================================
//  Keyboard input suppression — hook GetAsyncKeyState + GetKeyState.
// ===========================================================================

using GetAsyncKeyState_t = SHORT (WINAPI*)(int);
using GetKeyState_t      = SHORT (WINAPI*)(int);
GetAsyncKeyState_t g_gaks_orig = nullptr;
GetKeyState_t      g_gks_orig  = nullptr;

SHORT WINAPI gaks_hook(int vk) {
    if (g_menu_visible.load()) return 0;
    return g_gaks_orig(vk);
}
SHORT WINAPI gks_hook(int vk) {
    if (g_menu_visible.load()) return 0;
    return g_gks_orig(vk);
}

void install_keyboard_hooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    auto gaks = (void*)GetProcAddress(user32, "GetAsyncKeyState");
    auto gks  = (void*)GetProcAddress(user32, "GetKeyState");
    if (gaks
        && MH_CreateHook(gaks, (LPVOID)&gaks_hook, (LPVOID*)&g_gaks_orig) == MH_OK)
        MH_EnableHook(gaks);
    if (gks
        && MH_CreateHook(gks, (LPVOID)&gks_hook, (LPVOID*)&g_gks_orig) == MH_OK)
        MH_EnableHook(gks);
}

// ===========================================================================
//  Per-frame render.
// ===========================================================================

void render_frame() {
    opendojo::rml_backend::begin_frame();
    opendojo::menu::draw();

    const UINT idx = g_swapchain->GetCurrentBackBufferIndex();
    if (idx >= g_buffer_count) return;
    FrameContext& fc = g_frames[idx];
    if (!fc.allocator) return;

    ID3D12Resource* back_buffer = nullptr;
    if (FAILED(g_swapchain->GetBuffer(idx, IID_PPV_ARGS(&back_buffer))) || !back_buffer)
        return;
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

    DXGI_SWAP_CHAIN_DESC desc{};
    g_swapchain->GetDesc(&desc);
    opendojo::rml_backend::end_frame(g_cmd_list, g_rtv_cpu,
                                     desc.BufferDesc.Width,
                                     desc.BufferDesc.Height);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_cmd_list->ResourceBarrier(1, &barrier);
    g_cmd_list->Close();

    ID3D12CommandList* lists[] = { g_cmd_list };
    g_queue->ExecuteCommandLists(1, lists);

    fc.fence_value = ++g_fence_next;
    g_queue->Signal(g_fence, fc.fence_value);

    back_buffer->Release();
}

void safe_render_frame() {
    __try {
        render_frame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) {
            OPENDOJO_LOG("render_hook: SEH caught 0x%08X during render — overlay disabled",
                         static_cast<unsigned>(GetExceptionCode()));
            logged = true;
        }
        g_menu_visible.store(false);
        g_device = nullptr;
    }
}

// ===========================================================================
//  Hook bodies.
// ===========================================================================

void STDMETHODCALLTYPE hook_execute_command_lists(
        ID3D12CommandQueue* self, UINT num, ID3D12CommandList* const* lists) {
    if (!g_queue) {
        D3D12_COMMAND_QUEUE_DESC desc = self->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_queue = self;
            OPENDOJO_LOG("render_hook: captured command queue 0x%p", g_queue);
        }
    }
    g_exec_orig(self, num, lists);
}

HRESULT STDMETHODCALLTYPE hook_present(
        IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    if (!g_rml_ready.load()) {
        static std::atomic<unsigned> present_count{0};
        const unsigned n = present_count.fetch_add(1);
        constexpr unsigned WARMUP_FRAMES = 60;
        if (!g_queue || n < WARMUP_FRAMES) {
            return g_present_orig(self, sync_interval, flags);
        }

        static IDXGISwapChain* rejected[8] = {};
        static int rejected_count = 0;
        static unsigned attempts = 0;
        for (int i = 0; i < rejected_count; ++i) {
            if (rejected[i] == self)
                return g_present_orig(self, sync_interval, flags);
        }

        ++attempts;
        constexpr unsigned MAX_ATTEMPTS = 300;
        if (attempts > MAX_ATTEMPTS) {
            if (!g_rml_ready.exchange(true)) {
                OPENDOJO_LOG("render_hook: gave up after %u init attempts", attempts);
            }
            return g_present_orig(self, sync_interval, flags);
        }

        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(self->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            if (rejected_count < 8) rejected[rejected_count++] = self;
            return g_present_orig(self, sync_interval, flags);
        }
        ID3D12Device* probe_dev = nullptr;
        if (FAILED(sc3->GetDevice(IID_PPV_ARGS(&probe_dev)))) {
            sc3->Release();
            if (rejected_count < 8) rejected[rejected_count++] = self;
            return g_present_orig(self, sync_interval, flags);
        }
        probe_dev->Release();

        g_swapchain = sc3;
        const bool ok = safe_init(g_swapchain);
        if (ok) {
            g_rml_ready.store(true);
            install_message_hook(g_hwnd);
            install_keyboard_hooks();
            resolve_xinput_get_state();
            OPENDOJO_LOG("render_hook: RmlUi menu ready — press toggle key");
        } else {
            sc3->Release();
            g_swapchain = nullptr;
            if (rejected_count < 8) rejected[rejected_count++] = self;
        }
        return g_present_orig(self, sync_interval, flags);
    }

    poll_gamepad_toggle_chord();
    if (g_menu_visible.load()) poll_gamepad_nav();

    opendojo::autosave::tick();

    if (!g_menu_visible.load()
        || !g_device || !g_swapchain
        || self != static_cast<IDXGISwapChain*>(g_swapchain)) {
        return g_present_orig(self, sync_interval, flags);
    }

    safe_render_frame();
    return g_present_orig(self, sync_interval, flags);
}

// ===========================================================================
//  Installation.
// ===========================================================================

bool do_install() {
    HWND tmp_hwnd = CreateWindowExW(0, L"STATIC", L"opendojo_dummy",
                                    WS_POPUP, 0, 0, 100, 100,
                                    nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    if (!tmp_hwnd) return false;

    ID3D12Device*       dev     = nullptr;
    ID3D12CommandQueue* queue   = nullptr;
    IDXGIFactory4*      factory = nullptr;
    IDXGISwapChain1*    sc1     = nullptr;
    auto cleanup = [&] {
        if (sc1)     sc1->Release();
        if (factory) factory->Release();
        if (queue)   queue->Release();
        if (dev)     dev->Release();
        DestroyWindow(tmp_hwnd);
    };

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&dev)))) {
        cleanup();
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
        cleanup(); return false;
    }
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        cleanup(); return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.BufferCount = 2;
    scd.Width  = 100;
    scd.Height = 100;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    if (FAILED(factory->CreateSwapChainForHwnd(queue, tmp_hwnd, &scd,
                                               nullptr, nullptr, &sc1))) {
        cleanup(); return false;
    }

    void** swapchain_vt = *reinterpret_cast<void***>(sc1);
    void** queue_vt     = *reinterpret_cast<void***>(queue);
    void* present_addr = swapchain_vt[8];
    void* exec_addr    = queue_vt[10];

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        cleanup(); return false;
    }
    if (MH_CreateHook(present_addr, (LPVOID)&hook_present,
                      (LPVOID*)&g_present_orig) != MH_OK) {
        cleanup(); return false;
    }
    if (MH_CreateHook(exec_addr, (LPVOID)&hook_execute_command_lists,
                      (LPVOID*)&g_exec_orig) != MH_OK) {
        cleanup(); return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        cleanup(); return false;
    }

    cleanup();
    g_hooks_live.store(true);
    OPENDOJO_LOG("render_hook: MinHook detours installed");
    return true;
}

void install_worker() {
    for (int i = 0; i < 120; ++i) {
        if (GetModuleHandleW(L"d3d12.dll") && GetModuleHandleW(L"dxgi.dll")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!GetModuleHandleW(L"d3d12.dll")) {
        OPENDOJO_LOG("render_hook: d3d12.dll never loaded — menu disabled");
        return;
    }
    if (!do_install()) {
        OPENDOJO_LOG("render_hook: install failed");
    }
}

}  // anonymous namespace

void install() {
    bool expected = false;
    if (!g_installing.compare_exchange_strong(expected, true)) return;
    std::thread(install_worker).detach();
}

void toggle_menu() {
    const bool was = g_menu_visible.exchange(!g_menu_visible.load());
    const bool now = !was;
    OPENDOJO_LOG("render_hook: toggle_menu — %s -> %s",
                 was ? "VISIBLE" : "hidden",
                 now ? "VISIBLE" : "hidden");
    if (now) opendojo::menu::invalidate();
}

bool menu_visible() { return g_menu_visible.load(); }

}  // namespace opendojo::render_hook
