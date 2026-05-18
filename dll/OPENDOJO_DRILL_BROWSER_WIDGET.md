OpenDojo Drill Browser — UMG widget specification
==================================================

Date: 2026-05-17
Target: UE 5.2.1 (Polaris-mirror project, Nexus T8 tutorial #435)
Output: `OpenDojo_P.pak` (+ `.utoc`/`.ucas`) at
        `<TEKKEN 8>/Polaris/Content/Paks/LogicMods/OpenDojo/`
Goal:   First canonical-UMG OpenDojo feature — replaces the F12 ImGui
        "Drills" tab. User browses drills on disk and picks Add (append
        to free slots) or Replace (clear all + load).

----------------------------------------------------------------------
Architecture overview
----------------------------------------------------------------------

```
BPModLoader (existing UE4SS mod) loads our .pak
   │
   ▼ spawns on LoadMapPost
BP_OpenDojoLoader_C : AActor
   │  PreBeginPlay:
   │    1. CreateWidget(WBP_UI_OpenDojo_Root_C, owner=PC)
   │    2. AddToViewport(widget, ZOrder=100)
   │    3. SetVisibility(Collapsed) — hidden until DLL asks
   │  PostBeginPlay:
   │    4. Cache the widget reference on this actor at field `Root`
   │       so the DLL can find it.
   ▼
WBP_UI_OpenDojo_Root_C : UUserWidget
   │  Holds the drill-browser sub-widget. Single root container so the
   │  DLL only needs one CreateWidget chain.
   ▼
WBP_UI_OpenDojo_DrillBrowser_C : UUserWidget
   │  The actual UI. Background overlay, title, scrollable drill list,
   │  close button. Communicates with DLL via:
   │    - DLL → widget:  AddDrillRow / ClearDrillRows / Show / Hide
   │    - widget → DLL:  three properties polled each tick
   ▼
WBP_UI_OpenDojo_DrillRow_C : UUserWidget
      One row inside the ScrollBox. Name, character, recording count,
      Add and Replace buttons. Fires a BP event up to the browser when
      a button is clicked.
```

DLL ↔ widget contract:
- DLL pushes data IN via BP-callable UFunctions on the browser widget.
- Widget signals events OUT via three integer/bool properties the DLL
  polls each tick (same pattern as `IsDialogDecided` for the native
  dialog primitive — see [[project_tekken_dialog_api]]).

----------------------------------------------------------------------
Why this shape (and not a Multicast Delegate)
----------------------------------------------------------------------

Binding a UE5 dynamic delegate from a DLL is non-trivial — would need
us to register a UFunction in our own UClass. Polling a couple of
properties is trivial and matches the dialog primitive's contract that
already works. Cost is one read of three properties per frame from
the render-hook tick — negligible.

----------------------------------------------------------------------
1. WBP_UI_OpenDojo_DrillRow_C
----------------------------------------------------------------------

Parent class: `UUserWidget`
Asset path:   `/Game/OpenDojo/UI/WBP_UI_OpenDojo_DrillRow.WBP_UI_OpenDojo_DrillRow_C`

### Variables (set via Variables panel; mark all as Instance Editable
where the user-facing label changes per row)

| Name              | Type            | Default | Notes                                                   |
|-------------------|-----------------|---------|---------------------------------------------------------|
| `Browser`         | `WBP_UI_OpenDojo_DrillBrowser_C*` |    | Set by parent on construction; used for click callback. |
| `DrillIndex`      | int32           | -1      | Stable index into the DLL's list_drills() result.       |
| `BP_DrillName`    | FString         | ""      | Display name.                                           |
| `BP_Character`    | FString         | ""      | "kazuya", "yoshimitsu", etc.                            |
| `BP_RecordingCount` | int32         | 0       | How many recordings inside the drill.                   |

### Widget tree (Designer view)

```
[Root: CanvasPanel]                       (auto for UserWidget)
└── HorizontalBox (Anchored: full, Padding: 4)
    ├── TextBlock (Size: Fill 2.4)
    │      Bind .Text to {BP_DrillName}
    ├── TextBlock (Size: Fill 1.0)
    │      Bind .Text to {BP_Character}
    ├── TextBlock (Size: Fill 0.8)
    │      Bind .Text to FString({BP_RecordingCount})
    ├── Button "btn_Add" (Size: Fixed 70px)
    │   └── TextBlock "Add"
    └── Button "btn_Replace" (Size: Fixed 80px)
        └── TextBlock "Replace"
```

### Functions / events

`UpdateData(FString Name, FString Character, int32 Recordings, int32 Index, WBP_UI_OpenDojo_DrillBrowser_C* Parent)`
- Marked **BlueprintCallable** so the DLL can invoke via ProcessEvent.
- Stores all five args into the matching variables.

`OnClicked_btn_Add` (auto-generated from Button click)
- Calls `Browser.OnRowAction(DrillIndex, 0)`.   // 0 = Add

