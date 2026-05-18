#pragma once

// Drill browser — first canonical-UMG OpenDojo feature.
//
// Depends on the .pak built per dll/OPENDOJO_DRILL_BROWSER_WIDGET.md
// shipping at <TEKKEN 8>/Polaris/Content/Paks/LogicMods/OpenDojo/.
// Once BPModLoader spawns BP_OpenDojoLoader_C and its PreBeginPlay
// adds the root widget to the viewport, this module:
//   1. Resolves WBP_UI_OpenDojo_Root_C and the Browser sub-widget.
//   2. Pushes the on-disk drill list into the widget via ProcessEvent
//      (AddDrillRow per drill).
//   3. Shows the widget; polls ClickAvailable + ClickedDrillIndex +
//      ClickedAction each render-tick.
//   4. On click, calls into opendojo::commands::load_drill for the
//      chosen drill + mode (Add / Replace) and hides the widget.
//
// Gracefully degrades when the .pak isn't deployed yet — open() logs
// and returns false instead of crashing.

namespace opendojo::drill_browser {

// Lazily resolve the widget classes + UFunctions on first call.
// Returns true once all resolution succeeded. Cheap to call again.
bool ensure_resolved();

// Show the browser populated with the current opendojo_drills/ list.
// Returns false if widget classes aren't loaded (no .pak yet) or no
// live root widget instance exists.
bool open();

// Per-render-tick poll for the widget's click signal. Cheap when no
// browser is open. Wire into the existing render_hook tick alongside
// dialog::tick().
void tick();

}  // namespace opendojo::drill_browser
