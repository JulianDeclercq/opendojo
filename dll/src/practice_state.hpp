#pragma once

// Practice-mode lifecycle tracking.
//
// Instead of polling the gameplay subsystem every frame to detect when
// the user enters/leaves practice mode (which is fragile and runs work
// outside practice for nothing), we hook the two Tekken functions that
// allocate / destruct the practice-mode controller singleton:
//
//   FUN_145CA3870 (RVA 0x05CA3870)  — practice controller factory.
//       Allocates the 0xD0-byte controller, calls its ctor (which
//       writes the singleton slot at module+0x9B79290), returns the
//       new instance. Single allocation path → fires exactly once per
//       practice session entry.
//
//   FUN_145C8C2F0 (RVA 0x05C8C2F0)  — controller derived destructor.
//       Clears the singleton slot (write at 0x145C8C349) before
//       tearing down sub-objects and operator delete. Hooking AT THE
//       ENTRY means our code runs while gameplay subsystems are still
//       live — perfect for "save before exit" semantics.
//
// See docs/RE_NOTES.md for the RE walk that justified these targets.

namespace opendojo::practice_state {

// True while the user is in practice mode. Driven by the lifecycle
// hooks above; flips on the controller ctor return and on the dtor
// entry. Single atomic load — cheap to call per frame.
bool is_active();

// Install the two MinHook detours. Idempotent. Call once at DLL init
// after the polaris base is known. MinHook is initialized internally
// (the call is also idempotent across the codebase).
void install_hooks();

}  // namespace opendojo::practice_state
