Tekken 8 native menu integration — findings
============================================

Snapshot of everything learned while trying to insert an "OpenDojo" row
into the practice pause menu directly from the OpenDojo DLL (i.e. without
UE4SS). The attempt was eventually abandoned in favor of a hotkey-driven
ImGui menu; the row-insertion code lives only in git history. These notes
exist so a future attempt doesn't have to rediscover everything.

Target binary: `Polaris-Win64-Shipping.exe` (Tekken 8, UE 5.2.1). Preferred
base `0x140000000`. All RVAs below are relative to that.

────────────────────────────────────────────────────────────────────────
UE5 reflection primitives (pattern-pinned)
────────────────────────────────────────────────────────────────────────

`findUnrealClass` (= `FindObject<UClass>`)

    pattern : "45 33 C0 49 8B CF E8 ?? ?? ?? ?? 48 8B 4C 24 60"
    target  : RIP-relative at offset +7 from match
    sig     : void*(*)(UObject* outer, const wchar_t* name, bool exact_class)

`findUnrealObjectsOfClass` (= `GetObjectsOfClass`)

    pattern : "E8 ?? ?? ?? ?? 90 48 89 6C 24 30"
    target  : RIP-relative at offset +1 from match
    sig     : void(*)(UClass* cls, TArray<UObject*>* out,
                      bool include_derived,
                      uint32 exclude_object_flags,
                      uint32 exclude_internal_flags)

`UObject::ProcessEvent` — vtable slot **77** (byte offset `0x268`).
Verified by dumping vtable[0..100] of `Default__PolarisDialogFunctionLibrary`
CDO and decompiling each. Slot 77 had the canonical PE signature
(`FFrame` setup via `PTR_FUN_147a82b68`, parm-blob memcpy, property
walk, `FFrame::Invoke` tail).

FName pool — `module+0x9955480`. Layout:

    pool+0x10 = blocks array (8-byte pointers)
    FName.idx encoding:
        bits 16..28 (13 bits) = block index
        bits  0..15 (16 bits) = stride (entry byte offset / 2)
    Entry header (2 bytes):
        bit 0      = is_wide
        bits 6..15 = length in characters

GUObjectArray:

    pages array : module+0x99FB530
    count       : module+0x99FB544
    FUObjectItem layout (0x18 bytes):
        +0x00 UObject* Object
        +0x08 int32   Flags
        +0x0C int32   ClusterRootIndex
        +0x10 int32   SerialNumber
    page index = i >> 16, slot in page = i & 0xFFFF

────────────────────────────────────────────────────────────────────────
UE 5.2 layout offsets (verified empirically on Tekken 8)
────────────────────────────────────────────────────────────────────────

UObject:
    +0x08 ObjectFlags        (uint32)
    +0x10 ClassPrivate       (UClass*)
    +0x18 NamePrivate        (FName: 2x uint32)
    +0x20 Outer              (UObject*)

UStruct (parent of UClass / UFunction):
    +0x40 SuperStruct
    +0x48 Children           (UField*  — UFunctions only on UE 5.0+)
    +0x50 ChildProperties    (FField*  — FProperties added in UE 5.0)
    +0x70 PropertyLink       (FProperty* head, traversed via PropertyLinkNext)

UField:
    +0x28 Next               (UField*)

FField:
    +0x20 Next               (FField*)
    +0x28 NamePrivate        (FName)

FProperty:
    +0x4C Offset_Internal    (int32 — used to read property value off owner object)
    +0x58 PropertyLinkNext   (FProperty*)

UFunction:
    +0xD8 Func               (native fn pointer — patchable for per-function hooks)

────────────────────────────────────────────────────────────────────────
PracticeMenuImpl (kamui::ui)
────────────────────────────────────────────────────────────────────────

Singleton pointer at `module+0x9B7BC90`. Written by constructor
`FUN_145da5840`, cleared by destructor `FUN_145da7740`. Destructor fires
on practice-mode EXIT, NOT per pause-menu close — manager persists
across menu open/close cycles within one practice session.

