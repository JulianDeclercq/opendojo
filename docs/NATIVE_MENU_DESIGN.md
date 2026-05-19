# Native In-Game Menu — Design Doc

**Status**: research / proposal. No code shipped. Produced from static-only
Ghidra analysis of Polaris-Win64-Shipping.exe (Tekken 8 v3.00.02) plus a
review of Irony's UE class-lookup pattern.

**Goal**: replace the current ImGui/D3D12 overlay with a menu that uses
Tekken 8's own UMG widgets — native fonts, native theming, gamepad
navigation through the game's input router, no DXGI vtable hooking, no
ImGui dependency.

## TL;DR

- Tekken 8's UI is a **two-layer architecture**: a studio C++ framework
  called **Kamui** (scenes + UI controllers + sequences) sitting on top
  of **UE5 UMG widgets**. We don't need to understand Kamui to ship a
  native menu — we can instantiate UMG widgets directly.
- The cleanest path is **Approach C: Instantiate `UPolarisUMGTextMenu`
  via UE5's `NewObject` + `AddToViewport`**. It's a reusable text-menu
  widget Polaris ships specifically for selectable lists (it has
  `Selectable`, `Editing`, and `Clamp` delegates). We construct one,
  populate it with drill names, and overlay it ourselves.
- Approach C trades some integration depth for **zero risk to Kamui's
  internal state machine** — we don't intercept the existing pause
  menu, we just spawn a sibling overlay that looks native.
- Drop ImGui, drop MinHook, drop the D3D12 ResizeBuffers/Present
  vtable swap. Replace with: pattern-scan three UE5 helpers
  (`FindObject<UClass>`, `NewObject`, `UUserWidget::AddToViewport`),
  build a thin C++ shim, plug into the existing menu logic.

---

## 1. Architecture map

### 1.1 Two-layer UI

```
┌─────────────────────────────────────────────────────────┐
│ Kamui C++ framework (studio-specific, namespace kamui)  │
│   kamui::scene::* — one scene per game screen           │
│   kamui::ui::*Impl — UI controllers (own UMG widgets)   │
│   kamui::sequence::* — top-level state machines         │
│   kamui::task::* — coroutine-style task primitives      │
└────────────────────────┬────────────────────────────────┘
                         │ owns / drives
                         ▼
┌─────────────────────────────────────────────────────────┐
│ UE5 UMG widgets (presentation, namespace UPolarisUMG*) │
│   UPolarisUMGPracticeMenu — the in-practice pause menu  │
│   UPolarisUMGPauseMenu — the in-battle pause menu       │
│   UPolarisUMGTextMenu — reusable text-list widget       │
│   UPolarisUMGDialog, UPolarisUMGSelectValueDialog, ...  │
└─────────────────────────────────────────────────────────┘
```

Concrete examples we found in Ghidra:

| C++ controller (Kamui) | UMG widget (UE5) | Blueprint asset |
|---|---|---|
| `kamui::ui::PracticeMenuImpl` | `UPolarisUMGPracticeMenu` | `/Game/UI/Widget/Practice/WBP_UI_PracticeMenu.WBP_UI_PracticeMenu_C` |
| `kamui::ui::PauseMenuImpl` | `UPolarisUMGPauseMenu` | `/Game/UI/Widget/Pause/WBP_UI_Pause.WBP_UI_Pause_C` |
| `kamui::ui::CharSelectImpl` | `UPolarisUMGCharSelect` | (similar) |
| `kamui::ui::QuickSelectImpl` | `UPolarisUMGQuickSelect` | (similar) |

### 1.2 The full UMG widget catalog (filtered for relevance)

From the `UPolarisUMG*` string pass — ~110 widget classes total. The
ones that look most useful for our scenarios:

**Reusable building blocks (most interesting)**
- `UPolarisUMGTextMenu` — list widget with `PolarisUMGTextMenuDelegate`,
  `PolarisUMGTextMenuSelectableDelegate`, `PolarisUMGTextMenuEditingDelegate`,
  `PolarisUMGTextMenuClampDelegate`. The naming pattern strongly implies
  "list of selectable rows with optional inline editing and value clamping"
  — basically what we want for drill selection.
