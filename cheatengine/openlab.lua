-- OpenLab v0 — Tekken 8 practice-mode drill export/import.
--
-- Run this inside Cheat Engine's Lua engine (Ctrl+Alt+L, File > Load).
-- Tekken must be running and CE must be attached to Polaris-Win64-Shipping.exe.
-- The game's recording-buffer pool is allocated lazily, so you also need to
-- use practice-mode recording at least once per game session before any
-- export/import will succeed.
--
-- Hotkeys (global; work regardless of which window has focus):
--   F1..F8        export current contents of user slot N to slot_N.drill
--   Ctrl+1..8     import slot_N.drill into user slot N
--   F9            print status (module base, pool address, per-slot counts)
--
-- Edit DRILL_DIR below to point at your local clone of the openlab repo.

local OpenLab = {
    DRILL_DIR        = [[C:\Users\ethan\Desktop\openlab\Mods\OpenLab\drills]],
    POOL1_PTR_OFFSET = 0x986AC70,
    SLOT_PITCH       = 0x1C22,        -- 7202 bytes per slot
    USER_SLOTS       = 8,
    MAGIC            = "OLAB",
    VERSION          = 1,
    HEADER_SIZE      = 0x10,

    -- Service-locator context (FUN_1418dba40 returns this global).
    CTX_PTR_OFFSET   = 0x9537300,

    -- Subsystem keys (4-byte hash, stored at these exe offsets). We look them
    -- up at runtime via the service-locator's hash map.
    KEY_GAMEPLAY     = 0x9537314,
    KEY_SINGLETON    = 0x95371B0,
    KEY_SUBB         = 0x953707C,
    KEY_SUBC         = 0x9537080,

    -- "Recorded" flags. Empirically identified via clean/recorded diffs.
    --
    -- gameplay has a per-slot array: 8 bytes per slot, starting at +0x480.
    --   Entry N (zero-indexed slot N) lives at gameplay + 0x480 + N*8:
    --     +0 .. +3 : uint32 = 1   (constant — "slot allocated")
    --     +4 .. +7 : uint32 = 0 (empty) / 2 (recorded)
    --   The game's tick handler maintains this array from pool1 contents —
    --   if you write pool1[N] but flip the wrong slot's flag, the tick
    --   handler immediately resets your write *and* writes the correct
    --   slot's flag — so always write the correct one.
    --
    -- The other three (singleton +0x8, subB +0x65, subC +0x25C) appear to
    -- be global "any slot recorded" indicators. We still write them on
    -- import; they're harmless reinforcement and may matter on first-record
    -- transitions out of a fully-clean state.
    --
    -- Other notes:
    --   - Closing/reopening the practice menu is required for the display
    --     to refresh. The slot data and playback are immediately correct.
    --   - On a fresh game launch with no practice activity, the pool isn't
    --     allocated yet — import_slot will report and bail.

    -- Per-slot gameplay flag layout.
    GAMEPLAY_SLOT_BASE   = 0x480,   -- start of 8-slot array
    GAMEPLAY_SLOT_STRIDE = 0x08,    -- bytes per per-slot entry
    GAMEPLAY_SLOT_FLAG   = 0x04,    -- offset within entry of the recorded flag
}

-- ---------------------------------------------------------------------------
-- Logging — goes to CE's Lua Engine output panel
-- ---------------------------------------------------------------------------

local function log(fmt, ...)
    print(string.format("[OpenLab] " .. fmt, ...))
end

-- ---------------------------------------------------------------------------
-- Memory access via CE built-ins
-- ---------------------------------------------------------------------------

local function get_module_base()
    -- getAddress accepts a module name and returns the runtime base.
    local ok, base = pcall(getAddress, "Polaris-Win64-Shipping.exe")
    if not ok or not base or base == 0 then return nil end
    return base
end

-- Walk the service-locator hash map (FUN_1418db8f0 in the binary) to resolve
-- a subsystem by its 4-byte hash key stored at `key_offset` from module base.
-- Returns the runtime address of the subsystem, or nil if not found.
local function lookup_subsystem(key_offset)
    local base = get_module_base()
    if not base then return nil end
    local ctx = readQword(base + OpenLab.CTX_PTR_OFFSET)
    if not ctx or ctx == 0 then return nil end
    local map = readQword(ctx + 0x10)
    if not map or map == 0 then return nil end
    local sentinel = readQword(map + 0x100)
    local mask     = readQword(map + 0x128)
    local buckets  = readQword(map + 0x110)
    if not buckets then return nil end
    local key = readInteger(base + key_offset)
    local bucket = buckets + (mask & key) * 0x10
    local first = readQword(bucket)
    local entry = readQword(bucket + 8)
    local steps = 0
    while entry ~= 0 and entry ~= sentinel and steps < 64 do
        if readInteger(entry + 0x10) == key then
            return readQword(entry + 0x18)
        end
        if entry == first then break end
        entry = readQword(entry + 8)
        steps = steps + 1
    end
    return nil
