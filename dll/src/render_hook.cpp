#include "render_hook.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include "MinHook.h"

#include "log.hpp"
#include "menu.hpp"
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
//  WndProc hook
// ===========================================================================

// WndProc hook intentionally absent — see init_imgui_runtime. F12 toggle
// and mouse buttons are polled directly from inside hook_present each frame.

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

bool init_imgui_runtime() {
    OPENDOJO_LOG("render_hook: imgui step 1/4 — context + theme");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // Don't pollute the game folder.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

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

    // WndProc hook intentionally skipped. UE5 integrity-checks the window
    // procedure; replacing it appears to trigger a fatal. F12 toggling and
    // mouse-button events are polled inside hook_present each frame instead.
    OPENDOJO_LOG("render_hook: imgui step 4/4 — input via poll, no WndProc hook");

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

// Feed ImGui the mouse button + F12 state. Mouse position and modifier keys
// are already polled inside ImGui_ImplWin32_NewFrame; this fills in what's
// normally delivered via WM_*BUTTON* / WM_KEY*.
void poll_input_into_imgui() {
    ImGuiIO& io = ImGui::GetIO();
    static bool last_lmb = false;
    static bool last_rmb = false;
    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (lmb != last_lmb) { io.AddMouseButtonEvent(0, lmb); last_lmb = lmb; }
    if (rmb != last_rmb) { io.AddMouseButtonEvent(1, rmb); last_rmb = rmb; }
}

// Per-frame rendering. Pulled into its own function so we can wrap it in
// SEH without dragging C++ unwind frames through __try. Acquires a fresh
// back buffer reference for this frame and releases it before returning
// so the game's ResizeBuffers never sees outstanding refs from us.
void render_frame() {
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    poll_input_into_imgui();
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
        OPENDOJO_LOG("render_hook: beginning ImGui init at present #%u", n);

        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(self->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            OPENDOJO_LOG("render_hook: QueryInterface IDXGISwapChain3 failed "
                        "— need flip-model swapchain");
            g_imgui_ready.store(true);
            return g_present_orig(self, sync_interval, flags);
        }
        g_swapchain = sc3;

        const bool ok = safe_init(g_swapchain);
        g_imgui_ready.store(true);
        if (ok) {
            OPENDOJO_LOG("render_hook: menu ready — press F12 to toggle");
        } else {
            OPENDOJO_LOG("render_hook: init failed — menu disabled this session");
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
    g_menu_visible.store(!g_menu_visible.load());
    if (g_menu_visible.load()) opendojo::menu::invalidate();
}

bool menu_visible() { return g_menu_visible.load(); }

}  // namespace opendojo::render_hook
