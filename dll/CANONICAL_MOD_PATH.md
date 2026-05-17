Canonical UE5 asset mod path for Tekken 8
=========================================

Date: 2026-05-17
Engine: UE 5.2.1 (string at Polaris+0x147627f20)

TL;DR
-----
Yes, the canonical path exists and works. **It requires UE 5.2 editor + a
custom T8 project** (~6–10 hours setup, ~120 GB disk, ~30 min per iteration).
The path lets us drop a `.pak` (+ `.utoc`/`.ucas` if IoStore-packed) into
`Polaris/Content/Paks/LogicMods/<ModName>/` and BPModLoaderMod auto-spawns
our Actor on level load. **Loc strings cannot use this path** — Tekken uses
Bandai's Gryphon system, which the community has no override workflow for;
`UPolarisTextBlock::SetRawText` remains the realistic loc bypass.

How BPModLoaderMod works (verified by reading its `main.lua`)
-------------------------------------------------------------
1. Scans `Polaris/Content/Paks/LogicMods/` for sub-folders + `.pak` files.
2. For each mod, either reads `LogicMods/<ModName>/config.lua` OR defaults
   to `AssetName="ModActor_C"`, `AssetPath="/Game/Mods/<ModName>/ModActor"`.
3. On `LoadMapPost` (level load) AND on **Insert** key (manual trigger),
   calls `AssetRegistryHelpers:GetAsset(...)` then `World:SpawnActor(ModClass)`.
4. Calls `PreBeginPlay()` on the spawned actor (and `PostBeginPlay()` later).
5. Mod must be an **Actor** subclass — plain widgets won't spawn directly,
   but the Actor's PreBeginPlay can `CreateWidget(WBP_X)` + `AddToViewport`.

Required tools (community-confirmed for T8)
-------------------------------------------
| Tool          | Purpose                                            | URL                                            |
|---------------|----------------------------------------------------|------------------------------------------------|
| **UE 5.2 editor** | Author BP graphs / widgets. Tekken-modder community has a customized 5.2 project mirroring Polaris's plugin set (shared via the Nexus tutorial Google Drive). | https://www.unrealengine.com/ + https://www.nexusmods.com/tekken8/mods/435 |
| FModel        | Extract assets from `.utoc`/`.ucas`/`.pak` for inspection + class reference. | https://fmodel.app/                            |
| UAssetGUI     | Low-level property edits of `.uasset` (no graph authoring). | https://github.com/atenfyr/UAssetGUI           |
| retoc         | Pack cooked `.uasset` into IoStore container (`.utoc`+`.ucas`). | https://github.com/trumank/retoc               |
| UnrealReZen   | Alternative IoStore packer.                         | https://github.com/rm-NoobInCoding/UnrealReZen |
| UnrealPak.exe | Pack into legacy `.pak` (works for LogicMods too, but IoStore preferred). | Ships with UE 5.2 editor                       |

