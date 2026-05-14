-- Read +0x24 of each practice subsystem to see what state pool_init's
-- `mov [rcx+0x24], rax` would normally clear.
--
-- Run this AFTER a fresh launch + entering practice mode + autoload has
-- run (i.e. the broken state: pool1 has data but the in-game UI doesn't
-- show recordings). We want to see which subsystem has a non-zero +0x24
-- — that's the likely "needs reinit" flag the dummy `this` failed to
-- clear.

local MODULE = "Polaris-Win64-Shipping.exe"

local function logf(fmt, ...) print(string.format(fmt, ...)) end

local base = getAddress(MODULE)
if not base or base == 0 then
    print("Polaris not loaded")
    return
end

-- Mirror opendojo::subsystems::lookup — service-locator hash-map walk.
local CTX_PTR_OFFSET = 0x9537300

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

local subsys = {
    { "gameplay",  0x9537314 },
    { "singleton", 0x95371B0 },
    { "subB",      0x953707C },
    { "subC",      0x9537080 },
    { "subD",      0x9537084 },
}

logf("Polaris base: 0x%X", base)
logf("pool1: 0x%X   pool2: 0x%X",
     readPointer(base + 0x986AC70) or 0,
     readPointer(base + 0x986AC78) or 0)
print("")

for _, s in ipairs(subsys) do
    local ptr = lookup(s[2])
    if ptr == 0 then
        logf("  %-9s = null  (not resolved — are you in practice?)", s[1])
    else
        local v24 = readPointer(ptr + 0x24) or 0
        -- Also dump a few neighboring qwords for context.
        local ctx = {}
        for off = 0, 7 do
            local q = readPointer(ptr + off * 8) or 0
            table.insert(ctx, string.format("+%02X=%016X", off * 8, q))
        end
        logf("  %-9s @ 0x%X   +0x24 = 0x%X", s[1], ptr, v24)
        logf("            head: %s", table.concat(ctx, "  "))
    end
end
print("")
print("done.")