- `UPolarisUMGDialog` — generic modal dialog (yes / no / cancel).
- `UPolarisUMGSelectValueDialog` — pick-a-value modal.
- `UPolarisUMGHelpDialog` — help text modal.
- `UPolarisUMGScrollBox` — scrollable container.
- `UPolarisUMGSlotSelect` — slot picker (relevant: this is probably the
  practice-mode "Input Recording" slot UI).

**Existing menus we could attach to or replace**
- `UPolarisUMGPracticeMenu` — the practice-mode pause menu. Hijack target.
- `UPolarisUMGPauseMenu` — the battle pause menu.
- `UPolarisUMGOptionHUD` — options HUD overlay.
- `UPolarisUMGCommandList` — the move-list overlay.
- `UPolarisUMGCommandListHelp` — move-list help overlay (low traffic,
  could potentially be repurposed).

**Notification-style widgets (low-stakes prototyping)**
- `UPolarisUMGSmallNotice` — small notification toast.
- `UPolarisUMGInformationBar` — banner/info bar.
- `UPolarisUMGTAMTelop` / `UPolarisUMGTAMTutorialTelop` — tutorial telop
  (text overlay during gameplay).

### 1.3 Subsystems we already touch

The recording-pool world (relevant because the new menu replaces the
ImGui control surface for the same operations):

| Service-locator key | Subsystem | Notes |
|---|---|---|
| `KEY_GAMEPLAY = 0x9537314` | Gameplay subsystem (slot-recorded flags at +0x480) | already used by `slot::set_recorded_flag` |
| `KEY_SINGLETON = 0x95371B0` | Singleton — "side gate" + record-active flags | already used |
| `KEY_RECORDING = 0x95371A4` | Recording subsystem — `this` for `pool_init` (newly identified during pool RE) | already used (`subsystems::ensure_pool_allocated`) |
| `KEY_SUBB = 0x953707C`, `KEY_SUBC = 0x9537080` | Per-slot "any recording exists" flag-bags | already used |

The pool/flag layer is independent of the UI layer. The native menu doesn't
change anything here — it just changes how the user *triggers* the
existing `commands::load_drill` / `commands::export_current_slots`
operations.

### 1.4 What we did NOT find (limits of static analysis)

- **The C++ classes `kamui::ui::PracticeMenuImpl` / `kamui::ui::PauseMenuImpl`
  do not have direct RTTI strings** (`.?AVPracticeMenuImpl@ui@kamui@@`
  is absent). They exist only inside `std::bind`-style _Binder mangled
  type descriptors. This means: either the classes are never directly
  instantiated by `dynamic_cast` / `typeid`, or RTTI was stripped for
  these specific types. We can't reliably xref them from Ghidra without
  finding their vtables by another route.
- **The UMG widget UClass registration table** is referenced by data
  pointers (e.g. `0x1495e7568` for `UPolarisUMGPracticeMenu`) but Ghidra
  didn't propagate xrefs from the registration table. To find a widget's
  `UClass*` at runtime we have to use UE5's `FindObject<UClass>` —
  same mechanism Irony uses.
- **Widget construction / `AddToViewport` exact addresses** weren't
  pinned. They exist as virtual methods on `UUserWidget`'s vtable — we'd
  need to resolve the vtable at runtime via a UClass lookup, or
  pattern-scan for the function bodies (Irony uses pattern-scan).

---

## 2. Approaches

Three viable approaches, ordered by integration depth.

### Approach A — Hijack `UPolarisUMGPracticeMenu`

**What**: insert OpenDojo entries into the existing in-practice pause menu.
The user opens the practice pause menu like normal and sees "OpenDojo"
alongside other items.