Field layout:

    +0x31, +0x32  unknown flag bytes ("decide write guards" — must both
                  be 0 for FUN_145dac1e0 to write `decided_id`. May
                  explain why a poll-based click detector saw no
                  manager+0xC8 updates in practice.)
    +0x37         "fresh click" flag (set to 1 by FUN_145dac1e0 after
                  storing a decided id)
    +0xC8         last decided id (uint32) — written by FUN_145dac1e0
                  when one of the bound delegates fires
    +0x198        old constructed-widget pointer (cleared before each
                  reconstruct; freed via vtable[0])
    +0x1A8        TWeakObjectPtr<WBP_UI_PracticeMenu_C> — outer widget
                  (P1 side)
    +0x1B0        TWeakObjectPtr — S2 sibling
    +0x1B8        initialized byte (1 after FUN_145db2a30 completes)

Vtable[8] called with (manager, paneIndex) — observed paneIndex ∈ {0, 1, 2}
in the destructor's cleanup pass. Likely "close pane".

Vtable[16] (offset 0x80) — called from inside FUN_145dac1e0 right after
the decided-id write. Probably "dispatch decided action".

────────────────────────────────────────────────────────────────────────
Polaris functions (Tekken-specific, named via Ghidra static analysis)
────────────────────────────────────────────────────────────────────────

`FUN_145db2a30` — "load + construct practice menu". RVA `0x5DB2A30`.

    void FUN_145db2a30(PracticeMenuImpl* mgr, void* world_ctx);

    Loads BP class `/Game/UI/Widget/Practice/WBP_UI_PracticeMenu.WBP_UI_PracticeMenu_C`
    (and S2 sibling), CreateWidget via FUN_144842900, AddToViewport via
    FUN_14483fe70 (mode 0x80), SetVisibility (FUN_142e90970), then binds
    10 delegate slots at widget+0x290..+0x4D0 (stride 0x40). Sets
    mgr+0x1B8 = 1 at the end.

    KEY GOTCHA: hooked this function as the "menu open" notification.
    DOES NOT FIRE PER OPEN. Only fires when the BP class is first
    constructed — generally at game startup before our DLL is fully
    initialized. Subsequent menu opens probably reuse the cached widget
    instance. THIS WAS NOT THE RIGHT HOOK TARGET.

`FUN_145dac190` (RVA `0x5DAC190`) — small predicate used inside one of
the delegate-binder thunks. Contains an unresolved jumptable.

`FUN_145dac1e0` (RVA `0x5DAC1E0`) — the bound-delegate body for slot **6**
(widget+0x410), NOT slot 0 (decide).

    if (mgr->flag31 == 0 && mgr->flag32 == 0) {
        mgr->decided_id = *param2;     // +0xC8 = id
        mgr->vtable[16](mgr, 0);       // dispatch
        mgr->fresh_click_flag = 1;     // +0x37 = 1
    }

    We initially hooked manager+0xC8 polling thinking this was the
    decide handler. It isn't — slot 0's binder target is `LAB_145dac1c0`,
    a code label inside `FUN_145dac190` that Ghidra couldn't decompose
    cleanly. The polling never saw +0xC8 update because the guard
    flags blocked the write, OR because the actual decide path uses
    a different slot.

`FUN_142e5ef30` — "InvokeDecideButton1Callback" C++ impl. RVA `0x2E5EF30`.

    void __fastcall FUN_142e5ef30(WBP_UI_PracticeMenu_C* widget,
                                  uint32 id);

    Reads widget+0x290 (the bound decide delegate ptr) and invokes it
    with id. Called by the BP exec stub for InvokeDecideButton1Callback
    (which is `FUN_142c17380`, the named-function table entry at
    `module+0x1479b9520`).

    THIS IS THE RIGHT HOOK FOR CLICK INTERCEPTION. Clean (rcx=widget,
    edx=id) signature; fires on every row decide; Polaris-internal so
    no UE4SS / ProcessEvent conflict. We successfully MinHook'd this
    and routed clicks to `toggle_menu()`.

