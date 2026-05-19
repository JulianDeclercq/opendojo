-- Find every CALL to Polaris+0x18E8E00 (pool_init) and dump the bytes
-- immediately before each call. Those bytes tell us how the natural
-- callers set up RCX (= the `this` arg pool_init expects).

local MODULE          = "Polaris-Win64-Shipping.exe"
local TARGET_RVA      = 0x18E8E00
local BEFORE_BYTES    = 24
local AFTER_BYTES     = 8

local function logf(fmt, ...) print(string.format(fmt, ...)) end

local base = getAddress(MODULE)
if not base or base == 0 then
    print("Polaris not loaded")
    return
end
local target_abs = base + TARGET_RVA
logf("Scanning for callers of 0x%X (Polaris+0x%X) ...", target_abs, TARGET_RVA)

-- Every near CALL is 5 bytes: 0xE8 + signed i32 disp. Target = addr+5+disp.
-- AOBScan returns all 0xE8 occurrences; we filter by computed target.
local results = AOBScan("E8 ?? ?? ?? ??", "+X-W")
if not results then
    print("AOBScan returned nil")
    return
end

local hits = 0
for i = 0, results.Count - 1 do
    local addr = tonumber("0x" .. results[i])
    -- Guard: the first byte must actually be 0xE8 (AOBScan with leading
    -- 0xE8 should guarantee this, but be defensive in case of overlapping
    -- matches inside instruction streams).
    local opc = readBytes(addr, 1, false) or 0
    if opc == 0xE8 then
        local disp = readInteger(addr + 1)
        if addr + 5 + disp == target_abs then
            hits = hits + 1
            local before = {}
            for off = -BEFORE_BYTES, -1 do
                local b = readBytes(addr + off, 1, false) or 0
                table.insert(before, string.format("%02X", b))
            end
            local at = {}
            for off = 0, AFTER_BYTES - 1 do
                local b = readBytes(addr + off, 1, false) or 0
                table.insert(at, string.format("%02X", b))
            end
            -- Walk back for function start (CC CC padding sentinel).
            local fstart = nil
            for back = 1, 0x600 do
                local b1 = readBytes(addr - back,     1, false) or 0
                local b2 = readBytes(addr - back - 1, 1, false) or 0
                if b1 == 0xCC and b2 == 0xCC then
                    fstart = addr - back + 1
                    break
                end
            end
            logf("  [%d] call site: 0x%X  (Polaris+0x%X)",
                 hits, addr, addr - base)
            if fstart then
                logf("       func start: 0x%X  (Polaris+0x%X)",
                     fstart, fstart - base)
            end
            logf("       before: %s", table.concat(before, " "))
            logf("       at:     %s", table.concat(at, " "))
        end
    end
end

results.destroy()
logf("done. %d caller(s) found.", hits)
