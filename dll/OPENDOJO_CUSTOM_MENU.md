OpenDojo custom-menu strategy
=============================

Date: 2026-05-17
Phase: prototype menu UI when user clicks the "OpenDojo" row.

TL;DR
-----
**Use `UPolarisDialogFunctionLibrary` (a BPFunctionLibrary static) to drive a
native Polaris dialog as the v1 OpenDojo menu.** It is fully styled, animates
correctly, has its own canvas parent, supports raw-text (no Gryphon), and
exposes a polling API that sidesteps UE4SS delegate-binding headaches and
UE4SS-3.0.1 BPFunctionLibrary-hook freezes (#467 — *hooks* freeze; *calls*
are fine).

The whole menu can be a single Lua function: hook the practice menu's
per-instance `OnDecideButton1(int32 ID)`, if `ID == our_row_idx` call
`OpenDialog(...)`, then `LoopAsync` poll `IsDialogDecided()` and dispatch by
`GetDialogCursor()`.

Why not the canonical UE-editor `_P.pak` path?
----------------------------------------------
Documented in CANONICAL_MOD_PATH.md as the longer-term goal, but:
- ~6–10 hours setup + UE 5.2 editor + ~120 GB disk.
- 30-min iterate cycle (cook + pack + drop + launch).
- Overkill for "click → see something" milestone.
- We can still migrate later for richer UI (grids, scroll panes, custom
  art) by replacing the `OpenDialog` call with `CreateWidget(WBP_OpenDojoMenu)`
  once the editor-built widget exists.

Phase-1 dialog API (verified statically — Polaris.hpp:11275)
------------------------------------------------------------
```
class UPolarisDialogFunctionLibrary : public UBlueprintFunctionLibrary {
    void OpenDialog(FString Description, int32 defaultCursor,
                    const TArray<FPolarisDialogButtonParam>& Params,
                    bool IsTextId, int32 display_side);
    void CloseDialog();
    bool IsDialogDecided();
    bool IsDialogClosed();
    int32 GetDialogCursor();
    void SetDialogDescription(FString Text, bool is_text_id);
    void SetDialogButtonText(int32 Index, FString Text, bool is_text_id);
    void SetDialogButtonEnable(int32 Index, bool flag);
    void SetDialogCursor(int32 Index);
    void SetDialogCancelEnable(bool flag);
    void SetDialogCloseAsCancel(bool flag);
    // + OpenDialogEx (with OnOpening delegate)
    // + OpenDialogEx2 (with OnOpening + OnCancel delegates + more flags)
};

struct FPolarisDialogButtonParam {  // size 0x28
    FString  Text;                              // 0x00
    bool     isEnable;                          // 0x10
    FPolarisDialogButtonParamOnDecide OnDecide; // 0x14  (size 0x10)
    bool     IsTextId;                          // 0x24
    bool     IsGhost;                           // 0x25
};
```

The underlying widget is `UWBP_UI_Dialog_C : UPolarisUMGDialog`
(WBP_UI_Dialog.hpp). It has its own `CanvasWindow` (UCanvasPanel) and animation
suite — solving the "widget without parent CanvasPanel doesn't render" issue
([[feedback_opendojo_widget_parent_required]]).

Lua call sketch
---------------
```lua
local DialogLib = StaticFindObject(
    "/Script/Polaris.Default__PolarisDialogFunctionLibrary")
-- or look up the BPFL class and use its CDO

local function open_opendojo_menu()
    local params = TArray<FPolarisDialogButtonParam>{
        {Text="Export current recordings", isEnable=true,  IsTextId=false},
        {Text="Import from clipboard",     isEnable=true,  IsTextId=false},
        {Text="Browse drills (TBD)",       isEnable=false, IsTextId=false},
        {Text="Close",                     isEnable=true,  IsTextId=false},
    }
    DialogLib:OpenDialog("OpenDojo", 0, params, false, 0)

    LoopAsync(50, function()
        if DialogLib:IsDialogDecided() then
            local cursor = DialogLib:GetDialogCursor()
            DialogLib:CloseDialog()
            if cursor == 0 then export_drill()
            elseif cursor == 1 then import_drill()
            elseif cursor == 2 then -- disabled
            elseif cursor == 3 then -- close, no-op
            end
            return true
        end
        if DialogLib:IsDialogClosed() then return true end
        return false
    end)
end
```

Wire-up: hook `OnDecideButton1` on the practice menu
----------------------------------------------------
`UWBP_UI_PracticeMenu_C::OnDecideButton1(int32 ID)` is the BP UFunction the
menu calls when the user confirms a row in pane 1 (WBP_UI_PracticeMenu.hpp:57).
Per-instance hooks work fine on UE4SS 3.0.1 (we already use this pattern for
SetTextID), so:

1. On the same F4 path where we capture TB addresses, also capture the menu
   instance, its `ListView_1` count *before* `AddButton1Data`, and remember
   `our_row_idx = pre_count` (the new row's ID equals the index it was
   appended at; pane is appended 0..N-1 with no gaps).
2. RegisterHook on `/Game/UI/Widget/Practice/WBP_UI_PracticeMenu.WBP_UI_PracticeMenu_C:OnDecideButton1`.
3. In pre-hook: read the `ID` param. If `ID == our_row_idx`, call
   `open_opendojo_menu()`. No need to suppress the original — ID 17 has no
   binding in the existing switch and falls through to default (likely no-op).
   If we observe an unwanted side effect, switch to suppression via
   `:set(nil)` on the return wrapper or use a `Context:SkipFunction()` if
   exposed (verify at runtime).

If pre-hook suppression turns out unavailable, the LoopAsync approach
still works: open the dialog in the post-hook — practice menu's default
handler for an out-of-range ID does nothing, so the dialog just stacks on
top.

Risks / open items
------------------
- **CDO discovery**: need to confirm `StaticFindObject` returns a usable
  CDO for `UPolarisDialogFunctionLibrary`. Alternative: find a runtime BP
  caller and read the function dispatch path. Either way, low risk —
  BPFunctionLibrary static calls via UE4SS Lua are well-trodden.
- **Display-side**: `display_side` (0/1/2) controls which monitor pane the
  dialog renders in. `0` is the default center; if it overlaps the practice
  menu poorly, pick another. Cheap to retry.
- **Cancel/close semantics**: `IsDialogClosed()` may fire on ESC/B-button.
  Handle both decided + closed in the poll loop.
- **Re-entry**: don't call `OpenDialog` while the previous instance is still
  open. Guard with a Lua bool `g_dialog_open`.
- **Practice menu re-render on pause**: if Tekken's pause-menu refreshes
  while the dialog is up, layering issues are possible. Display-side and
  the dialog's own animation suite likely handle this, but worth a sanity
  test on first run.

Next steps
----------
- Implement the hook + OpenDialog call in `main.lua` behind the existing
  F4 capture path.
- Verify ID mapping (our_row_idx) on first runtime test.
- Once the click → dialog flow works, replace placeholder button labels
  with the real OpenDojo actions: Export → write JSON to clipboard,
  Import → read JSON from clipboard, etc.
- Longer-term: when the menu needs more than a few buttons, migrate
  Description-as-body and Buttons-as-list to a custom WBP via
  CANONICAL_MOD_PATH.md (still spawned by clicking the OpenDojo row, just
  via `CreateWidget` instead of `OpenDialog`).
