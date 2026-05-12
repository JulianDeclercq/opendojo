-- OpenLab: Tekken 8 practice-mode drill export/import.
--
-- Tekken stores 8 user practice recordings in a heap-allocated pool whose
-- pointer lives at Polaris-Win64-Shipping.exe+0x986AC70. Each slot is a
-- fixed 0x1C22 (7202) bytes. We read/write those bytes directly via LuaJIT
-- FFI; no Cheat Engine or external process needed.

local OpenLab = {
    MOD_NAME          = "OpenLab",
    POOL1_PTR_OFFSET  = 0x986AC70,
    SLOT_PITCH        = 0x1C22,        -- 7202 bytes per slot
    USER_SLOTS        = 8,
    SCRATCH_SLOT      = 8,             -- in-progress recording lives here

    -- File format
    MAGIC             = "OLAB",
    VERSION           = 1,
    HEADER_SIZE       = 0x10,

    -- Drill files are stored alongside the mod so a directory junction
    -- pulls them into the version-controlled openlab repo automatically.
    -- This path is resolved relative to the game's cwd (Win64/).
    DRILL_DIR         = "Mods/OpenLab/drills",
}

-- ---------------------------------------------------------------------------
-- FFI setup
-- ---------------------------------------------------------------------------

local ffi = require("ffi")
ffi.cdef[[
    void* GetModuleHandleA(const char* lpModuleName);
]]

local function log(fmt, ...)
    -- Trailing \n matches the UE4SS console expectation.
    print(string.format("[" .. OpenLab.MOD_NAME .. "] " .. fmt .. "\n", ...))
end

-- Resolve the recording-buffer pool's runtime base address.
-- Returns a uint8_t* into the pool, or nil if not yet allocated.
local function get_pool1_base()
    local mod = ffi.C.GetModuleHandleA("Polaris-Win64-Shipping.exe")
    if mod == nil then
        log("GetModuleHandleA returned NULL — is this Tekken 8?")
        return nil
    end
    local module_base = ffi.cast("uintptr_t", mod)
    local pool1_ptr = ffi.cast("uintptr_t*", module_base + OpenLab.POOL1_PTR_OFFSET)
    local pool1 = pool1_ptr[0]
    if pool1 == 0 then
        log("pool1 is NULL — use practice-mode recording at least once first")
        return nil
    end
    return ffi.cast("uint8_t*", pool1)
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
        math.floor(n /    16777216) % 256
    )
end

local function unpack_le32(s, offset)
    offset = offset or 1
    return s:byte(offset)
         + s:byte(offset + 1) *      256
         + s:byte(offset + 2) *    65536
         + s:byte(offset + 3) * 16777216
end

local function event_count(slot_data)
    if #slot_data < 2 then return 0 end
    return slot_data:byte(1) + slot_data:byte(2) * 256
end

local function drill_path(slot_idx)
    -- Filenames use 1-based user-facing slot numbers.
    return string.format("%s/slot_%d.drill", OpenLab.DRILL_DIR, slot_idx + 1)
end

local function ensure_drill_dir()
    -- Best-effort mkdir; ignore errors. Windows-style backslashes for cmd.
    local win_path = OpenLab.DRILL_DIR:gsub("/", "\\")
    os.execute(string.format('if not exist "%s" mkdir "%s"', win_path, win_path))
end

-- ---------------------------------------------------------------------------
-- Export / import
-- ---------------------------------------------------------------------------

local function export_slot(slot_idx)
    local addr = slot_address(slot_idx)
    if not addr then return false end

    local data = ffi.string(addr, OpenLab.SLOT_PITCH)
    local n = event_count(data)
    if n == 0 then
        log("slot %d is empty — nothing to export", slot_idx + 1)
        return false
    end

    ensure_drill_dir()
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
    f:write(data)
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
        log("%s is too small: %d bytes (need %d)", path, #content, need)
        return false
    end
    if content:sub(1, 4) ~= OpenLab.MAGIC then
        log("%s: bad magic %q (expected %q)", path, content:sub(1, 4), OpenLab.MAGIC)
        return false
    end
    local version = unpack_le32(content, 5)
    if version ~= OpenLab.VERSION then
        log("%s: unsupported version %d (this mod is v%d)", path, version, OpenLab.VERSION)
        return false
    end

    local data = content:sub(OpenLab.HEADER_SIZE + 1, OpenLab.HEADER_SIZE + OpenLab.SLOT_PITCH)
    local addr = slot_address(slot_idx)
    if not addr then return false end

    ffi.copy(addr, data, OpenLab.SLOT_PITCH)
    log("imported %s -> slot %d (%d events)", path, slot_idx + 1, event_count(data))
    return true
end

local function show_status()
    log("=== OpenLab status ===")
    local mod = ffi.C.GetModuleHandleA("Polaris-Win64-Shipping.exe")
    if mod == nil then
        log("  Tekken module not found")
        return
    end
    local base = tonumber(ffi.cast("uintptr_t", mod))
    log("  module base    = 0x%X", base)
    log("  pool1 ptr addr = 0x%X", base + OpenLab.POOL1_PTR_OFFSET)

    local pool1 = get_pool1_base()
    if not pool1 then return end
    log("  pool1 base     = 0x%X", tonumber(ffi.cast("uintptr_t", pool1)))

    for i = 0, OpenLab.USER_SLOTS - 1 do
        local data = ffi.string(pool1 + i * OpenLab.SLOT_PITCH, OpenLab.SLOT_PITCH)
        local n = event_count(data)
        log("  slot %d: %s", i + 1,
            n == 0 and "empty" or string.format("%d events", n))
    end
end

-- ---------------------------------------------------------------------------
-- Keybinds
--
-- NumPad N        = export user slot N to slot_N.drill
-- Ctrl + NumPad N = import slot_N.drill into user slot N
-- NumPad 0        = print status (pool address, per-slot event counts)
--
-- NumPad chosen to avoid conflict with PracticeRecHook (F4–F12) and with
-- in-game number-row inputs.
-- ---------------------------------------------------------------------------

local NUM_KEYS = {
    Key.NUM_ONE,   Key.NUM_TWO,   Key.NUM_THREE, Key.NUM_FOUR,
    Key.NUM_FIVE,  Key.NUM_SIX,   Key.NUM_SEVEN, Key.NUM_EIGHT,
}

for i = 1, OpenLab.USER_SLOTS do
    local slot_idx = i - 1
    RegisterKeyBind(NUM_KEYS[i], function() export_slot(slot_idx) end)
    RegisterKeyBind(NUM_KEYS[i], { ModifierKey.CONTROL }, function() import_slot(slot_idx) end)
end

RegisterKeyBind(Key.NUM_ZERO, function() show_status() end)

log("loaded — NumPad 1..8 = export, Ctrl+NumPad 1..8 = import, NumPad 0 = status")
log("drill files: %s/", OpenLab.DRILL_DIR)
