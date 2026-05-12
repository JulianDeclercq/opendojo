// OpenDojo DLL entry point. Lives at <game>\Polaris\Binaries\Win64\dinput8.dll
// (proxy name controlled by CMake's OPENDOJO_PROXY option). When the game
// loads what it thinks is the system dinput8.dll, this DllMain runs first.
//
// Responsibilities:
//   1. Resolve and pin the real dinput8.dll so our forwarded exports work.
//   2. Open the log file.
//   3. Spawn the OpenDojo init thread (so we don't block the loader — DllMain
//      runs under the loader lock and can't do meaningful work synchronously).
//
// Actual recording-buffer access, hotkeys, drill I/O — none of that has been
// ported yet. This DllMain just proves the load path is wired up.

#include <windows.h>

#include <chrono>
#include <thread>

#include "drill.hpp"
#include "hotkeys.hpp"
#include "log.hpp"
#include "memory.hpp"
#include "proxy.hpp"
#include "slot.hpp"
#include "subsystems.hpp"

namespace {

// Background thread: anything that can't or shouldn't run under the loader
// lock goes here. Keep DllMain itself trivial.
void init_thread() {
    OPENDOJO_LOG("OpenDojo v0.1.0 starting up");

    // The game's main module. Once we port the CE Lua, this is what we add
    // module offsets to in order to reach pool1 and the service-locator.
    auto base = opendojo::memory::polaris_base();
    if (!base) {
        OPENDOJO_LOG("WARNING: Polaris-Win64-Shipping.exe not loaded — "
                    "DLL was injected into the wrong process");
        return;
    }
    OPENDOJO_LOG("polaris_base = 0x%llX", static_cast<unsigned long long>(base));

    // Pool1 is lazy — null until the user records once per game launch.
    auto p1 = opendojo::subsystems::pool1();
    OPENDOJO_LOG("pool1 = 0x%llX (%s)",
                static_cast<unsigned long long>(p1),
                p1 ? "ready" : "not allocated yet — record once in practice mode");

    // Subsystems: try to resolve all five we care about. Most are alive by
    // the time DLL_PROCESS_ATTACH fires; gameplay sometimes lags. Logging
    // the misses tells us if we need a deferred resolver.
    struct entry { const char* name; std::uintptr_t key; };
    const entry probes[] = {
        { "gameplay",  opendojo::subsystems::KEY_GAMEPLAY  },
        { "singleton", opendojo::subsystems::KEY_SINGLETON },
        { "subB",      opendojo::subsystems::KEY_SUBB      },
        { "subC",      opendojo::subsystems::KEY_SUBC      },
        { "subD",      opendojo::subsystems::KEY_SUBD      },
    };
    for (auto& p : probes) {
        auto addr = opendojo::subsystems::lookup(p.key);
        OPENDOJO_LOG("  %-9s subsystem = 0x%llX %s",
                    p.name,
                    static_cast<unsigned long long>(addr),
                    addr ? "" : "(not resolved)");
    }

    // Start the hotkey loop on its own thread. It registers F1..F8 / Ctrl+1..8
    // / F9 and dispatches to commands::* — survives this thread's exit.
    opendojo::hotkeys::start();

    OPENDOJO_LOG("init thread done — entering diagnostic poll loop");

    // Diagnostic poller: log transitions in pool1 / subsystem availability so
    // we can see exactly when the game finishes initializing them. Runs for
    // up to 10 minutes after attach, then exits. Removed once hotkeys land
    // and we have a manual status trigger.
    constexpr auto poll_deadline = std::chrono::minutes(10);
    constexpr auto poll_interval = std::chrono::seconds(2);
    const auto t0 = std::chrono::steady_clock::now();

    struct probe_state { const char* name; std::uintptr_t key; std::uintptr_t last; };
    probe_state state[] = {
        { "gameplay",  opendojo::subsystems::KEY_GAMEPLAY,  0 },
        { "singleton", opendojo::subsystems::KEY_SINGLETON, 0 },
        { "subB",      opendojo::subsystems::KEY_SUBB,      0 },
        { "subC",      opendojo::subsystems::KEY_SUBC,      0 },
        { "subD",      opendojo::subsystems::KEY_SUBD,      0 },
    };
    std::uintptr_t last_pool1                                = 0;
    bool           announced_ok                              = false;
    std::uint16_t  last_slot_counts[opendojo::slot::USER_SLOTS] = {};

    auto elapsed_s = [&] {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count();
    };

    while (std::chrono::steady_clock::now() - t0 < poll_deadline) {
        bool all_resolved = true;
        for (auto& p : state) {
            auto cur = opendojo::subsystems::lookup(p.key);
            if (cur != p.last) {
                OPENDOJO_LOG("[poll T+%llds] %s: 0x%llX -> 0x%llX",
                            static_cast<long long>(elapsed_s()),
                            p.name,
                            static_cast<unsigned long long>(p.last),
                            static_cast<unsigned long long>(cur));
                p.last = cur;
            }
            if (!p.last) all_resolved = false;
        }

        auto pool1_now = opendojo::subsystems::pool1();
        if (pool1_now != last_pool1) {
            OPENDOJO_LOG("[poll T+%llds] pool1: 0x%llX -> 0x%llX",
                        static_cast<long long>(elapsed_s()),
                        static_cast<unsigned long long>(last_pool1),
                        static_cast<unsigned long long>(pool1_now));
            last_pool1 = pool1_now;

            // Confirm slot reads work end-to-end: dump every user slot's
            // event count the moment pool1 first appears.
            if (pool1_now) {
                for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
                    auto n = opendojo::slot::event_count(i);
                    OPENDOJO_LOG("  slot %zu: %u events", i + 1, static_cast<unsigned>(n));
                    last_slot_counts[i] = n;
                }
            }
        }

        // Track slot event-count transitions (a new recording, a slot
        // clear, etc.) so the log shows in-engine recording activity.
        if (pool1_now) {
            for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
                auto cur = opendojo::slot::event_count(i);
                if (cur != last_slot_counts[i]) {
                    OPENDOJO_LOG("[poll T+%llds] slot %zu: %u -> %u events",
                                static_cast<long long>(elapsed_s()),
                                i + 1,
                                static_cast<unsigned>(last_slot_counts[i]),
                                static_cast<unsigned>(cur));
                    last_slot_counts[i] = cur;

                    // Drill round-trip self-test on any new recording.
                    // Encodes the bytes to text, decodes the text back,
                    // and confirms the meaningful prefix matches.
                    if (cur > 0) {
                        std::uint8_t bytes[opendojo::slot::SLOT_PITCH];
                        if (!opendojo::slot::read(i, bytes)) {
                            OPENDOJO_LOG("  [drill] slot %zu: read failed", i + 1);
                            continue;
                        }
                        std::string text = opendojo::drill::encode_text(bytes, i);
                        auto r = opendojo::drill::decode_text(text);
                        if (!r.error.empty()) {
                            OPENDOJO_LOG("  [drill] slot %zu: decode FAILED: %s",
                                        i + 1, r.error.c_str());
                            continue;
                        }
                        const std::size_t meaningful = 2 + std::size_t(cur) * 4;
                        bool ok = (r.data.size() == opendojo::slot::SLOT_PITCH);
                        std::size_t first_diff = 0;
                        if (ok) {
                            for (std::size_t b = 0; b < meaningful; ++b) {
                                if (bytes[b] != r.data[b]) {
                                    ok = false;
                                    first_diff = b;
                                    break;
                                }
                            }
                        }
                        if (ok) {
                            OPENDOJO_LOG("  [drill] slot %zu: round-trip OK (%u events, %zu bytes)",
                                        i + 1, static_cast<unsigned>(cur), meaningful);
                        } else {
                            OPENDOJO_LOG("  [drill] slot %zu: MISMATCH at byte %zu (orig=0x%02X, decoded=0x%02X)",
                                        i + 1, first_diff,
                                        static_cast<unsigned>(bytes[first_diff]),
                                        static_cast<unsigned>(r.data[first_diff]));
                        }
                    }
                }
            }
        }

        if (all_resolved && !announced_ok) {
            OPENDOJO_LOG("[poll T+%llds] all subsystems resolved",
                        static_cast<long long>(elapsed_s()));
            announced_ok = true;
        }

        std::this_thread::sleep_for(poll_interval);
    }

    OPENDOJO_LOG("[poll] diagnostic poller exiting after %llds",
                static_cast<long long>(elapsed_s()));
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);

            // Open the log first so proxy::load() failures show up there.
            opendojo::log::init();

            // Real dinput8.dll must be reachable before the game makes its
            // first forwarded call. If this fails the game will crash on
            // DirectInput8Create, so we abort the DLL load and let the game
            // fall back to the real system DLL.
            if (!opendojo::proxy::load()) {
                OPENDOJO_LOG("proxy::load() failed — refusing to attach");
                opendojo::log::shutdown();
                return FALSE;
            }

            // Real work runs off the loader lock.
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
