-- Snapshot Tekken's practice subsystems to find the persistent "this slot
-- has a recording" indicator.
--
-- Usage (in the CE Lua engine):
--   1. Make sure all practice slots are erased and pool1 is allocated
--      (at least one practice recording session done this game launch).
--   2. snapshot("before") -- writes _before.bin
--   3. In Tekken: record into slot 1 normally → confirm the save dialog.
--   4. snapshot("after")  -- writes _after.bin
--   5. diff()             -- prints the bytes that changed
--
-- The bytes that changed (outside pool1 itself) are the candidate
-- "is recorded" indicator.

local SNAPSHOT_DIR = [[C:\Users\ethan\Desktop\openlab\cheatengine\snapshots]]

local function log(fmt, ...) print(string.format("[diff] " .. fmt, ...)) end

local function get_ctx_base()
    local base = getAddress("Polaris-Win64-Shipping.exe")
    if not base or base == 0 then return nil end
    return base
end

local function lookup_subsystem(key_offset)
    local base = get_ctx_base()
    if not base then return nil end
    local ctx = readQword(base + 0x9537300)
    local map = readQword(ctx + 0x10)
    local sentinel = readQword(map + 0x100)
    local mask = readQword(map + 0x128)
    local buckets = readQword(map + 0x110)
    local key = readInteger(base + key_offset)
    local bucket = buckets + (mask & key) * 0x10
    local first = readQword(bucket)
    local entry = readQword(bucket + 8)
    while entry ~= 0 and entry ~= sentinel do
        if readInteger(entry + 0x10) == key then
            return readQword(entry + 0x18)
        end
        if entry == first then break end
        entry = readQword(entry + 8)
    end
    return nil
end

local function regions()
    local base = get_ctx_base()
    local pool1 = readQword(base + 0x986AC70)
    local pool2 = readQword(base + 0x986AC78)
    return {
        { name = "gameplay",   addr = lookup_subsystem(0x9537314), size = 0x2000 },
        { name = "singleton",  addr = lookup_subsystem(0x95371b0), size = 0x2000 },
        { name = "controller", addr = lookup_subsystem(0x95371a4), size = 0x1000 },
        { name = "subA",       addr = lookup_subsystem(0x9537078), size = 0x1000 },
        { name = "subB",       addr = lookup_subsystem(0x953707c), size = 0x1000 },
        { name = "subC",       addr = lookup_subsystem(0x9537080), size = 0x1000 },
        { name = "subD",       addr = lookup_subsystem(0x9537084), size = 0x1000 },
        { name = "subE",       addr = lookup_subsystem(0x9537088), size = 0x1000 },
        { name = "pool1",      addr = pool1,                       size = 0xFD32 },
        { name = "pool2",      addr = pool2,                       size = 0x4FD2 },
    }
end

os.execute(string.format('if not exist "%s" mkdir "%s"',
    SNAPSHOT_DIR, SNAPSHOT_DIR))

function _G.snapshot(label)
    label = label or "snap"
    for _, r in ipairs(regions()) do
        if r.addr and r.addr ~= 0 then
            local bytes = readBytes(r.addr, r.size, true)
            local path = string.format("%s\\%s_%s.bin", SNAPSHOT_DIR, r.name, label)
            local f, err = io.open(path, "wb")
            if f then
                local chars = {}
                for i = 1, #bytes do chars[i] = string.char(bytes[i]) end
                f:write(table.concat(chars))
                f:close()
                log("wrote %s (%d bytes) addr=0x%X", path, #bytes, r.addr)
            else
                log("open %s failed: %s", path, tostring(err))
            end
        else
            log("region %s has no address", r.name)
        end
    end
end

function _G.diff(before, after)
    before = before or "before"
    after  = after  or "after"
    for _, r in ipairs(regions()) do
        local pa = string.format("%s\\%s_%s.bin", SNAPSHOT_DIR, r.name, before)
        local pb = string.format("%s\\%s_%s.bin", SNAPSHOT_DIR, r.name, after)
        local fa = io.open(pa, "rb")
        local fb = io.open(pb, "rb")
        if not fa or not fb then
            log("region %s: snapshot missing (a=%s b=%s)",
                r.name, tostring(fa ~= nil), tostring(fb ~= nil))
            if fa then fa:close() end
            if fb then fb:close() end
        else
            local da = fa:read("*all")
            local db = fb:read("*all")
            fa:close()
            fb:close()
            local n = math.min(#da, #db)
            local diff_count = 0
            local first_diff = -1
            local diffs = {}
            for i = 1, n do
                if da:byte(i) ~= db:byte(i) then
                    diff_count = diff_count + 1
                    if first_diff == -1 then first_diff = i - 1 end
                    if #diffs < 64 then
                        diffs[#diffs+1] = string.format("    +0x%X: %02X -> %02X",
                            i - 1, da:byte(i), db:byte(i))
                    end
                end
            end
            if diff_count == 0 then
                log("%s: no changes", r.name)
            else
                log("%s: %d bytes changed (first @ +0x%X)",
                    r.name, diff_count, first_diff)
                for _, d in ipairs(diffs) do log("%s", d) end
                if diff_count > 64 then
                    log("    (... %d more changed bytes)", diff_count - 64)
                end
            end
        end
    end
end

log("loaded. Use: snapshot('before') ; <do save in game> ; snapshot('after') ; diff()")
