#include "hotkeys.hpp"

#include "log.hpp"

// Hotkeys are intentionally disabled in this build. The in-game menu (toggled
// with F12) is the only UX surface while we lock down the menu design.
// When hotkeys come back, they will be configurable from the menu's Settings
// tab and registered through this module.

namespace opendojo::hotkeys {

bool start() {
    OPENDOJO_LOG("hotkeys: disabled in this build — use the in-game menu (F12)");
    return true;
}

}  // namespace opendojo::hotkeys