`OnClicked_btn_Replace`
- Calls `Browser.OnRowAction(DrillIndex, 1)`.   // 1 = Replace

----------------------------------------------------------------------
2. WBP_UI_OpenDojo_DrillBrowser_C
----------------------------------------------------------------------

Parent class: `UUserWidget`
Asset path:   `/Game/OpenDojo/UI/WBP_UI_OpenDojo_DrillBrowser.WBP_UI_OpenDojo_DrillBrowser_C`

### Variables

| Name                  | Type     | Default | Purpose                                                  |
|-----------------------|----------|---------|----------------------------------------------------------|
| `RowsBox`             | `UVerticalBox*` | (bound, see Designer) | Container for row widgets. |
| `ScrollContainer`     | `UScrollBox*` | (bound) | Wraps RowsBox for scrolling. |
| `ClickAvailable`      | bool     | false   | Set to true when a row is clicked or Close pressed. **Mark BlueprintReadWrite + ExposeOnSpawn off.** |
| `ClickedDrillIndex`   | int32    | -1      | Index passed to AddDrillRow for the clicked row. Set in OnRowAction. |
| `ClickedAction`       | int32    | -1      | 0 = Add, 1 = Replace, 2 = Close. |

### Widget tree

```
[Root: CanvasPanel]
└── Image "BG_Dim" (Anchored: full, Color: black RGBA 0,0,0,0.75)
└── Border "BG_Panel" (Anchored: center, Size: 800x520, Brush color: dark)
    └── VerticalBox (Anchored: full inside the border, Padding: 16)
        ├── TextBlock "Title" (Text: "OpenDojo — Drills",
        │       Font size 28, Align center, Padding bottom: 8)
        ├── HorizontalBox (Header row — column labels)
        │   ├── TextBlock "Name"        (Fill 2.4, Color gray)
        │   ├── TextBlock "Character"   (Fill 1.0, Color gray)
        │   ├── TextBlock "Recordings"  (Fill 0.8, Color gray)
        │   ├── TextBlock "" (Fixed 70px)
        │   └── TextBlock "" (Fixed 80px)
        ├── ScrollBox "ScrollContainer" (Fill, padding-bottom 8)
        │   └── VerticalBox "RowsBox"
        └── HorizontalBox (Footer row)
            ├── Spacer (Fill)
            └── Button "btn_Close" (Size: 120x40)
                └── TextBlock "Close"
```

`RowsBox` and `ScrollContainer` must be **Variable** (the checkbox at the
top of the Designer panel) so we can refer to them in graphs.

### Functions / events

`AddDrillRow(FString Name, FString Character, int32 Recordings, int32 DrillIndex)`
- **BlueprintCallable**, called by DLL via ProcessEvent.
- Spawns a `WBP_UI_OpenDojo_DrillRow_C` via `Create Widget`.
- Calls the row's `UpdateData(Name, Character, Recordings, DrillIndex, Self)`.
- Adds it to `RowsBox` via `Add Child to Vertical Box`.

`ClearDrillRows()`
- **BlueprintCallable**.
- Calls `RowsBox.ClearChildren()`.

`OnRowAction(int32 DrillIndex, int32 Action)`
- Called by row widgets when their Add/Replace button is clicked.
- Sets `ClickedDrillIndex = DrillIndex`, `ClickedAction = Action`,
  `ClickAvailable = true`.

`OnClicked_btn_Close` (from Button click)
- Sets `ClickedDrillIndex = -1`, `ClickedAction = 2`, `ClickAvailable = true`.

----------------------------------------------------------------------
3. WBP_UI_OpenDojo_Root_C
----------------------------------------------------------------------

Parent class: `UUserWidget`
Asset path:   `/Game/OpenDojo/UI/WBP_UI_OpenDojo_Root.WBP_UI_OpenDojo_Root_C`

This is a thin shell so the DLL has a single, stable widget instance to
locate. It owns the current sub-screen — for v1 there's only one
(DrillBrowser).

### Variables

| Name        | Type                                  | Purpose                                |
|-------------|---------------------------------------|----------------------------------------|
| `Browser`   | `WBP_UI_OpenDojo_DrillBrowser_C*`     | Reference to the active sub-widget.    |

### Widget tree

```
[Root: CanvasPanel]
└── WidgetSwitcher "Switcher" (Anchored: full)
    └── WBP_UI_OpenDojo_DrillBrowser_C "Browser"
```

Initial visibility: **Collapsed** (set on the CanvasPanel via the
Visibility property in Designer).

### Functions

`ShowDrillBrowser()`
- Set widget Visibility to `SelfHitTestInvisible` (visible, doesn't
  block input below — or `Visible` if we want it to block).
- Set Switcher.ActiveWidget = Browser.
- Call Browser.SetKeyboardFocus() (so the controller / keyboard input
  goes here).

`HideAll()`
- Visibility = Collapsed.