`FUN_145da5840` (RVA `0x5DA5840`) — `PracticeMenuImpl` constructor. Sets
mgr->vtable to `&PTR_FUN_14855d388`, zero-initializes fields, sets the
singleton at `module+0x9B7BC90`.

`FUN_145da7740` (RVA `0x5DA7740`) — `PracticeMenuImpl` destructor. Clears
singleton, calls `vtable[8](mgr, paneIdx, 0)` for panes {0,1,2}, frees
all subobjects. Fires on practice-mode EXIT only.

────────────────────────────────────────────────────────────────────────
BP classes + UFunctions
────────────────────────────────────────────────────────────────────────

BP-generated UClasses (loaded only after practice scene initialization):

    WBP_UI_PracticeMenu_C        — outer menu widget (~17 default rows)
    WBP_UI_PracticeMenu_S2_C     — P2 sibling for split-screen
    WBP_UI_PracticeMenu_Button_1_C — single row widget
    BP_PracticeMenu_Button_1_Item_C — data item bound to each row

Engine classes (always loaded):

    /Script/CoreUObject.Object
    /Script/UMG.UserWidget
    /Script/UMG.ListView
    /Script/UMG.Widget
    /Script/Polaris.PolarisTextBlock
    /Script/Polaris.PolarisRichTextBlock
    /Script/Polaris.PolarisEditableText
    /Script/Polaris.PolarisEditableTextBox
    /Script/Polaris.PolarisDialogFunctionLibrary

UFunctions on `WBP_UI_PracticeMenu_C` (BP-defined — Func = ProcessInternal):

    AddButton1Data(FString label, bool enable)  — append item to ListView_1
    UpdateListView1()                           — rebuild row widgets
    OnDecideButton1(int32 id)                   — fires when row is confirmed
    InvokeDecideButton1Callback(int32 id)       — calls FUN_142e5ef30
    InvokeSelectButton1Callback(int32 id)       — selection change
    ClearButton1Data()
    SetCursorButton1(int32 id)
    (parallel set for Button2/3/4 — pane index)

Engine UFunctions used by us:

    UPolarisTextBlock::SetRawText(FString text, bool ReplaceUnsupportedChar)
        Writes FText literal; bypasses Gryphon localization. Verified
        in `Polaris.hpp` (CXXHeaderDump line 12351).
    UPolarisTextBlock::SetTextID(FString text_id)
        Hook-target: re-apply SetRawText post-call to defeat Gryphon
        wrapping. Lua mod's working pattern.
    UUserWidget::RemoveFromParent
        Candidate close-event hook — fires when BP unparents the widget.
        Not confirmed whether Tekken actually uses this for pause-menu
        close vs. just SetVisibility(Collapsed). NEEDS VERIFICATION if
        this attempt is resumed.
    UListView::ListItems  (FProperty)
        TArray<UObject*> — the visible list. Length = item count.

FProperty offsets (verified at runtime):

    on WBP_UI_PracticeMenu_Button_1_C:
        list_item     : offset 976  (UObject* — BP_PracticeMenu_Button_1_Item_C)
        TB_Menu_OFF   : offset 960  (UPolarisTextBlock* — unhovered label)
        TB_Menu_ON    : offset 968  (UPolarisTextBlock* — hovered label)
    on BP_PracticeMenu_Button_1_Item_C:
        Text          : offset 56   (FString — item label)
    on UListView (engine):
        ListItems     : offset 2976 (TArray<UObject*>)

Resolution note: my `find_fproperty_offset` walks UStruct::ChildProperties
at +0x50 first, then falls back to PropertyLink at +0x70. For
`WBP_UI_PracticeMenu_C` the `ListView_1` FProperty was NOT found in
either chain on this build — reason undetermined. The row class
properties all resolved cleanly via ChildProperties. Workaround:
discover `cls_item` from a live row's `list_item.ClassPrivate` instead
of reading ListView_1, then count live `cls_item` instances as a proxy
for `ListView_1.ListItems.Num`.

