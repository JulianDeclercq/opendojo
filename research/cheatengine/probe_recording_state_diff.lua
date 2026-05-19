-- Snapshot every piece of state that we suspect controls "in-game UI
-- shows recordings + offers playback". Designed to be run twice and
-- diffed:
--
--   A) BROKEN state:  fresh launch, enter practice, autoload fired, F12
--      menu shows slot data but in-game UI says no recordings.
--   B) WORKING state: from A, open F12, click Replace on the same drill.
--      In-game UI now shows recordings.
--
-- Whatever differs between A and B is what manual Replace does that
-- our autoload doesn't — i.e. the state we're missing.

local MODULE = "Polaris-Win64-Shipping.exe"

local base = getAddress(MODULE)
if not base or base == 0 then
    print("Polaris not loaded — attach CE to Tekken first.")
    return
end

local CTX_PTR_OFFSET   = 0x9537300
local POOL1_PTR_OFFSET = 0x986AC70
local POOL2_PTR_OFFSET = 0x986AC78
local SLOT_PITCH       = 0x1C22

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

local KEYS = {
    { "gameplay",  0x9537314 },
    { "singleton", 0x95371B0 },
    { "recording", 0x95371A4 },
    { "subB",      0x953707C },
    { "subC",      0x9537080 },
    { "subD",      0x9537084 },
}

local function logf(fmt, ...) print(string.format(fmt, ...)) end

logf("Polaris base = 0x%X", base)
local p1 = readPointer(base + POOL1_PTR_OFFSET) or 0
local p2 = readPointer(base + POOL2_PTR_OFFSET) or 0
logf("pool1 = 0x%X   pool2 = 0x%X", p1, p2)

-- Event counts per slot (first 2 bytes of each slot in pool1).
if p1 ~= 0 then
    local ec = {}
    for i = 0, 7 do
        ec[#ec+1] = tostring(readShortInteger(p1 + i * SLOT_PITCH) & 0xFFFF)
    end
    logf("slot event_counts: [%s]", table.concat(ec, ", "))
end

print("")

-- Subsystem pointer + key fields.
local subs = {}
for _, k in ipairs(KEYS) do
    local ptr = lookup(k[2])
    subs[k[1]] = ptr
    logf("  %-9s = 0x%X", k[1], ptr)
end

print("")

-- gameplay: per-slot flag array at +0x480, 8-byte stride, flag at +4
local gp = subs.gameplay
if gp ~= 0 then
    local flags = {}
    for i = 0, 7 do
        local addr = gp + 0x480 + i * 8
        local sentinel = readInteger(addr)     & 0xFFFFFFFF  -- +0
        local flag     = readInteger(addr + 4) & 0xFFFFFFFF  -- +4
        flags[#flags+1] = string.format("[%d]=%d/%d", i, sentinel, flag)
    end
    logf("gameplay per-slot (sentinel/flag): %s", table.concat(flags, " "))
end

-- Other gameplay fields the game might care about — scan offsets we know
-- are written or read by various subsystem callers.
if gp ~= 0 then
    logf("gameplay misc:")
    for _, off in ipairs({0x047C, 0x0478, 0x0500, 0x0504}) do
        logf("  +0x%-4X = 0x%X", off, readInteger(gp + off) & 0xFFFFFFFF)
    end
end

print("")

-- singleton fields touched by the natural caller FUN_141913f00
local ss = subs.singleton
if ss ~= 0 then
    logf("singleton fields:")
    for _, spec in ipairs({
        {0x000, 4, "u32 word[0] (first dword, bit 22 = 0x400000)"},
        {0x002, 1, "u8  side gate"},
        {0x008, 1, "u8"},
        {0x022, 4, "u32 word[0x22]  (= 1 when natural caller actively recording)"},
        {0x023, 4, "u32 word[0x23]"},
        {0x024, 4, "u32 word[0x24]  (slot index)"},
        {0x025, 4, "u32 word[0x25]  (side query)"},
        {0x026, 1, "u8  byte[0x26]"},
        {0x099, 1, "u8  byte[0x99]  (= 1 when natural caller actively recording)"},
    }) do
        local off, sz, desc = spec[1], spec[2], spec[3]
        local v
        if sz == 1 then v = readBytes(ss + off, 1, false) or 0
        elseif sz == 4 then v = readInteger(ss + off) & 0xFFFFFFFF
        else v = 0 end
        logf("  +0x%-4X (%d B) = 0x%X    %s", off, sz, v, desc)
    end
end

print("")

-- recording subsystem fields touched by the natural caller's helper chain
local rec = subs.recording
if rec ~= 0 then
    logf("recording fields:")
    for _, spec in ipairs({
        {0x024, 4, "u32  pool_init writes 0 here"},
        {0x028, 4, "u32  pre_clear writes 0 here (gated)"},
        {0x05C, 4, "u32  post_init writes idx when side=0"},
        {0x060, 4, "u32  post_init writes idx when side!=0"},
        {0x064, 1, "u8   post_init writes side"},
    }) do
        local off, sz, desc = spec[1], spec[2], spec[3]
        local v
        if sz == 1 then v = readBytes(rec + off, 1, false) or 0
        elseif sz == 4 then v = readInteger(rec + off) & 0xFFFFFFFF
        else v = 0 end
        logf("  +0x%-4X (%d B) = 0x%X    %s", off, sz, v, desc)
    end
end

print("")

-- subB and subC — fields OpenDojo's set_recorded_flag writes to.
if subs.subB ~= 0 then
    local v = readBytes(subs.subB + 0x065, 1, false) or 0
    logf("subB  +0x065 = 0x%X", v)
end
if subs.subC ~= 0 then
    local v = readInteger(subs.subC + 0x25C) & 0xFFFFFFFF
    logf("subC  +0x25C = 0x%X (-1 means 0xFFFFFFFF)", v)
end

print("done.")
