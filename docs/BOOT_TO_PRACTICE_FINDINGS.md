# Boot-to-Practice RE Findings

RE session for the proposed "skip title + main menu, jump straight into practice
on game startup" feature. Tekken 8 v3.00.02 / Polaris-Win64-Shipping.exe.

## Goal

When the user launches Tekken with OpenDojo, optionally bypass the title
sequence and main-menu navigation and drop the user directly into Practice
mode with sensible defaults. Engine init must still run; only the
"unskippable intro" + manual menu clicks are skipped.

## Architecture of practice entry (confirmed)

Practice entry is **state-driven, not call-driven**. There is no single
"`EnterPractice(...)`" function we can call. Instead:

1. The user navigates Main Menu → Practice → character select → stage
   select → confirm.
2. The menu code populates various state (character IDs, stage, side, etc.)
   in subsystems and globals.
3. The menu code flips a top-level **battle-mode enum** to a practice
   value.
4. The per-frame engine tick (`FUN_145c58d50`) sees the new mode and calls
   `FUN_145c5fc40` (singleton-ensure), which constructs the practice
   controller via `FUN_145ca38a0(submode)`.

By the time the practice controller is being constructed, the menu code
has long since returned — there is **no menu frame on the call stack of
the practice ctor**, confirmed via runtime HW exec BP.

## Battle-mode enum

The enum lives at offset 0 of a service-locator-resolved subsystem. To get
its address at runtime:

```
engine_root = *(uint64*)( polaris_base + 0x9B75568 )
entry       = subsystem_lookup(engine_root, key=0x13D315F5)
value_ptr   = *(uint64*)(entry + 0x18)        // entry value
battle_obj  = *(uint64*)(value_ptr + 8)
mode_enum   = *(uint32*)(battle_obj + 0)
```

The lookup hash key `0x13D315F5` is stored in `engine_root + 0x88`. The
service-locator implementation is `FUN_1459f07c0` (std::unordered_map).
The whole chain is wrapped in `FUN_1459f4560(engine_root)` and read by
`FUN_141984a20` (which is literally `return *param_1`).

Known mode values that the practice path checks for in `FUN_145c5fc40`:

| Value | Meaning | Practice ctor arg |
|-------|---------|-------------------|
| 1     | PracticePre  | 0 |
| 4     | PracticeMain | 2 |
| 0x13  | (variant)    | 1 |
| 0x14  | RevengePractice (via `FUN_145c93300` check) | — |
| 0x15  | OnlinePractice | 3 |

Empirically the mode is `1` (PracticePre) immediately upon entering
practice mode in the game.

## Practice controller

- Ctor wrapper: `FUN_145ca38a0(uint8 submode)` at polaris RVA `0x5CA38A0`
- Ctor body:    `FUN_145c8aff0` at polaris RVA `0x5C8AFF0`
- Allocates 0x150 bytes, stores `submode` byte at `this + 0x12D`, registers
  itself in the global slot at polaris+0x9B792A8 (this is the practice
  singleton OpenDojo already hooks).
- vtable: `PTR_LAB_148550eb0`
- Dtor:   vtable slot 0 → polaris RVA `0x5C8C880` (already hooked for
  autosave-on-exit)

The ctor does **not** take character/stage args — those must be populated
in other subsystems before the mode enum is flipped.

## Per-frame singleton-ensure (FUN_145c5fc40)

```c
mode = read_mode_enum()
if mode in {1, 0x13, 0x15}:
    if practice_singleton == NULL:
        singleton = FUN_145ca38a0(submode_for_this_mode)
        register_with_subsystem(singleton)
elif mode == 4:  // PracticeMain
    if practice_singleton == NULL and other_gates_pass:
        singleton = FUN_145ca38a0(2)
        ...
elif mode == 0x14:  // RevengePractice
    ...
```

So the practice controller is created lazily — once we flip the mode
enum AND the singleton is null, the engine constructs it next frame.

This is OpenDojo's existing extension point: we already hook the
practice-controller ctor and dtor.