────────────────────────────────────────────────────────────────────────
Widget layout (`WBP_UI_PracticeMenu_C` instance)
────────────────────────────────────────────────────────────────────────

    +0x228  WidgetTree (UWidgetTree*)
    +0x280  input flag byte (set to 0 by FUN_145db2a30)
    +0x282  input flag byte (set to 1 by FUN_145db2a30)
    +0x290  ┐  10 delegate-binder slots, stride 0x40:
    +0x2D0  │     slot 0 @ +0x290 → LAB_145dac1c0 (decide)
    +0x310  │     slot 1 @ +0x2D0 → LAB_145dac090
    +0x350  │     slot 2 @ +0x310 → LAB_145dabf90
    +0x390  │     slot 3 @ +0x350 → LAB_145dac030
    +0x3D0  │     slot 4 @ +0x390 → LAB_145dac220
    +0x410  │     slot 5 @ +0x3D0 → LAB_145dac2f0
    +0x450  │     slot 6 @ +0x410 → LAB_145dac1d0 (handler = FUN_145dac1e0)
    +0x490  │     slot 7 @ +0x450 → LAB_145dabfe0
    +0x4D0  ┘     slot 8 @ +0x490 → LAB_145dac230
                  slot 9 @ +0x4D0 → LAB_145dac080

Each binder is 0x40 bytes. Layout (std::function-like):

    +0x00  fn pointer        (one of LAB_145dacXXX)
    +0x08  ?
    +0x10  ?  (alternate receiver — non-null overrides default)
    +0x18  ?
    +0x20  vtable            (= &PTR_FUN_146ec6318 — shared by all slots)
    +0x28  receiver ptr      (= PracticeMenuImpl* manager)
    +0x30  ?
    +0x38  ?

────────────────────────────────────────────────────────────────────────
Gryphon localization system
────────────────────────────────────────────────────────────────────────

Bandai-internal Polaris-specific localization layer. All UI text routes
through it; `AddButton1Data` puts our label through `GetText` which
returns "err(\<key\>) @@@@" for unknown keys. The literal "err({0}) @@@@"
template lives at `0x148544120`; err FText built at `0x145b99510`.

`UGryphonFunctionLibrary` API (`/Script/Polaris.GryphonFunctionLibrary`):

    HasText(textId)
    GetText(textId)
    GetString(textId)
    RegisterAsset(category, asset)
    UnregisterAsset(category)

No `AddEntry` / runtime insert — text IDs are baked into a
`UGryphonTextBinaryAsset`. We can't add "OpenDojo" as a known key.

**Bypass**: `UPolarisTextBlock::SetRawText(FString, bool)`. Writes
the FText directly without going through Gryphon. Each call to BP's
`UpdateData` on a row re-runs the Gryphon lookup → our row's label
flickers back to "err()". Workaround: hook
`UPolarisTextBlock::SetTextID` post-call (UFunction.Func patch); when
self matches our captured TB pointers, re-apply SetRawText. Lua mod
uses RegisterHook on `/Script/Polaris.PolarisTextBlock:SetTextID`.

────────────────────────────────────────────────────────────────────────
Hooking ProcessEvent — UE4SS conflict
────────────────────────────────────────────────────────────────────────

If UE4SS is loaded, it has already MinHook'd `UObject::ProcessEvent`.
Our `MH_CreateHook` on the same function address fails with
`MH_ERROR_ALREADY_CREATED` (status 3). Workarounds:

  1. Skip the global PE hook. Patch the specific BP UFunction's
     `Func` pointer at `UFunction+0xD8` directly. ProcessEvent reads
     this slot when dispatching — patching it intercepts only that
     specific UFunction. No collision because we never touch the
     global PE entry point. This is the approach that worked for
     SetTextID + UpdateData + UpdateListView1 hooks.

  2. If UE4SS is not loaded, the vtable slot 77 of any UObject points
     directly to UE's PE. Read `*(void***)obj[77]` and MinHook that.
     Filter shim by `function->Name` to limit work.

