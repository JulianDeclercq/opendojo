# OpenDojo native DLL

Standalone Windows DLL for Tekken 8 drill export/import. No UE4SS, no Cheat
Engine — just drop a single DLL into the game folder.

## Status

**Scaffolding only.** Currently:

- Builds a `dinput8.dll` that the game loads via the DLL proxy pattern.
- Forwards every export to the real `C:\Windows\System32\dinput8.dll` so
  controller input keeps working unchanged.
- Writes `opendojo.log` next to the game exe on startup as a load-success
  indicator (and as the only debug channel — release builds have no console).

What's **not** here yet (still in the CE Lua script):
- Pool1 read/write
- Service-locator walking + subsystem flag writes
- Drill text format encode/decode
- Hotkey loop
- Per-slot flag handling

Those land in subsequent commits. The CE script in `../cheatengine/opendojo.lua`
remains the working v0 until the DLL feature-matches it.

## Build

Requires:
- Visual Studio 2022 (or VS Build Tools 2022) with the C++ workload installed.
- CMake 3.20+.
- 64-bit Windows.

From this `dll/` directory:

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\dinput8.dll`.

The DLL is statically linked against the CRT (`/MT`), so end users do **not**
need the Visual C++ redistributable installed.

## Install

1. Close Tekken 8.
2. Copy the built `dinput8.dll` to
   `<TEKKEN 8 install>\Polaris\Binaries\Win64\`
   (the directory that contains `Polaris-Win64-Shipping.exe`).
3. Launch Tekken 8 **offline**. Denuvo / online play with a third-party DLL
   in the game folder is an anti-cheat risk; this DLL is meant for solo
   practice-mode use only.
4. Verify `opendojo.log` was created in that same directory and reads
   `OpenDojo v0.1.0 starting up`. If yes, the DLL loaded successfully.

To uninstall: delete `dinput8.dll` from that folder.

## Why dinput8.dll?

Tekken 8 imports DirectInput8 for legacy controller compatibility, so
Windows' DLL search order picks up our `dinput8.dll` from the game folder
before the system copy. This is the canonical fighting-game modding entry
point.

If a future Tekken patch ever drops the DirectInput8 import, fall back to
`version.dll` — it's loaded by virtually every Win32 process. Adding a new
proxy is a two-file diff: drop a `proxy_<name>.cpp` and `proxy_<name>.def`
next to the existing ones, then rebuild with
`-DOPENDOJO_PROXY=<name>`.

## Layout

```
dll/
├── CMakeLists.txt
├── README.md
├── proxy_dinput8.def         # export name list — must match the real DLL
└── src/
    ├── main.cpp              # DllMain + init thread
    ├── log.hpp / log.cpp     # thread-safe file logger
    ├── proxy.hpp             # proxy-loader interface
    └── proxy_dinput8.cpp     # forwarded exports + real-DLL resolver
```

## Notes for the future

- **Where drills live**: not decided yet. Probably
  `<game>\Polaris\Binaries\Win64\opendojo_drills\` for "drop-and-go" UX, with
  a config file override. Will be revisited when drill I/O ports over.
- **Hotkeys**: planned via `RegisterHotKey` on a dedicated thread. The CE
  script's behavior (global, focus-independent) is the right baseline.
- **Aux desync**: see `../memory/project_opendojo_aux_desync.md`. Once the
  DLL is in place we can hook `FUN_141807610`'s mod-251 result or NOP the
  comparison in `FUN_1418e8ea0` so drills become fully hand-editable.