**How**:
1. Hook `kamui::ui::PracticeMenuImpl::OnShow` (or equivalent — needs
   RE'ing). Address unknown; the C++ class lacks direct RTTI so we'd
   have to walk back from one of its `_Binder` mangled-name xrefs.
2. After the menu's normal items are populated, append our entries by
   mutating the underlying item list.
3. Bind a callback that fires when the user selects our entry —
   triggers `commands::load_drill` etc.

**Pros**:
- Deepest integration. Menu appears in the exact place users already
  look for practice settings.
- Inherits all of the game's input routing, focus handling, and
  back-button behavior for free.

**Cons**:
- **Highest risk**. We don't know the practice menu's data model. The
  item list might be a `TArray<UPolarisUMGTextMenuRow*>`, a fixed-size
  static array, or a blueprint-defined widget tree. Without instrumenting
  a live process, we'd be guessing.
- **Brittle to patches**. Anything we patch inside the kamui controller
  flow risks breaking on any game update.
- **Hard to RE without runtime access**. `PracticeMenuImpl` has no RTTI
  string in this build — we'd need to walk from `_Binder` references to
  find its vtable, then guess which slot is `OnShow`.

**Estimate**: 2–4 weeks of careful RE + extensive runtime testing once a
process is available. Not a fit for the static-only window.

### Approach B — Spawn a sibling `UPolarisUMGTextMenu` instance

**What**: skip hijacking entirely. Construct our own instance of
`UPolarisUMGTextMenu`, populate it with drill rows, add it to the
viewport ourselves. Looks native because it *is* the game's widget — same
font, same theme, same animations.

**How**:
1. Resolve `UClass* PolarisUMGTextMenuClass = FindObject<UClass>(
   nullptr, L"/Script/Polaris.PolarisUMGTextMenu", true);` — Irony's
   `findUnrealClass` pattern works here.
2. Get a `UWorld*` / `APlayerController*` to act as the widget owner —
   pattern-scan or read `GEngine`-equivalent global.
3. `UUserWidget* w = NewObject<UUserWidget>(owner, PolarisUMGTextMenuClass);`
4. `w->Initialize();`
5. `w->AddToViewport(zOrder);` — call via the widget's vtable.
6. Populate items by writing to the widget's properties / binding its
   delegates. The exact API needs more RE — `UPolarisUMGTextMenu`'s
   delegate signatures (`Selectable`, `Editing`, `Clamp`) tell us
   roughly the shape but not the exact methods.

**Pros**:
- **Native look-and-feel** — same widget the game uses internally for
  other text lists.
- **No interference with Kamui state**. Our widget is sibling, not
  embedded. If we crash, the game keeps running.
- **Pattern lifted from Irony**: they already do `FindObject<UClass>`
  at Polaris. Pattern + offset + RIP-relative resolve, same as our
  `players::detect_cpu` machinery.
- **Replaces the ImGui + D3D12 hooking stack entirely** — kill
  `render_hook.cpp`, `theme.cpp`, the MinHook dependency, the dxgi
  vtable swap, the ResizeBuffers/AddRef caveats from
  `feedback_opendojo_render_hook_caveats`.

**Cons**:
- **Item-population API is unknown**. `UPolarisUMGTextMenu` likely has
  a method like `AddItem(FText label, FDelegate onSelect)` — but we
  don't know the exact signature. Best-case: it's a simple
  `void(*)(UPolarisUMGTextMenu*, FText*, FScriptDelegate*)`. Worst-case:
  it's blueprint-driven and we have to populate a `TArray` field
  directly.
- **Input routing**. When our widget is visible, we need the game's
  input system to deliver gamepad / keyboard events to it, not to the
  active scene. Standard UE5 fix: call
  `UWidgetBlueprintLibrary::SetInputMode_UIOnly(playerController, w)`.
  Have to confirm this works while in a practice match.
- **Viewport z-order**. UMG draws below the game's HUD by default. We
  want our menu on top of everything when shown.

**Estimate**: 1–2 weeks once a process is available for runtime
inspection. Roughly the same effort as the current ImGui menu took, but
the result is permanent (no DXGI hooking maintenance, no per-patch
fragility from vtable scans). **This is the recommended path.**

### Approach C — Hybrid: build our own UClass via UE5 reflection

**What**: register a custom UClass at runtime that inherits from
UUserWidget. Use UE5's reflection system to define widget slots
programmatically. Tinted with native widgets but a new identity.

**How**:
1. `UClass::CreateClass(parent=UUserWidget::StaticClass(), ...)` — UE5
   exposes this at the C++ level.
2. Populate the class's vtable / FProperty list to add child widgets
   (image, text, scroll box).
3. Instantiate and AddToViewport as in Approach B.

**Pros**: Maximum control over widget tree. We don't depend on the
behavior of any specific Polaris widget.

**Cons**: **Extremely high effort**. UE5's class-registration internals
are large and version-sensitive. Anyone who's done this before in a UE5
mod has invested weeks. Strongly not recommended unless the simpler
approaches dead-end.

**Estimate**: 4–8+ weeks. Skip.

---

## 3. Recommended path: Approach B

**Phase 0 — proof of concept (no game running yet)**

Build out the static scaffolding while we wait for runtime access:

1. **New module `unreal.hpp/.cpp`** that knows how to call UE5
   primitives from inside Polaris. Mirrors the pattern in Irony's
   `game/memory.zig` — three RIP-relative offsets to resolve at module
   init:
   - `FindObject<UClass>` — Irony pattern:
     `45 33 C0 49 8B CF E8 ?? ?? ?? ?? 48 8B 4C 24 60` (offset +7,
     resolve RIP-relative).
   - `UUserWidget::AddToViewport` — needs pattern (TBD; resolve from
     `AddToViewport` string xref at `0x147f9b458` once UE registers it,
     or just walk the UClass vtable when we have a widget instance).
   - `NewObject` equivalent — UE5 ships
     `UObject* StaticConstructObject_Internal(...)`. Pattern is well
     known in UE5 modding circles; pin via xref to "Outer".
2. **Resolve `UClass*` for `UPolarisUMGTextMenu`** at init:
   `FindObject(nullptr, L"/Script/Polaris.PolarisUMGTextMenu", true)`.
3. **Resolve a world / player controller**. Two options:
   - Use Irony's pattern-resolved holder (the same chain we already use
     in `players::detect_cpu`). The Player struct's outer is the world.
   - Search for `GWorld` global — UE5 defines this. There's an xref
     pattern from `FWorldContext::OwningGameInstance` we can lift.