end

-- Resolve the four subsystems we need to flip the "recorded" flags.
-- Cached after first lookup since the addresses are stable for the session.
local subsys_cache = {}
local function get_subsys(name, key)
    if subsys_cache[name] then return subsys_cache[name] end
    local addr = lookup_subsystem(key)
    if addr then subsys_cache[name] = addr end
    return addr
end

-- Sets the per-slot "recorded" flag in the gameplay array and the three
-- global "any slot recorded" indicators. `slot_idx` is 0..7.
-- Returns true on success, false if any subsystem couldn't be resolved.
local function set_recorded_flags(slot_idx, recorded)
    local gameplay  = get_subsys("gameplay",  OpenLab.KEY_GAMEPLAY)
    local singleton = get_subsys("singleton", OpenLab.KEY_SINGLETON)
    local subB      = get_subsys("subB",      OpenLab.KEY_SUBB)
    local subC      = get_subsys("subC",      OpenLab.KEY_SUBC)
    if not (gameplay and singleton and subB and subC) then
        log("subsystem lookup failed (gameplay=%s singleton=%s subB=%s subC=%s)",
            tostring(gameplay), tostring(singleton), tostring(subB), tostring(subC))
        return false
    end
    local slot_flag_addr = gameplay + OpenLab.GAMEPLAY_SLOT_BASE
                                    + slot_idx * OpenLab.GAMEPLAY_SLOT_STRIDE
                                    + OpenLab.GAMEPLAY_SLOT_FLAG
    if recorded then
        writeInteger(slot_flag_addr,     2)
        writeBytes(singleton + 0x008, 0x01)
        writeBytes(subB      + 0x065, 0x00)
        writeInteger(subC    + 0x25C,    1)
    else
        writeInteger(slot_flag_addr,     0)
        writeBytes(singleton + 0x008, 0x00)
        writeBytes(subB      + 0x065, 0x01)
        writeInteger(subC    + 0x25C,   -1)
    end
    return true
end

local function get_pool1_base()
    local base = get_module_base()
    if not base then
        log("Polaris-Win64-Shipping.exe not found — is CE attached to Tekken?")
        return nil
    end
    local pool1 = readQword(base + OpenLab.POOL1_PTR_OFFSET)
    if not pool1 or pool1 == 0 then
        log("pool1 is NULL — use practice-mode recording at least once first")
        return nil
    end
    return pool1
end

local function slot_address(slot_idx)
    if slot_idx < 0 or slot_idx >= OpenLab.USER_SLOTS then
        log("invalid slot index %d (must be 0..%d)", slot_idx, OpenLab.USER_SLOTS - 1)
        return nil
    end
    local pool1 = get_pool1_base()
    if not pool1 then return nil end
    return pool1 + slot_idx * OpenLab.SLOT_PITCH
end

-- ---------------------------------------------------------------------------
-- File format helpers
-- ---------------------------------------------------------------------------

local function pack_le32(n)
    return string.char(
        n            % 256,
        math.floor(n /         256) % 256,
        math.floor(n /       65536) % 256,
        math.floor(n /    16777216) % 256)
end

local function unpack_le32(s, offset)
    offset = offset or 1
    return s:byte(offset)
         + s:byte(offset + 1) *      256
         + s:byte(offset + 2) *    65536
         + s:byte(offset + 3) * 16777216
end

local function bytes_to_string(bytes)
    -- Lua 5.x: avoid string.char(unpack(...)) for >8000 args; loop instead.
    local chars = {}
    for i = 1, #bytes do chars[i] = string.char(bytes[i]) end
    return table.concat(chars)
end

local function string_to_bytes(s)
    local bytes = {}
    for i = 1, #s do bytes[i] = s:byte(i) end
    return bytes
end

local function event_count_from_bytes(bytes)
    if #bytes < 2 then return 0 end
    return bytes[1] + bytes[2] * 256
end

local function drill_path(slot_idx)
    -- File names use 1-based, user-facing slot numbers.
    return OpenLab.DRILL_DIR .. "\\slot_" .. (slot_idx + 1) .. ".drill"
end

-- ---------------------------------------------------------------------------
-- Export / import
-- ---------------------------------------------------------------------------

local function export_slot(slot_idx)
    local addr = slot_address(slot_idx)
    if not addr then return false end

    local bytes = readBytes(addr, OpenLab.SLOT_PITCH, true)
    if not bytes then
        log("readBytes failed at 0x%X", addr)
        return false
    end
    local n = event_count_from_bytes(bytes)
    if n == 0 then
        log("slot %d is empty — nothing to export", slot_idx + 1)
        return false
    end

    local path = drill_path(slot_idx)
    local f, err = io.open(path, "wb")
    if not f then
        log("open(%q) failed: %s", path, tostring(err))
        return false
    end
    f:write(OpenLab.MAGIC)
    f:write(pack_le32(OpenLab.VERSION))
    f:write(pack_le32(slot_idx))
    f:write(pack_le32(0))
    f:write(bytes_to_string(bytes))
    f:close()
    log("exported slot %d (%d events) -> %s", slot_idx + 1, n, path)
    return true
