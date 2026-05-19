OpenDojo — styling the ImGui menu with Tekken assets
=====================================================

OpenDojo's menu is rendered with Dear ImGui. By default it uses ImGui's
bundled `ProggyClean` bitmap font + a hand-picked dark/crimson palette
that approximates Tekken 8's UI (`dll/src/theme.cpp`). To get closer to
the native look, drop an extracted Tekken UI font into the mod's asset
folder and the DLL will pick it up at next launch.

Font swap (drop-in)
-------------------

1. Extract a TrueType font from Tekken's `.pak` archives — see
   "Extracting assets" below.
2. Save it as `opendojo.ttf` under:

       <game>/Polaris/Binaries/Win64/Mods/OpenDojo/font/opendojo.ttf

3. Relaunch the game. The DLL's `try_load_custom_font` finds the file at
   ImGui init time and uses it as the menu's default font (18 px).
   If the file is missing or fails to parse, the menu silently falls back
   to ProggyClean — check `opendojo.log` for either:

       render_hook: loaded custom font <path>
       render_hook: AddFontFromFileTTF failed for <path> — falling back

Extracting assets
-----------------

Tekken 8's assets live in IoStore archives under
`<game>/Polaris/Content/Paks/`. Two paths to extract:

### Path A — FModel (GUI, easiest)

1. Open `FModel.exe` (already at `C:/Users/ethan/Desktop/tekken_mod/FModel.exe`).
2. Settings → set the game directory to `<game>/Polaris/`. Use the
   bundled usmap (`T8_mapping_01_28_2026.usmap`) for type info.
3. Browse `pakchunk0-Windows` → `Polaris/Content/UI/Font/` (path varies
   per build; search for "Font").
4. Right-click a `.uasset` font face → "Save Properties (.props)" to
   inspect, or "Export Raw Data" if it embeds the TTF.
5. UE5 font assets typically reference a `FontFace` asset whose
   `LoadingPolicy=Inline` payload is the TTF bytes — that's the file
   you want.

### Path B — retoc (CLI, scriptable)

1. Get retoc (https://github.com/trumank/retoc) — pinned releases on
   GitHub. Place `retoc.exe` somewhere on PATH.
2. Bulk-extract Polaris IoStore archives:

       retoc to-legacy --aes-key <key> <game>/Polaris/Content/Paks/ extracted/

   (Tekken's pak key is in the AES-key list shipped with FModel.)
3. Search the extracted tree for `*.uasset` font assets — same files
   FModel exposes, just in a flat directory layout.

### Verifying the extracted TTF

`opendojo.ttf` should be a vanilla TrueType file (starts with bytes
`00 01 00 00` or `OTTO`). If it starts with UE-asset magic (e.g.
`C1 83 2A 9E`) you grabbed the raw .uasset, not the inlined font
payload — re-extract from the FontFace sub-object's `Inline` payload.

Theme tuning
------------

The palette + spacing is in `dll/src/theme.cpp`. Tekken's accent is a
deep crimson — `ACCENT_R/G/B` in `dll/src/theme.hpp`. Window/frame
rounding is 0 to match Tekken's angular UI.

If you want to fully re-skin (button textures, panel backgrounds), the
ImGui DX12 backend can sample arbitrary `ID3D12Resource` textures. Load
the extracted .uasset's texture payload through D3D12 the same way the
ImGui font atlas is uploaded (see `imgui_impl_dx12.cpp`'s font upload
path). Out of scope for v1.

Open questions for future passes
--------------------------------

- Exact Tekken UI font name + which .pakchunk holds it (FModel can tell
  us — needs a UI session to confirm visually).
- Whether the practice menu uses a different font from main-menu text
  (extracted props files include `FontObject` references).
- Controller-glyph icon textures for a button-prompt strip in the menu
  footer.