4. **Wire the construction sequence**:
   ```cpp
   auto* cls = unreal::find_class("/Script/Polaris.PolarisUMGTextMenu");
   auto* pc  = unreal::get_first_player_controller();
   auto* w   = unreal::new_object<UUserWidget>(pc, cls);
   unreal::call_vtable<void()>(w, kInitializeVtableSlot);
   unreal::call_vtable<void(int)>(w, kAddToViewportVtableSlot, /*zOrder=*/100);
   ```
   Vtable slot indices need to be RE'd from `UUserWidget`'s vtable. We
   can derive them by finding the vtable (xref from a known UPolaris
   widget's class default object) and inspecting which slot points at
   the `AddToViewport` symbol.

**Phase 1 — minimal viable menu (first runtime session)**

1. Hotkey opens our widget, shows a static `UPolarisUMGTextMenu` with
   one hard-coded row. Goal: prove the widget is visible, on top of the
   game, with native styling.
2. Verify input routing — make sure gamepad / keyboard works on our
   widget without disrupting the game when hidden.

**Phase 2 — wire to existing OpenDojo commands**

1. Replace the static row with the live drill list from
   `commands::list_drills()`.
2. Bind selection callback → `commands::load_drill(path, ReplaceAll)`.
3. Add tabs / submenus by stacking widget instances or by using
   `UPolarisUMGScrollBox`.

**Phase 3 — remove ImGui stack**

Once Phase 2 is solid:

- Delete `render_hook.cpp`, `theme.cpp`, `menu.cpp` (replace with new
  native menu module).
- Delete MinHook + ImGui from `CMakeLists.txt`.
- Delete the DXGI vtable scaffolding.
- Keep the F12 (or chosen hotkey) input as a global keyboard hook
  driving our `show()` / `hide()`. Or migrate to a native input
  binding by hooking into Polaris's input mapping.

---

## 4. Risks and mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| `UPolarisUMGTextMenu`'s item-population API is more complex than just an `AddItem` method | High | First-session priority: dump the widget's vtable and FProperty list at runtime. If too complex, fall back to a simpler widget (`UPolarisUMGScrollBox` + manually-constructed `UTextBlock` children). |
| Input focus conflicts with active scene (gamepad inputs get eaten by the practice match instead of our menu) | Med | Standard UE5 fix is `SetInputMode_UIOnly`. Verify it works in a battle scene; if not, we may need to pause the input router via Polaris-side state. |
| Game patches change UMG widget UClass paths (rename `/Script/Polaris.PolarisUMGTextMenu`) | Low | All resolution goes through `FindObject` by name — names tend to be stable across minor patches. Treat the same as our existing AOB patterns: if a patch breaks them, re-pin. |
| Game updates change vtable layout of `UUserWidget` (rare) | Low | Resolve vtable slots by name-matching at init via UE5's reflection (UClass::Functions). Costs an init-time scan; saves us from version drift. |
| ImGui removal breaks the build before native menu is functional | High | Keep both in parallel during Phase 1 + 2. Only delete ImGui in Phase 3, behind a CMake flag if needed. |
| The C++ `PracticeMenuImpl` does its own per-frame teardown of UI it doesn't own (might delete our widget) | Low (sibling widget, not embedded) | If observed, AddToViewport into the game viewport directly instead of the player controller's widget stack. |

---

## 5. Open questions for the next runtime session

These need a live process to resolve and can't be answered from static
analysis:

1. **`UPolarisUMGTextMenu` API surface**: dump its vtable, walk its
   FProperty list. Find the method that adds items and the property
   layout that holds them.
2. **Best owner for the widget**: experiment with player-controller-
   owned vs. game-instance-owned widgets. PC-owned is simpler; GI-owned
   survives level transitions.
3. **Z-order discovery**: what value puts us above the practice HUD?
   100 is the conventional "menu" z-order in UE5; verify it lands above
   `UPolarisUMGOptionHUD` and friends.
4. **Input mode**: confirm `SetInputMode_UIOnly` works in a practice
   match. If it doesn't, find Polaris's input-router pause toggle.
5. **Style polish**: does `UPolarisUMGTextMenu` inherit theme/font
   automatically, or do we need to set them? (Probably automatic via
   the widget's default style assets.)

---

## 6. Implementation skeleton (proposed)

What the new code structure could look like, replacing the ImGui stack:

```
dll/src/
├── native_ui/
│   ├── unreal.hpp/.cpp        # UE5 primitives (FindObject, NewObject, vtable calls)
│   ├── widget.hpp/.cpp        # Thin wrapper around a UUserWidget* (show/hide/populate)
│   ├── drill_menu.hpp/.cpp    # The OpenDojo menu — list of drills, callbacks
│   └── input.hpp/.cpp         # Hotkey -> show/hide, focus mgmt
├── commands.hpp/.cpp          # unchanged
├── drill.hpp/.cpp             # unchanged
├── slot.hpp/.cpp              # unchanged
├── subsystems.hpp/.cpp        # unchanged
├── players.hpp/.cpp           # unchanged
├── autosave.hpp/.cpp          # unchanged
├── memory.hpp/.cpp            # unchanged
├── log.hpp/.cpp               # unchanged
├── main.cpp                   # init thread spawns native_ui::install()
└── proxy_dinput8.cpp          # unchanged (proxy DLL plumbing)
```

Removed: `render_hook.cpp/.hpp`, `menu.cpp/.hpp`, `theme.cpp/.hpp`,
ImGui + MinHook in CMakeLists.

Net change: less code, less D3D12 fragility, native look. Trade-off is
the upfront UE5 RE investment.

---

## 7. References

- **Irony** (`C:\Users\ethan\Desktop\Irony`): reference impl of
  `FindObject<UClass>` resolution at Polaris.
  - Pattern in `src/dll/game/memory.zig`:
    `45 33 C0 49 8B CF E8 ?? ?? ?? ?? 48 8B 4C 24 60` for findUnrealClass.
  - Pattern in same file for `findUnrealObjectsOfClass`:
    `E8 ?? ?? ?? ?? 90 48 89 6C 24 30`.
- **opendojo memory notes**:
  - `project-tekken-player-chain` (player struct chain via AOB)
  - `feedback-opendojo-render-hook-caveats` (DXGI gotchas that go away
    with native menu)
  - `project-tekken-pool-init` (pool allocation context)
- **UE5 docs / community RE**: standard patterns for `NewObject`,
  `AddToViewport`, `SetInputMode_UIOnly`. Many published examples
  available; we don't need to discover these from scratch.

---

*Document produced during a static-only investigation window. Live-process
follow-up is required to validate Phase 0 → 1.*
