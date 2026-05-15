-- Wide-net byte-level probe of singleton/recording/gameplay/subB/subC
-- to find what changes when the user opens/closes the in-game practice
-- pause menu. After we autoload the slot data + flags, the in-game UI
-- shows "no recordings" until the user opens/closes the practice pause
-- menu several times. Whatever flips during those cycles is the missing
-- piece — once identified, we can write that ourselves after autoload.
--
-- Usage:
--   1. Make sure autoload has already fired for the current character
--      (F12 menu shows slot data, in-game menu shows "no recordings").
--   2. Load + Execute this script. It snapshots key regions and then
--      polls every 100ms for 45 seconds, reporting each byte that
--      changes plus the relative time.
--   3. While the probe runs:
--        - wait ~3s, do nothing
--        - open the in-game practice pause menu (Start button)
--        - wait ~3s, navigate to Practice Settings → CPU Action (or
--          wherever the slot list is)
--        - close the menu (Start again or B)
--        - repeat once or twice more
--   4. Paste the output. Correlate timestamps with your actions.

local MODULE = "Polaris-Win64-Shipping.exe"
local base = getAddress(MODULE)
if not base or base == 0 then
    print("Polaris not loaded.")
    return
end

local function nowMs() return getTickCount() end

local CTX_PTR_OFFSET = 0x9537300
local POOL1_OFFSET   = 0x986AC70

local KEYS = {
    gameplay  = 0x9537314,
    singleton = 0x95371B0,
    recording = 0x95371A4,
    subB      = 0x953707C,
    subC      = 0x9537080,
}

local function lookup(key_offset)
    local ctx = readPointer(base + CTX_PTR_OFFSET)
    if not ctx or ctx == 0 then return 0 end
    local map = readPointer(ctx + 0x10)
    if not map or map == 0 then return 0 end
    local sentinel = readPointer(map + 0x100)
    local mask     = readPointer(map + 0x128)
    local buckets  = readPointer(map + 0x110)
    if not buckets or buckets == 0 then return 0 end
    local key    = readInteger(base + key_offset) & 0xFFFFFFFF
    local bucket = buckets + (mask & key) * 0x10
    local first  = readPointer(bucket)
    local entry  = readPointer(bucket + 8)
    for _ = 1, 64 do
        if not entry or entry == 0 or entry == sentinel then break end
        if (readInteger(entry + 0x10) & 0xFFFFFFFF) == key then
            return readPointer(entry + 0x18) or 0
        end
        if entry == first then break end
        entry = readPointer(entry + 8)
    end
    return 0
end

-- Resolve all subsystem pointers once. They're stable for the lifetime
-- of the practice scene; we'll re-check before each snap as a sanity
-- guard but the lookup is cheap.
local subs = {}
for name, off in pairs(KEYS) do
    subs[name] = lookup(off)
    print(string.format("  %-9s = 0x%X", name, subs[name]))
end

if subs.singleton == 0 or subs.recording == 0 or subs.gameplay == 0 then
    print("Required subsystems not all resolved. Are you in practice mode?")
    return
end

-- Snapshot regions. Returns a single concat string so the comparison is
-- a fast string compare; on mismatch we re-walk byte-by-byte to find
-- the changed offsets.
local function snap_bytes(addr, len)
    if addr == 0 then return string.rep("\0", len) end
    return readBytes(addr, len, true) or {}
end

local function bytes_to_str(t)
    if type(t) == "string" then return t end
    local out = {}
    for i = 1, #t do out[i] = string.char(t[i]) end
    return table.concat(out)
end

local regions = {
    { name = "singleton[0..0xFF]",  base = subs.singleton, len = 0x100 },
    { name = "recording[0..0x7F]",  base = subs.recording, len = 0x80  },
    { name = "gameplay[0x440..0x500]", base = subs.gameplay + 0x440, len = 0xC0 },
    { name = "subB[0x60..0x70]",    base = subs.subB + 0x60, len = 0x10 },
    { name = "subC[0x250..0x260]",  base = subs.subC + 0x250, len = 0x10 },
}

-- Compute region absolute offsets for pretty display.
local region_offset_base = {
    ["singleton[0..0xFF]"]    = 0,
    ["recording[0..0x7F]"]    = 0,
    ["gameplay[0x440..0x500]"]= 0x440,
    ["subB[0x60..0x70]"]      = 0x60,
    ["subC[0x250..0x260]"]    = 0x250,
}

local function snap_all()
    local s = {}
    for _, r in ipairs(regions) do
        s[r.name] = bytes_to_str(snap_bytes(r.base, r.len))
    end
    return s
end

local function diff_strings(name, a, b)
    if a == b then return nil end
    local base_off = region_offset_base[name] or 0
    local out = {}
    local len = math.min(#a, #b)
    for i = 1, len do
        local va = a:byte(i)
        local vb = b:byte(i)
        if va ~= vb then
            out[#out+1] = string.format("  +0x%03X: 0x%02X -> 0x%02X",
                                        base_off + i - 1, va, vb)
        end
    end
    return out
end

print("")
print("Snapshotting baseline. Now perform your menu open/close cycles.")
print("Probe will run for 45 seconds.")
print("")

local t0 = nowMs()
local prev = snap_all()
print(string.format("=== T+0 baseline captured ==="))

local sample_interval = 100
local total_runtime_ms = 45000

while (nowMs() - t0) < total_runtime_ms do
    sleep(sample_interval)
    local cur = snap_all()
    local elapsed = nowMs() - t0
    for _, r in ipairs(regions) do
        local d = diff_strings(r.name, prev[r.name], cur[r.name])
        if d and #d > 0 then
            print(string.format("[T+%5dms] %s changed:", elapsed, r.name))
            for _, line in ipairs(d) do print(line) end
        end
    end
    prev = cur
end

print("")
print("done.")