────────────────────────────────────────────────────────────────────────
Approaches tried, in order, and why they failed
────────────────────────────────────────────────────────────────────────

1. **Lua mod (`Mods/OpenDojoMenu/Scripts/main.lua`)** — `AddButton1Data` +
   `UpdateListView1`, `SetTextID` post-hook, `OnDecideButton1` pre-hook.
   WORKED end-to-end. User accepted the proof of concept but wanted
   the work to live in the DLL with no UE4SS dependency.

2. **DLL port via per-frame `find_first_live_object_of_class` polling.**
   Worked but lagged the game from 60→30fps because `GetObjectsOfClass`
   was called per-frame against UE's per-class hash table.

3. **MinHook on `FUN_145db2a30`** as "menu open" notification. Did not
   fire — function only runs on first BP class load, not per pause-menu
   open. The widget is constructed once and reused.

4. **MinHook on `UObject::ProcessEvent` to intercept `OnDecideButton1`.**
   Collided with UE4SS's pre-existing hook → `MH_ERROR_ALREADY_CREATED`.

5. **Poll `manager+0xC8` for decided-id changes.** Never observed any
   change. Either the guard flags at `manager+0x31/0x32` blocked the
   write, or the path I was watching is for a non-decide event.

6. **MinHook on `FUN_142e5ef30` (InvokeDecide dispatcher)** — Worked!
   Fired on every click with `(self, id)` cleanly. Required a 250ms
   debounce because BP fires the event multiple times per physical
   click. This was the final click-intercept design.

7. **Hook `UpdateListView1.Func` to re-insert per menu show.** Required
   a `thread_local` recursion guard for our internal `UpdateListView1`
   call. Worked logically but inherited all the timing fragility — e.g.
   if `cls_item` wasn't resolved yet when first `UpdateListView1` fired,
   we couldn't compute `our_row_idx`.

8. **Direct `ListItems` TArray manipulation** to shift OpenDojo from
   end-of-list to index 1 (after Practice Settings, so user could reach
   it without scrolling past ~14 offscreen items). Caused UE state
   corruption inside `UpdateListView1` — game crashed shortly after
   the shift. UListView keeps internal mirrors of `ListItems` that
   weren't updated by our raw memmove.

9. **Hook `UpdateData.Func` on the row class.** Worked — captured TBs
   per row state change without polling. Lua-parity pattern. Required
   deferring SetTextID hook install until AFTER first UpdateData
   succeeded, else partially-initialized rows crashed BP.

10. **Spawn install worker on DllMain.** Game wouldn't boot — find_class
    raced with UE's GUObjectArray initialization → deadlocked the
    main thread against the UObject table lock. Fix: defer worker
    spawn until render_hook reports ImGui init success (i.e. ~60+
    frames into the game render loop, well past UE init).

────────────────────────────────────────────────────────────────────────
If anyone tries again, here's the order
────────────────────────────────────────────────────────────────────────

1. Spawn install worker thread, but only after render_hook reports
   ImGui ready. NEVER touch UE reflection from DllMain.

2. Engine resolve: pattern-pin `findUnrealClass` + `findObjectsOfClass`,
   resolve engine classes (UserWidget, ListView, PolarisTextBlock),
   resolve `SetRawText` / `SetTextID` UFunctions, `ListItems` FProperty
   offset on UListView.

3. BP resolve loop (100 ms cadence): `find_class_by_name` on
   "WBP_UI_PracticeMenu_C" and "WBP_UI_PracticeMenu_Button_1_C" via
   UUserWidget-derived enumeration. Resolve UFunctions on each (Add /
   Update / OnDecide / UpdateData). Resolve row-class FProperty
   offsets (list_item, TB_Menu_OFF, TB_Menu_ON). Discover `cls_item`
   via `read u64 at row+off_list_item, then class @ +0x10`. Resolve
   item-class `Text` FProperty offset. Loop until everything resolves
   OR a generous timeout — `cls_item` only becomes available after the
   user has opened the practice pause menu once.