end

local function import_slot(slot_idx)
    local path = drill_path(slot_idx)
    local f, err = io.open(path, "rb")
    if not f then
        log("open(%q) failed: %s", path, tostring(err))
        return false
    end
    local content = f:read("*all")
    f:close()

    local need = OpenLab.HEADER_SIZE + OpenLab.SLOT_PITCH
    if #content < need then
        log("%s too small: %d bytes (need %d)", path, #content, need)
        return false
    end
    if content:sub(1, 4) ~= OpenLab.MAGIC then
        log("%s: bad magic %q", path, content:sub(1, 4))
        return false
    end
    local version = unpack_le32(content, 5)
    if version ~= OpenLab.VERSION then
        log("%s: unsupported version %d (this is v%d)", path, version, OpenLab.VERSION)
        return false
    end

    local data = content:sub(OpenLab.HEADER_SIZE + 1, OpenLab.HEADER_SIZE + OpenLab.SLOT_PITCH)
    local addr = slot_address(slot_idx)
    if not addr then return false end

    writeBytes(addr, string_to_bytes(data))
    if not set_recorded_flags(slot_idx, true) then
        log("WARNING: pool1 written but flags not set — menu may show 'Not Set'")
    end
    log("imported %s -> slot %d (%d events). Close+reopen the practice menu to see the update.",
        path, slot_idx + 1, data:byte(1) + data:byte(2) * 256)
    return true
end

local function show_status()
    log("=== OpenLab status ===")
    local base = get_module_base()
    if not base then return end
    log("  module base    = 0x%X", base)
    log("  pool1 ptr addr = 0x%X", base + OpenLab.POOL1_PTR_OFFSET)
    local pool1 = readQword(base + OpenLab.POOL1_PTR_OFFSET)
    if not pool1 or pool1 == 0 then
        log("  pool1          = NULL (record once first)")
        return
    end
    log("  pool1          = 0x%X", pool1)
    for i = 0, OpenLab.USER_SLOTS - 1 do
        local b = readBytes(pool1 + i * OpenLab.SLOT_PITCH, OpenLab.SLOT_PITCH, true)
        if b then
            local n = event_count_from_bytes(b)
            log("  slot %d: %s", i + 1,
                n == 0 and "empty" or string.format("%d events", n))
        end
    end
end

-- ---------------------------------------------------------------------------
-- Hotkey registration
--
-- CE's createHotkey takes (callback, keycode, [keycode...]). Modifier keys
-- come first. Virtual key codes:
--   VK_CONTROL    = 0x11 (left or right Ctrl)
--   VK_1..8       = 0x31..0x38 (number-row digit keys)
--   VK_F1..F12    = 0x70..0x7B
-- ---------------------------------------------------------------------------

-- Clean up any pre-existing hotkeys from a previous load of this script
-- so reloading doesn't stack duplicates.
if _G.openlab_hotkeys then
    for _, hk in ipairs(_G.openlab_hotkeys) do
        local ok = pcall(function() hk.destroy(hk) end)
        if not ok then pcall(function() hk:destroy() end) end
    end
end
_G.openlab_hotkeys = {}

local function register_hotkey(callback, ...)
    local ok, hk = pcall(createHotkey, callback, ...)
    if ok and hk then table.insert(_G.openlab_hotkeys, hk) end
    return ok, hk
end

local VK_CONTROL = 0x11
for i = 1, OpenLab.USER_SLOTS do
    local slot_idx = i - 1
    local f_key   = 0x6F + i  -- F1=0x70, ..., F8=0x77
    local num_key = 0x30 + i  -- 1=0x31,  ..., 8=0x38
    register_hotkey(function() export_slot(slot_idx) end, f_key)
    register_hotkey(function() import_slot(slot_idx) end, VK_CONTROL, num_key)
end
register_hotkey(function() show_status() end, 0x78)  -- F9

log("loaded — F1..F8 = export, Ctrl+1..8 = import, F9 = status")
log("drill files: %s", OpenLab.DRILL_DIR)
log("call OpenLab_destroy() to release hotkeys without closing CE")

-- Make the API accessible from the CE Lua console for manual testing.
_G.OpenLab = {
    export_slot = export_slot,
    import_slot = import_slot,
    show_status = show_status,
    config      = OpenLab,
}

-- Release all hotkeys (they're global so they'll fire from any focused
-- window — call this when you want to type freely elsewhere without
-- triggering exports/imports).
function _G.OpenLab_destroy()
    if not _G.openlab_hotkeys then return end
    for _, hk in ipairs(_G.openlab_hotkeys) do
        local ok = pcall(function() hk.destroy(hk) end)
        if not ok then pcall(function() hk:destroy() end) end
    end
    _G.openlab_hotkeys = nil
    log("hotkeys released")
end
