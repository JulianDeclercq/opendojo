#pragma once

// Phase-1 of the DLL pivot from the UE4SS Lua mod (DLL_PIVOT_PLAN.md).
//
// Goal: render a single Polaris dialog via
// UPolarisDialogFunctionLibrary::OpenDialog, invoked from our F12 ImGui
// menu. Once visible, layers 2+3 of the plan (row insert, OnDecideButton1
// hook) come back in scope.
//
// Two diagnostics are exposed before the dialog itself, since the central
// open question is which UObject vtable slot ProcessEvent occupies in
// this build of Polaris. UE 5.2 reference slot is 67 (offset 0x218), but
// builds vary; one runtime dump nails it.

namespace opendojo::dialog {

// One-time resolution. Looks up the CDO + UFunctions on first call.
// Cheap to call again — caches internally. Returns true if everything
// resolved (CDO, OpenDialog UFunction, IsDialogDecided UFunction,
// GetDialogCursor UFunction, CloseDialog UFunction).
bool ensure_resolved();

// Dump the first 100 entries of the resolved CDO's vtable to opendojo.log.
// Each line is the slot index, slot offset, absolute address, and RVA
// relative to the Polaris module. Use this to identify which slot is
// ProcessEvent — typically the one whose RVA decompiles in Ghidra to a
// function that touches FFrame, an EX_ opcode table, or "Script call stack".
void dump_pe_vtable();

// Open a single-button test dialog with the given raw text. Bypasses
// Gryphon (IsTextId=false), polls IsDialogDecided() / IsDialogClosed()
// from the render-thread tick, and closes itself on user input.
//
// Returns false on resolution failure (logs why); true if the OpenDialog
// call dispatched without exception.
bool open_test_dialog(const char* description, const char* button_text);

// Read the current dialog-manager-singleton pointer and log it. Useful
// after entering different game states to see when it gets populated.
void log_dialog_manager_state();

// Force-construct the dialog manager via Polaris's own factory. Use ONLY
// if the singleton is null and you've confirmed natural init paths (pause
// menu open, etc.) don't fire. Risk: the constructor may depend on UE
// systems not initialized in our context. Logged and SEH-guarded.
void force_init_dialog_manager();

// Per-frame tick — drives the close-on-decided polling loop. Cheap when
// no dialog is open. Wire into the existing render_hook tick.
void tick();

}  // namespace opendojo::dialog