Tekken modding community resources
----------------------------------
- [Nexus T8 tutorial #435](https://www.nexusmods.com/tekken8/mods/435) — has the umodel/glTF guide + project links.
- [tekkenmods.com](https://tekkenmods.com/) — primary mod host; examine existing UI mods (e.g. ExtraSettings #4021, Tranquility HUD #177) with FModel to see canonical Polaris widget references.
- [UE4SS BPModLoader docs](https://docs.ue4ss.com/feature-overview/blueprint-modloader.html).
- [Dmgvol IoStorePacking guide](https://github.com/Dmgvol/UE_Modding/blob/main/BasicModding/IoStorePacking.md).
- [Buckminsterfullerene02 UE-Modding-Tools](https://github.com/Buckminsterfullerene02/UE-Modding-Tools).

End-to-end steps (custom OpenDojo widget)
-----------------------------------------
1. **Install UE 5.2** via Epic Games Launcher (Versions tab → 5.2.x → Install).
2. **Acquire / build the Polaris-mirror project** (Nexus #435 Google Drive,
   or rebuild manually: use UE4SS to dump C++ headers via the
   `CXXHeaderDump` mechanism — already done at
   `E:\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\CXXHeaderDump\`
   — and recreate stub C++ classes for any Polaris classes our BP parents).
3. **Create new BP Actor** `/Game/Mods/OpenDojo/BP_OpenDojoLoader` (Actor
   subclass).
   - In `PreBeginPlay`: `CreateWidget(WBP_OpenDojoMenu)` → `AddToViewport`.
   - In `WBP_OpenDojoMenu` (UserWidget): canvas + text + button(s),
     bound to BP events for navigation/confirm/cancel.
4. **Cook**: Project Settings → Packaging → enable "Use Io Store" → Build →
   Cook Content For Windows. Outputs `cooked/Windows/.../*.uasset` + `.uexp`.
5. **Pack with retoc** (preferred for IoStore consumers like T8):
   ```
   retoc to-zen --version UE5_2 cooked\Windows OpenDojo_P.utoc
   ```
   or `UnrealReZen --engine-version GAME_UE5_1 cooked/Windows OpenDojo_P.pak`.
6. **Drop** `OpenDojo_P.pak` (+ `.utoc`/`.ucas`) at
   `Polaris/Content/Paks/LogicMods/OpenDojo/OpenDojo_P.pak`.
7. **Optional**: `LogicMods/OpenDojo/config.lua` to override the Actor class:
   ```lua
   Mods["OpenDojo"] = {
       AssetName = "BP_OpenDojoLoader_C",
       AssetPath = "/Game/Mods/OpenDojo/BP_OpenDojoLoader",
   }
   ```
8. Launch game. BPModLoader spawns `BP_OpenDojoLoader`, its `PreBeginPlay`
   creates the widget, widget appears. UE4SS Lua can hook the widget's BP
   events to wire OpenDojo's actual data layer.

What this gives us
------------------
- **Native UMG**: full Polaris styling, the InvalidationBox / Slate caching
  Just Works.
- **No runtime patching**: no UpdateData hook fights, no SetRawText overrides.
- **No err(...) loc problem**: BP can use `FText::FromString("OpenDojo")`
  directly in the editor, which produces a culture-invariant FText that
  Gryphon doesn't touch.
- **A persistent "OpenDojo" widget**: separate from the practice menu, can
  appear via hotkey, button hijack, or any trigger we wire.

What this DOESN'T give us
-------------------------
- Adding a row to the EXISTING `WBP_UI_PracticeMenu_C` left pane.
  Doing that natively would require either replacing Polaris's WBP outright
  (risky — we'd have to author a complete `_P.pak` that overrides the
  vanilla file, which breaks every game update), or some BP-modifier
  mechanism that doesn't exist for T8.
  → Insertion stays a runtime call to `AddButton1Data` + `UpdateListView1`.

Editor-free path?
-----------------
**Not viable for new BP authoring.** UAssetGUI/UAssetAPI can only edit
property bags of existing `.uasset` files. They cannot author a new BP
class with graph/event nodes. Hex-cloning an existing widget might work if
no new logic is needed; otherwise the UE 5.2 editor is unavoidable.

Recommended hybrid (current direction)
--------------------------------------
1. **Runtime path (now)**: UE4SS Lua — `AddButton1Data` + post-hook
   `UpdateData` + `SetRawText`. Lowest setup cost, fastest iteration.
2. **Canonical path (longer-term)**: separate `_P.pak` containing
   `BP_OpenDojoLoader` + `WBP_OpenDojoMenu`, loaded via BPModLoader. Use
   this for the actual OpenDojo menu UI (browser, edit, export, etc.).
   The existing practice menu integration remains runtime (AddButton1Data
   to add an "OpenDojo" entry that, when clicked, opens our widget).

This split keeps the runtime mod tiny and the heavy UI in proper UE5
assets — best of both worlds.
