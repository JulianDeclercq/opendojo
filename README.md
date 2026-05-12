# OpenLab

Tekken 8 practice-mode drill sharing.

Export the CPU Opponent Action recordings from your practice slots to portable
files; import them on any machine. Drills are byte-for-byte clones of the
in-engine recording buffer — once imported, Tekken plays them back exactly as
if you'd recorded them yourself.

## Status

v0 — minimum viable export/import. No metadata, no slot picker (cursor-based),
no Action Interval support. Tested on Tekken 8 v3.00.02 (UE 5.2,
`Polaris-Win64-Shipping.exe`).

## Requirements

- Tekken 8 with [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) installed
- Practice mode used at least once per session (the recording-buffer pool is
  allocated lazily on first use)

## Install

Copy or symlink the `Mods/OpenLab` directory into Tekken's UE4SS mods folder:

```
<Tekken install>/Polaris/Binaries/Win64/Mods/OpenLab
```

Then add this line to `Mods/mods.txt` (or create an `enabled.txt` inside
`Mods/OpenLab/`):

```
OpenLab : 1
```

A directory junction works cross-volume on Windows and doesn't require admin:

```powershell
New-Item -ItemType Junction `
    -Path  "E:\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\Mods\OpenLab" `
    -Target "C:\path\to\openlab\Mods\OpenLab"
```

## Usage

In practice mode, with at least one recording in any slot:

| Key | Action |
|-----|--------|
| `Numpad 1` – `Numpad 8` | Export user slot 1–8 to `slot_N.drill` |
| `Ctrl` + `Numpad 1` – `8` | Import `slot_N.drill` into user slot 1–8 |

Drill files live in `Mods/OpenLab/drills/` (which is in this repo, so they're
version-controlled and shareable as-is).

## File format

```
+0x00  4 bytes   magic "OLAB"
+0x04  4 bytes   version (uint32 LE) = 1
+0x08  4 bytes   slot index hint (0..7)
+0x0C  4 bytes   reserved
+0x10  7202 bytes  raw practice slot data
```

The 7202-byte payload is Tekken's internal slot pitch (`0x1c22`). The first
two bytes are the event count; each subsequent 4-byte record is an input
transition. Trailing zeros are padding.

## How it works

Tekken stores practice-mode recordings in two heap-allocated global pools:

- **Pool 1** (recording-buffer pool): pointer at
  `Polaris-Win64-Shipping.exe+0x986AC70`. Layout: 9 slots × 7202 bytes. Slots
  0–7 = user slots; slot 8 = scratch (in-progress).
- **Pool 2** (frequency-modulated metadata): pointer at `+0x986AC78`. Only
  used for advanced playback modes (currently unsupported by OpenLab).

When you press Confirm on the "Record this movement?" dialog, the native save
function (`Polaris+0x18df920`) copies 7202 bytes from slot 8 to the chosen
user slot. OpenLab reads/writes that same pool directly.

## Roadmap

- v0 (now): byte-clone export/import for the 8 user slots
- v1: cursor-aware slot selection; status/list keybind
- v2: human-readable drill format (decoded events)
- v3: Action Interval / pool 2 support
- v4: drill metadata (name, character, date, author)
