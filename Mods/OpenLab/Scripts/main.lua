-- OpenLab UE4SS mod — informational stub for v0.
--
-- Tekken's UE4SS build ships reference Lua 5.4 without LuaJIT FFI, so we
-- cannot read/write raw process memory directly from this script. v0 of
-- OpenLab runs as a Cheat Engine Lua script instead — see ../../cheatengine/
-- openlab.lua and the project README.
--
-- This stub stays in place because the directory junction makes it visible
-- to UE4SS at startup. It just prints a one-liner in the UE4SS console so
-- you can confirm the OpenLab repo is correctly linked into Tekken's mods
-- folder.

print("[OpenLab] mod folder linked correctly. v0 hotkeys run from the Cheat Engine script (cheatengine/openlab.lua). UE4SS-native version pending.\n")