4. Install hooks (UFunction.Func patches at +0xD8):
     - `UpdateListView1` (recursion-guarded): on entry, walk live
       `cls_item` instances; if none has `Text == "OpenDojo"`, count
       and call `AddButton1Data("OpenDojo", true)`. Compute
       `our_row_idx = pre_insert_count`. Call orig.
     - `UpdateData` on row class: call orig first; if
       `self.list_item.Text == "OpenDojo"`, capture
       `TB_Menu_OFF/ON`; immediately `SetRawText("OpenDojo")`;
       install `SetTextID` hook on first capture.
     - `SetTextID` on UPolarisTextBlock: call orig; if self ∈
       captured TBs, re-apply `SetRawText("OpenDojo")`.
     - `RemoveFromParent` on UUserWidget: filter by self ==
       menu widget → hide ImGui. (UNVERIFIED — may not fire on
       pause-menu close. Confirm before relying on it.)
     - `FUN_142e5ef30` MinHook: filter by `(self, id) == (menu, our_row_idx)`,
       debounce 250ms, toggle ImGui.

5. **Open question** still: how to move the row to index 1 without
   corrupting UListView state. Approaches not yet tried:
     - Hook AddButton1Data, on game-side calls let through; on the
       final UpdateListView1 of menu setup, call AddButton1Data
       ourselves and then ClearButton1Data + replay the original 17
       items with OpenDojo inserted at index 1 in the replay order.
     - Patch UListView's internal index↔item map via reflection
       (PropertyLink walk to find pool/visible/etc fields).
     - Hook UListView's RegenerateAllEntries and pre-process the
       order there.

6. Find a true "pause menu close" hook target. Candidates:
     - `RemoveFromParent` (UUserWidget UFunction). Test whether
       Tekken calls this or just SetVisibility(Collapsed).
     - `manager+0x1B8` 1→0 transition — but byte may never clear
       within one practice session.
     - PracticeMenuImpl `vtable[8]` (close-pane handler). Fires
       once per pane in destructor; may also fire on pause-menu
       close. Worth disassembling its body.

────────────────────────────────────────────────────────────────────────
File pointers (where the code lived)
────────────────────────────────────────────────────────────────────────

In git history (deleted from main):

    dll/src/practice_menu.{cpp,hpp}    — final-attempt port
    dll/src/native_menu.{cpp,hpp}      — original Ghidra exploration
    dll/src/dialog.{cpp,hpp}           — UPolarisDialogFunctionLibrary
                                         CDO probe + ProcessEvent
                                         dispatch helpers
    dll/src/drill_browser.{cpp,hpp}    — abandoned BP-widget browser

Still in the tree:

    docs/NATIVE_MENU_DESIGN.md          — pre-deletion design doc
    docs/OPENDOJO_DRILL_BROWSER_WIDGET.md
    docs/OPENDOJO_MENU_FINDINGS.md
    docs/OPENDOJO_CUSTOM_MENU.md
    docs/DLL_PIVOT_PLAN.md
    docs/findings_native_menu_widget_dump.log  — runtime widget-tree dump
    Mods/OpenDojoMenu/Scripts/main.lua  — Lua proof of concept (disabled)

Memory entries (`~/.claude/memory/`):

    project_tekken_dialog_api.md
    project_tekken_processevent_primitives.md
    project_tekken_fname_pool.md
    project_tekken_native_menu_blockers.md
    project_tekken_practice_manager.md
    project_tekken_textmenu_api.md
    project_tekken_ui_architecture.md
    project_opendojo_v1_imgui_strategy.md
    feedback_opendojo_practice_vs_replay_managers.md
