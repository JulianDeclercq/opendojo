#pragma once

// Native in-game menu — Phase 0 scaffolding.
//
// Long-term replacement for the ImGui + D3D12 vtable-hook overlay. Uses
// Tekken 8's own UMG widget (UPolarisUMGTextMenu) instantiated as a
// sibling overlay. Native fonts, native theming, no DXGI hook stack.
//
// See NATIVE_MENU_DESIGN.md for the full plan and runtime-validation
// open questions.
//
// Phase 0 (this commit): pattern-pin findUnrealClass, resolve the
// UPolarisUMGTextMenu UClass*, stub out construction / show / hide /
// item population. Build green; no behavior change at runtime. The
// existing ImGui menu is untouched.
//
// Phase 1 (next runtime session): wire NewObject, AddToViewport,
// Initialize via the widget's vtable; populate items; route input.

namespace opendojo::native_menu {

// One-time pattern resolution. Cheap to call repeatedly — caches
// internally. Returns true once all required UE5 primitives have been
// pattern-pinned. Currently: findUnrealClass + the PolarisUMGTextMenu
// UClass pointer. The rest of Phase 1 will extend this set.
bool ensure_resolved();

// Toggle the native menu. Constructs the widget the first time `show`
// is called and reuses it across show/hide cycles afterward. No-op until
// Phase 1 wires the construction path.
void show();
void hide();
bool is_visible();
void toggle();

// Per-frame entry point. Polls the hotkey (F11) and drives show / hide.
// Called from render_hook::hook_present once we've integrated it.
void tick();

}  // namespace opendojo::native_menu