`GetBrowser()` → `WBP_UI_OpenDojo_DrillBrowser_C*`
- Returns `Browser`. BlueprintCallable. DLL uses this to get the
  browser pointer without walking children.

----------------------------------------------------------------------
4. BP_OpenDojoLoader_C (the Actor BPModLoader spawns)
----------------------------------------------------------------------

Parent class: `AActor`
Asset path:   `/Game/OpenDojo/BP_OpenDojoLoader.BP_OpenDojoLoader_C`

### Variables

| Name        | Type                          | Purpose                                       |
|-------------|-------------------------------|-----------------------------------------------|
| `Root`      | `WBP_UI_OpenDojo_Root_C*`     | The constructed root widget.                  |

### Events

`PreBeginPlay()` (override)
1. Get Player Controller (index 0).
2. `Root = CreateWidget(WBP_UI_OpenDojo_Root_C, Owner=PC)`.
3. `Root.AddToViewport(ZOrder=100)`.
4. *(Visibility starts Collapsed via the widget's default.)*

`PostBeginPlay()` (override)
- (No-op for now. Reserved for any post-spawn handshake we discover
  the DLL needs.)

----------------------------------------------------------------------
5. config.lua for BPModLoader
----------------------------------------------------------------------

Drop alongside the .pak so BPModLoader knows which Actor class to spawn.

Path: `<TEKKEN 8>/Polaris/Content/Paks/LogicMods/OpenDojo/config.lua`

```lua
Mods["OpenDojo"] = {
    AssetName = "BP_OpenDojoLoader_C",
    AssetPath = "/Game/OpenDojo/BP_OpenDojoLoader",
}
```

----------------------------------------------------------------------
6. Packaging
----------------------------------------------------------------------

In the Polaris-mirror UE 5.2 project:

1. **Project Settings → Packaging** → enable **"Use Io Store"** (so the
   output matches Tekken's runtime container format).
2. **File → Cook Content** → Windows.
3. From a shell:
   ```
   retoc to-zen --version UE5_2 <project>/Saved/Cooked/Windows OpenDojo_P.utoc
   ```
   This produces `OpenDojo_P.utoc` + `OpenDojo_P.ucas` + `OpenDojo_P.pak`.

4. Copy all three plus `config.lua` into:
   `<TEKKEN 8>/Polaris/Content/Paks/LogicMods/OpenDojo/`

5. Launch Tekken. BPModLoader's `LoadMapPost` hook will spawn
   `BP_OpenDojoLoader_C` automatically.

----------------------------------------------------------------------
7. DLL integration plan (Phase-2, follow-up commit)
----------------------------------------------------------------------

Once the .pak is dropping correctly, the DLL needs to:

1. **Resolve** `WBP_UI_OpenDojo_Root_C` UClass via `findUnrealClass` —
   reusable from `native_menu.cpp`. Path: `/Game/OpenDojo/UI/WBP_UI_OpenDojo_Root.WBP_UI_OpenDojo_Root_C`.
2. **Find the live instance** of that class via the existing
   `findUnrealObjectsOfClass` — there'll be exactly one (the one
   `BP_OpenDojoLoader` created).
3. **Resolve UFunctions** by name on the live widget:
   - `GetBrowser` on `WBP_UI_OpenDojo_Root_C`
   - `ShowDrillBrowser` / `HideAll` on the Root
   - `ClearDrillRows` / `AddDrillRow` on the Browser
4. **Push drills**: for each `DrillHeader` in `commands::list_drills()`,
   build the parms blob (FString name, FString character, int32 count,
   int32 index) and call `AddDrillRow` via the existing `call_pe()`
   helper in `dialog.cpp`.
5. **Show**: call `ShowDrillBrowser` on the Root.
6. **Poll** each tick: read `ClickAvailable` from the Browser. When
   true, read `ClickedDrillIndex` and `ClickedAction`, dispatch:
   - `Action == 0`: `load_drill(drills[index].path, AppendToFree)`
   - `Action == 1`: `load_drill(drills[index].path, ReplaceAll)`
   - `Action == 2`: just hide.
   Then call `HideAll` and set `ClickAvailable = false`.

The DLL stub for this lives in `dll/src/drill_browser.{hpp,cpp}` —
shipped alongside this doc.

----------------------------------------------------------------------
8. Open questions to resolve in-editor
----------------------------------------------------------------------

- **Input routing**: does the widget receive controller input by
  default, or do we need to `SetInputMode_GameAndUI` / take focus?
  Test by adding a button and confirming controller D-pad selects it.
- **Tickrate of ClickAvailable polling**: 60 Hz render tick is fine,
  no need for a dedicated thread.
- **Drill name length**: long names will need TextOverflow=Ellipsis on
  the row's name TextBlock. Set on the row TextBlock's wrap settings.
- **Empty list state**: if `RowsBox` is empty after `AddDrillRow` calls,
  add a "No drills saved yet" placeholder TextBlock. Easy follow-up.