## State populated by upstream menus (unknown details — needs more RE)

Before the menu code flips the mode enum, the character-select and
stage-select screens populate:

- **Character IDs (P1 + CPU)**: stored on the Player struct at `+0x168`.
  Player struct is reached via the GlobalPlayerHolder → `+0x30`/`+0x38`.
  We don't yet know where the menu code WRITES these.
- **Stage ID**: unknown global.
- **Side (P1/P2)**: stored at `gameplay + 0x47C` (OpenDojo already reads
  this for movelist resolution).
- **CPU difficulty, round count, etc.**: unknown.

Replicating these writes is the load-bearing unknown for Option B (full
skip). Without these set up, the practice controller will likely
construct but crash on first access (or load with garbage data).

## What we couldn't pin down today

The **specific instruction that writes the mode enum** when the user
confirms practice-mode entry. Runtime HW write BPs on `0x1F238570000`
showed:

- The page is heavily trafficked (many adjacent fields on the same 4KB
  page, ntdll `repe movsb` memcpys, etc.)
- VEH-based HW BPs are page-granular in practice, catching unrelated
  writes (and even some reads)
- Polaris-range hits included writes to `+0x18`, `+0x10`, plus reads —
  but no clean "small enum value written to offset 0" hit was
  identifiable in the noise

Possible next steps for finding the writer:

1. **Windows native debugger** (CE `interface=1`) for real DR-register
   precision, not VEH page-protect
2. **AOB scan in polaris .text** for `mov dword [reg], 4` /
   `mov dword [reg], 1` patterns where the destination register is
   loaded from the service-locator chain
3. **Walk forward from a known menu handler** — search Ghidra for the
   string `"GotoPractice"` (at `0x147814450`) and trace into its UFunction
   handler

## Strategy options for the feature

### Option A — title-skip only (lower risk, partial benefit)

Find what dismisses the title screen / Bink intro / "press any key"
prompt and auto-fire it. User still navigates the main menu manually.
Eliminates the most annoying unskippable part. Requires finding the title
state machine + its dismiss handler — separate RE task from above.

### Option B — full skip (high value, high RE cost)

Replicate every state-write the menu chain does:

1. Wait until engine init reaches a known "ready" point (some specific
   subsystem becomes non-null — TBD which one)
2. Populate character IDs (P1 + CPU), stage, side, CPU difficulty in
   their respective globals
3. Flip the battle-mode enum to `4` (PracticeMain) directly via memory
   write
4. Let the per-frame tick construct the practice controller as normal

Required RE work before this is buildable:

- Find each of the menu-confirm handlers (character-select-confirm,
  stage-select-confirm) and identify what they write
- Find the "engine ready" signal so we don't fire too early
- Find and dismiss the title screen (same as Option A)

### Option C — hybrid: skip title, fast-forward menu

Fire synthetic input events to navigate Main Menu → Practice → defaults.
Avoids needing to know the underlying state writes but is fragile across
patches and UI variants. Not recommended.

## Recommendation

If the goal is "save user time on every launch," **Option A** (title
skip) is probably 80% of the value for 20% of the work — the Bink intro
+ press-any-key is what the user described as the most annoying part.

**Option B** is a separate, significantly larger RE project. Worth doing
only if title-skip alone isn't enough.

## Runtime addresses captured this session (heap, won't persist)

These won't be valid across game restarts but document the session for
reference:

- polaris base: `0x7FF65E520000`
- engine_root: `0x1F23325BA40`
- service-locator entry: `0x1F36B701780` (key `0x13D315F5`)
- entry value: `0x1F35544E800`
- battle-state struct: `0x1F238570000` (mode enum = first uint32)

The stable inputs are:
- `polaris + 0x9B75568` — engine_root pointer
- service-locator hash key — `0x13D315F5`
- Service-locator lookup function — `FUN_1459f07c0` (polaris RVA `0x59F07C0`)
- Mode-enum getter — `FUN_141984a20` (polaris RVA `0x1984A20`)
