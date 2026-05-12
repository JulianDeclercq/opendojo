# OpenDojo

Tekken 8 practice-mode drill sharing.

Export the CPU Opponent Action recordings from your practice slots to portable
files; import them on any machine. Drills are byte-for-byte clones of the
in-engine recording buffer — once imported, Tekken plays them back exactly as
if you'd recorded them yourself.

## Status

**v0** — working end-to-end via a Cheat Engine Lua script. Tested on Tekken 8
v3.00.02 (UE 5.2, `Polaris-Win64-Shipping.exe`). Export/import round-trips
correctly: an imported drill plays back as if you'd recorded it yourself.
You may need to close and reopen the practice menu once after import for the
display to refresh — the data and playback are correct immediately.

The original plan was a UE4SS-native Lua mod, but Tekken's UE4SS build ships
reference Lua 5.4 without LuaJIT FFI, so a Lua mod cannot read raw process
memory. CE has built-in memory primitives, so we use CE for v0. A native
UE4SS mod (C++ DLL or LuaJIT-enabled UE4SS build) is on the roadmap.

## Requirements

- Tekken 8 v3.00.02
- [Cheat Engine](https://www.cheatengine.org/) (any modern version)
- Optional: [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) — the UE4SS mod in
  this repo is currently just a "linked correctly" stub, but the directory
  junction setup makes future expansion painless

## Install

1. Clone this repo somewhere stable (e.g. `C:\Users\you\Desktop\opendojo`).
2. Open `cheatengine/opendojo.lua` in a text editor and update `DRILL_DIR` to
   the absolute path of `<repo>\Mods\OpenDojo\drills` on your machine.
3. (Optional, for the UE4SS stub): create a directory junction so the mod
   files live in this repo but Tekken's UE4SS can still see them. Junctions
   work cross-volume on Windows and don't require admin:

   ```powershell
   New-Item -ItemType Junction `
       -Path  "E:\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\Mods\OpenDojo" `
       -Target "<repo>\Mods\OpenDojo"
   ```

   Then add `OpenDojo : 1` to `<TekkenInstall>\Polaris\Binaries\Win64\Mods\mods.txt`.

## Usage

1. Start Tekken 8 and get into a practice match.
2. **Record into at least one practice slot once per session.** This is what
   allocates the recording-buffer pool — until you do it, the pool pointer is
   NULL and there's nothing to read/write. After this first recording the
   pool is alive for the rest of the session.
3. Open Cheat Engine and attach it to `Polaris-Win64-Shipping.exe`.
4. In CE: **Table > Show Cheat Table Lua Script** (or open `Memory View > Tools
   > Auto Assemble`, then **File > Load Lua Script**). Paste or load
   `cheatengine/opendojo.lua` and execute it. You should see:

   ```
   [OpenDojo] loaded — NumPad 1..8 = export, Ctrl+NumPad 1..8 = import, NumPad 0 = status
   [OpenDojo] drill files: C:\path\to\opendojo\Mods\OpenDojo\drills
   ```

5. Use the hotkeys (they're global — they work whether CE or Tekken has focus):

   | Key | Action |
   |-----|--------|
   | **F1** – **F8** | Export user slot 1–8 to `slot_N.drill` |
   | **1** – **8** | Import `slot_N.drill` into user slot 1–8 |
   | **F9** | Print status — module base, pool address, per-slot event counts |

   The number-row import keys are global — they'll fire from any focused
   window, including Tekken itself. Don't unload the script if you're typing
   in another app, or 1–8 will trigger imports unexpectedly. Close the CE
   Lua engine window (or call `OpenDojo_destroy()` from CE's console) to
   release the hotkeys.

Drill files land in `Mods/OpenDojo/drills/` inside this repo — committed and
shareable as-is.

## File format

```
+0x00   4 bytes    magic "OLAB"
+0x04   4 bytes    version (uint32 LE) = 1
+0x08   4 bytes    slot index hint (0..7)
+0x0C   4 bytes    reserved
+0x10   7202 bytes raw practice slot data
total   7218 bytes
```

The 7202-byte payload is Tekken's internal slot pitch (`0x1c22`). The first
two bytes of the payload are the event count; each subsequent 4-byte record
is an input transition. Trailing zeros are padding.

## How it works

Tekken stores practice-mode recordings in two heap-allocated global pools:

- **Pool 1** (recording-buffer pool): pointer at
  `Polaris-Win64-Shipping.exe+0x986AC70`. Layout: 9 slots × 7202 bytes. Slots
  0–7 = user slots; slot 8 = scratch (in-progress).
- **Pool 2** (frequency-modulated metadata): pointer at `+0x986AC78`. Only
  used for advanced playback modes — OpenDojo v0 doesn't touch it.

When you press Confirm on the "Record this movement?" dialog, the native save
function (`Polaris+0x18df920`) `memcpy`s 7202 bytes from slot 8 into your
chosen user slot. OpenDojo reads/writes that same pool directly.

Writing pool1 by itself isn't enough — the game has four global "at least
one slot has a recording" flags scattered across different subsystems, and
the menu/playback refuse to recognize the data until those are set. OpenDojo
resolves the subsystems at runtime by walking the same service-locator hash
map the game uses (`FUN_1418db8f0`), then writes the flags after the pool1
copy.

Per-event format (each 4 bytes):

| Byte | Meaning |
|------|---------|
| 0    | High nibble = port marker (always `0x2` for CPU/P2). Low nibble = Tekken numpad direction (0=neutral, 2=down, 4=back, 6=forward, 8=up) |
| 1    | Button bitmask: `0x10 LP`, `0x20 RP`, `0x40 LK`, `0x80 RK` |
| 2    | Not fully decoded — likely auxiliary input flags (Heat / Rage / throw shortcut) and/or animation state |
| 3    | Duration in frames until the next transition. Sums across a recording ≈ recording length × 60fps. |

## Roadmap

- v0 (now): byte-clone export/import via CE script, for the 8 user slots
- v1: cursor-aware slot selection so you don't have to remember which slot
  you're targeting
- v2: human-readable drill format (decoded events + JSON or text serialization)
- v3: native UE4SS mod (either compile a C DLL with `ReadProcessMemory` /
  `WriteProcessMemory` exports for Lua to call, or move to a UE4SS build that
  ships LuaJIT with FFI)
- v4: Action Interval / pool 2 support
- v5: drill metadata (name, character, date, author, notes)

## Layout

```
opendojo/
├── README.md
├── .gitignore
├── cheatengine/
│   └── opendojo.lua            # v0 entry point — load this in CE
└── Mods/
    └── OpenDojo/
        ├── enabled.txt        # UE4SS sentinel
        ├── Scripts/main.lua   # UE4SS info stub
        └── drills/            # exported drill files live here (committed)
```
