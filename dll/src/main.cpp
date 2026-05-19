// OpenDojo DLL entry point. Lives at <game>\Polaris\Binaries\Win64\dinput8.dll
// (proxy name controlled by CMake's OPENDOJO_PROXY option). When the game
// loads what it thinks is the system dinput8.dll, this DllMain runs first.
//
// Responsibilities:
//   1. Resolve and pin the real dinput8.dll so our forwarded exports work.
//   2. Open the log file.
//   3. Spawn the OpenDojo init thread (off the loader lock).
//
// The init thread:
//   - Logs module base and subsystem-resolution sanity, then
//   - Kicks off the render hook (which itself spawns another thread that
//     waits for the game's D3D12 runtime to be loaded before patching
//     vtables and standing up the ImGui overlay).

#include <windows.h>

#include <filesystem>
#include <string>
#include <thread>

#include "config.hpp"
#include "log.hpp"
#include "memory.hpp"
#include "proxy.hpp"
#include "render_hook.hpp"
#include "subsystems.hpp"

namespace {

void init_thread() {
    OPENDOJO_LOG("OpenDojo v0.1.0 starting up");

    auto base = opendojo::memory::polaris_base();
    if (!base) {
        OPENDOJO_LOG("WARNING: Polaris-Win64-Shipping.exe not loaded — "
                    "DLL was injected into the wrong process");
        return;
    }
    OPENDOJO_LOG("polaris_base = 0x%llX", static_cast<unsigned long long>(base));

    // pool1 is lazy — null until the user records once per game launch.
    auto p1 = opendojo::subsystems::pool1();
    OPENDOJO_LOG("pool1 = 0x%llX (%s)",
                static_cast<unsigned long long>(p1),
                p1 ? "ready" : "not allocated yet — record once in practice mode");

    // Load persistent settings (hotkey binding etc.). Defaults are
    // applied if no config.json exists yet.
    opendojo::config::load();

    // Renderer hook drives the menu. Skipped if a file named
    // "opendojo_no_menu" exists next to the DLL — used to diagnose whether
    // the hook itself is destabilizing the game.
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring marker = std::filesystem::path(exe_path).parent_path()
                              .append(L"opendojo_no_menu").wstring();
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
        OPENDOJO_LOG("opendojo_no_menu marker found at %ls — render hook NOT installed",
                    marker.c_str());
    } else {
        opendojo::render_hook::install();
    }

    OPENDOJO_LOG("init thread done — press F12 in game to open menu (if enabled)");
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);

            opendojo::log::init();

            if (!opendojo::proxy::load()) {
                OPENDOJO_LOG("proxy::load() failed — refusing to attach");
                opendojo::log::shutdown();
                return FALSE;
            }

            std::thread(init_thread).detach();
            break;
        }
        case DLL_PROCESS_DETACH:
            opendojo::log::shutdown();
            opendojo::proxy::unload();
            break;
    }
    return TRUE;
}
