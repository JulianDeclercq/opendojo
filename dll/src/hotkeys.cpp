#include "hotkeys.hpp"

#include <windows.h>

#include <atomic>
#include <thread>

#include "commands.hpp"
#include "log.hpp"

namespace openlab::hotkeys {

namespace {

// Hotkey IDs. RegisterHotKey just needs them unique within the process.
constexpr int ID_EXPORT_BASE = 1;    // F1..F8       -> IDs 1..8
constexpr int ID_IMPORT_BASE = 11;   // Ctrl+1..8    -> IDs 11..18
constexpr int ID_STATUS      = 19;   // F9           -> ID 19

std::atomic<bool> g_started{false};

void run_loop() {
    int ok = 0;
    int fail = 0;

    for (int i = 0; i < 8; ++i) {
        if (RegisterHotKey(nullptr, ID_EXPORT_BASE + i, 0, VK_F1 + i)) {
            ++ok;
        } else {
            OPENLAB_LOG("hotkey: RegisterHotKey F%d failed (GLE=%lu)",
                        i + 1, GetLastError());
            ++fail;
        }
        if (RegisterHotKey(nullptr, ID_IMPORT_BASE + i, MOD_CONTROL, '1' + i)) {
            ++ok;
        } else {
            OPENLAB_LOG("hotkey: RegisterHotKey Ctrl+%d failed (GLE=%lu)",
                        i + 1, GetLastError());
            ++fail;
        }
    }
    if (RegisterHotKey(nullptr, ID_STATUS, 0, VK_F9)) {
        ++ok;
    } else {
        OPENLAB_LOG("hotkey: RegisterHotKey F9 failed (GLE=%lu)", GetLastError());
        ++fail;
    }

    OPENLAB_LOG("hotkeys: %d registered, %d failed — F1..F8 export, Ctrl+1..8 import, F9 status",
                ok, fail);

    // Message pump. The process holds this thread for its lifetime; we
    // never deliberately exit. The OS tears down the thread + unregisters
    // hotkeys at process termination.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message != WM_HOTKEY) continue;
        const int id = static_cast<int>(msg.wParam);
        if (id >= ID_EXPORT_BASE && id < ID_EXPORT_BASE + 8) {
            openlab::commands::export_slot(static_cast<std::size_t>(id - ID_EXPORT_BASE));
        } else if (id >= ID_IMPORT_BASE && id < ID_IMPORT_BASE + 8) {
            openlab::commands::import_slot(static_cast<std::size_t>(id - ID_IMPORT_BASE));
        } else if (id == ID_STATUS) {
            openlab::commands::show_status();
        }
    }
}

}  // anonymous

bool start() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return true;
    std::thread(run_loop).detach();
    return true;
}

}  // namespace openlab::hotkeys
