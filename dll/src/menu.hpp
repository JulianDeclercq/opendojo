#pragma once

// Top-level menu driver — lives on top of the RmlUi backend.
// Owns the click handlers and the per-frame DOM refresh that pulls live
// data (drill list, slot occupancy, autosave/hotkey state) into the
// elements declared in main.rml. The render hook calls draw() each frame
// the menu is visible and invalidate() when it transitions from hidden
// to visible.

namespace opendojo::menu {

// One per-frame refresh + per-frame style/DOM sync. Cheap when the
// underlying state hasn't changed — Rml::Element::SetInnerRML diffs
// internally and AddEventListener is wired once on first call.
void draw();

// Mark the drill list cache as stale so the next draw() re-scans
// opendojo/. Cheap to call.
void invalidate();

}  // namespace opendojo::menu
