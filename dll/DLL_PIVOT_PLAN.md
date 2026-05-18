DLL pivot — Tekken 8 OpenDojo native menu via Polaris dialog primitive
======================================================================

Date: 2026-05-17
Trigger: UE4SS Lua approach hit two walls in the same session —
1. F9 (`OpenDialog` smoke test) failed with `<fn raised>` (almost certainly
   UE4SS 3.0.1 botching the Lua-table → `TArray<FPolarisDialogButtonParam>`
   marshaling — that TArray nests an FString + a dynamic delegate).
2. F4 froze the game after adding the per-class `OnDecideButton1` hook
   (regression vs. last working version that only did SetTextID).

Both are Lua/UE4SS infrastructure limits — not bugs in our design.
The dialog primitive itself is sound (statically verified, Polaris.hpp:11275).
Pivot to DLL eliminates both walls.

Current state
-------------
- **Lua mod**: reverted F4 to the previously-working version (row insert +
  SetTextID persistence). Dead helpers (`open_opendojo_menu`,
  `find_dialog_lib`, `install_decide_hook`, plus globals) remain in the
  file but are unreachable on F4. F9 still bound, still fails harmlessly.
- **DLL**: `dll/src/native_menu.cpp` has substantial UE5 plumbing already:
  `findUnrealClass`, `findUnrealObjectsOfClass` (GetObjectsOfClass),
  GUObjectArray pages walker, FName decoder, UClass/UFunction layout
  decoded for UE 5.2, CreateWidget RVA pinned. **What's missing: a
  ProcessEvent caller.** That's the single primitive we need to add.

Why we need ProcessEvent and not just C-call the static
-------------------------------------------------------
`UPolarisDialogFunctionLibrary::OpenDialog` is a BPFunctionLibrary static.
At the binary level there are two entry points:
- The C++ static itself (where the work happens).
- An auto-generated thunk `execOpenDialog` that unpacks args from the BP
  stack and calls the C++ static.

We *could* skip ProcessEvent by finding the C++ static directly and
calling it as a normal function. But:
- The function-name string `"OpenDialog"` only lets us locate the
  UFunction registration; mapping that to the C++ static address requires
  decompiling the exec thunk first (manual).
- ProcessEvent works for *any* UFunction with a flat arg-blob — once
  we have it, we get OpenDialog, IsDialogDecided, GetDialogCursor,
  CloseDialog, AddButton1Data, UpdateListView1, and every other
  reflected method "for free."
- It's also the standard hook surface for intercepting OnDecideButton1
  (the click callback).

So ProcessEvent is the right investment.

Tasks (in build order)
----------------------
1. **Find ProcessEvent.** Two paths:
   - **(A) Signature scan.** UE5.2 ProcessEvent has stable prologue +
     internal references. Easiest static lead: the "Script call stack:\n"
     string at `0x147a82d08` is referenced from `FUN_14317a9c0`
     (a GetStackTrace wrapper). ProcessEvent is one or two CALL hops
     upstream. Decompile and trace.
   - **(B) Vtable slot.** UObject's vtable has ProcessEvent at a fixed
     slot (typically 67 / offset 0x218 in UE 5.2). Resolve at runtime
     from any UObject pointer. Quick to wire; one-time runtime dump
     confirms the slot.

   Recommended: (B) first to get unstuck, then (A) later for robustness
   against game updates.

2. **FString builder.** UE5 layout: `{ wchar_t* Data, int32 Num, int32 Max }`.
   Allocate `Data` from process heap (GMalloc), copy UTF-16, null-term,
   Num = Max = wcslen+1.

3. **TArray<T> builder.** `{ T* Data, int32 Num, int32 Max }`. Same
   GMalloc pattern. For `FPolarisDialogButtonParam` the inner FString
   needs its own allocation.

4. **Args struct for OpenDialog.** Lay out by hand:
   ```
   struct OpenDialog_Args {
       FString Description;                       // 0x00 (16 bytes)
       int32   defaultCursor;                     // 0x10
       // 4-byte pad
       TArray<FPolarisDialogButtonParam> Params;  // 0x18 (16 bytes)
       bool    IsTextId;                          // 0x28
       // 3-byte pad
       int32   display_side;                      // 0x2C
   };
   ```
   (Exact alignment to verify from `UFunction::PropertiesSize`.)

5. **CDO lookup.** Already wired (`findUnrealClass` on
   `"PolarisDialogFunctionLibrary"`), then read `UClass +
   ClassDefaultObject` offset (UE 5.2: +0x150).

6. **UFunction lookup.** Walk `UClass::Children` chain (already wired
   via the existing `dump_uclass_functions` path in `native_menu.cpp`)
   filtering by FName == "OpenDialog".

7. **Call ProcessEvent.** `cdo->ProcessEvent(openDialogUFunction, &args)`.

8. **Phase-1 deliverable.** Bind to an existing hotkey (F11 free?) —
   on press, open a single-button dialog with raw text "OpenDojo —
   Close". If it renders, primitive is proven.

9. **Phase-2.** Replace Lua F4: from DLL, find live
   `WBP_UI_PracticeMenu_C`, ProcessEvent `AddButton1Data("OpenDojo", true)`
   + `UpdateListView1`, capture the row index.

10. **Phase-3.** Hook ProcessEvent globally (MinHook, already a
    plausible dep in this build). Filter by `Function->Name == "OnDecideButton1"`
    and `Object->Class == WBP_UI_PracticeMenu_C`; if `*(int32*)Parms == our_row_idx`,
    fire `OpenDialog`. Original handler runs as-is (no suppression).

11. **Retire Lua mod.** Delete `OpenDojoMenu/Scripts/main.lua` and
    UE4SS dep. v1 of OpenDojo native menu lives wholly in the DLL.

Risks / decisions to make next session
--------------------------------------
- **GMalloc address**: UE5's global allocator. We need it for FString/TArray
  heap allocs. Sig-scannable; OR we can use UE's exposed `FMemory::Malloc`
  via signature.
- **Padding inside `OpenDialog_Args`**: UE5 packs based on
  `UFunction::PropertiesSize`. Worth a one-time sanity dump (the existing
  native_menu.cpp already walks UFunction children to log offsets).
- **MinHook dep**: not in CMakeLists currently; add as a fetched
  dependency. Cheap.
- **Phase-3 ProcessEvent hook is risky**: the hook runs on every script
  call (thousands/sec). Filtering must be ultra-tight to avoid perf hit.
  Consider hooking `Function->Func` swap instead (per-UFunction trampoline)
  — narrower surface, requires UFunction immutability assumptions.

Next-session entry point
------------------------
Open this file. Decide Step-1 path (B is faster). Add `dialog.hpp/.cpp`
under `dll/src/`. Wire a hotkey in `hotkeys.cpp`. Build, deploy, test
single-button dialog. If it renders, the rest is mechanical.
