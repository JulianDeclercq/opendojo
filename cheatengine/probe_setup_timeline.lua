-- Capture the timeline of the game's practice-mode setup writes, so we
-- can replace our 5-second guess-delay with a deterministic "setup
-- complete" signal.
--
-- The wait gate is the Player-struct holder (the same chain the mod uses
-- via players::detect_cpu). That pointer is 0 on title / main menu /
-- character select and becomes non-zero exactly when a match scene
-- starts populating. KEY_GAMEPLAY is NOT a practice gate — it's
-- non-zero everywhere after game init, including on the title screen.
--
-- Usage:
--   1. Disable autosave first (F12 → Status → uncheck) so OUR writes
--      don't pollute the timeline; we want the game's natural setup
--      writes only.
--   2. Get to title screen / main menu (out of any match).
--   3. Load + Execute this script in CE's Lua Engine. It will block.
--   4. Enter practice mode in the game.
--   5. The script captures snapshots and prints deltas.

local MODULE = "Polaris-Win64-Shipping.exe"
local base = getAddress(MODULE)
if not base or base == 0 then
    print("Polaris not loaded — attach CE to Tekken first.")
    return
end

local function nowMs() return getTickCount() end

-- ---------------------------------------------------------------------------
-- Locate the Player-struct holder slot. The holder POINTER itself is
-- non-null from very early in game init (it's a long-lived context
-- object), but `holder[0x30]` (= Player1 pointer) is the actual
-- "match-is-loaded" signal — 0 on title / main menu / character select,
-- non-zero when a match's player struct is populated. This mirrors
-- detect_cpu()'s own check (if (!p1 || !p2) return).
--
-- Hardcoded RVA verified on Tekken 8 v3.00.02 (derived from
-- probe_chars.lua). If the game updates, re-run probe_chars.lua.
-- ---------------------------------------------------------------------------
local HOLDER_SLOT_RVA = 0x9B7A950
local HOLDER_P1_OFFS  = 0x30
local holder_slot     = base + HOLDER_SLOT_RVA

local function readPlayer1()
    local h = readPointer(holder_slot) or 0
    if h == 0 then return 0 end
    return readPointer(h + HOLDER_P1_OFFS) or 0
end

print(string.format("Player holder slot: 0x%X   holder value: 0x%X   Player1*: 0x%X",
                    holder_slot,
                    readPointer(holder_slot) or 0,
                    readPlayer1()))

local CTX_PTR_OFFSET   = 0x9537300
local POOL1_PTR_OFFSET = 0x986AC70
local POOL2_PTR_OFFSET = 0x986AC78

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

local function safeU8(addr)  return addr ~= 0 and (readBytes(addr, 1, false) or 0) or 0 end
local function safeU32(addr) return addr ~= 0 and (readInteger(addr) & 0xFFFFFFFF) or 0 end
local function safeU64(addr) return addr ~= 0 and (readPointer(addr) or 0) or 0 end

local function snapshot()
    local s = {}
    local h = safeU64(holder_slot)
    s["_holder"]    = h
    s["_player1"]   = h ~= 0 and safeU64(h + 0x30) or 0
    s["_player2"]   = h ~= 0 and safeU64(h + 0x38) or 0
    s["_pool1"]     = safeU64(base + POOL1_PTR_OFFSET)
    s["_pool2"]     = safeU64(base + POOL2_PTR_OFFSET)

    local gp = lookup(KEYS.gameplay)
    s["gameplay_ptr"] = gp
    if gp ~= 0 then
        for i = 0, 7 do
            local a = gp + 0x480 + i * 8
            s[string.format("gp_slot[%d]_sent", i)] = safeU32(a)
            s[string.format("gp_slot[%d]_flag", i)] = safeU32(a + 4)
        end
        for _, off in ipairs({0x478, 0x47C, 0x4C0, 0x4C4, 0x500, 0x504}) do
            s[string.format("gp_+0x%03X", off)] = safeU32(gp + off)
        end
    end

    local ss = lookup(KEYS.singleton)
    s["singleton_ptr"] = ss
    if ss ~= 0 then
        s["ss_word0"]   = safeU32(ss + 0x000)
        s["ss_+0x002"]  = safeU8 (ss + 0x002)
        s["ss_+0x008"]  = safeU8 (ss + 0x008)
        s["ss_+0x022"]  = safeU32(ss + 0x022)
        s["ss_+0x024"]  = safeU32(ss + 0x024)
        s["ss_+0x025"]  = safeU32(ss + 0x025)
        s["ss_+0x026"]  = safeU8 (ss + 0x026)
        s["ss_+0x099"]  = safeU8 (ss + 0x099)
    end

    local rec = lookup(KEYS.recording)
    s["recording_ptr"] = rec
    if rec ~= 0 then
        s["rec_+0x024"] = safeU32(rec + 0x024)
        s["rec_+0x028"] = safeU32(rec + 0x028)
        s["rec_+0x05C"] = safeU32(rec + 0x05C)
        s["rec_+0x060"] = safeU32(rec + 0x060)
        s["rec_+0x064"] = safeU8 (rec + 0x064)
    end

    local sb = lookup(KEYS.subB)
    s["subB_ptr"] = sb
    if sb ~= 0 then s["subB_+0x065"] = safeU8(sb + 0x065) end

    local sc = lookup(KEYS.subC)
    s["subC_ptr"] = sc
    if sc ~= 0 then s["subC_+0x25C"] = safeU32(sc + 0x25C) end

    return s
end

-- ---------------------------------------------------------------------------
-- Block until Player1* transitions from 0 -> non-zero. That's the actual
-- "match scene is populating" signal.
-- ---------------------------------------------------------------------------
print("")
print("Waiting for match entry (Player1 pointer transitions to non-zero).")
print("Enter PRACTICE MODE now.")

-- If Player1* is already non-zero at script start, the user is already
-- in a match — wait for them to leave and re-enter for a clean baseline.
if readPlayer1() ~= 0 then
    print("Already in a match. Exit to title; the probe will start when you")
    print("re-enter practice.")
    while readPlayer1() ~= 0 do sleep(20) end
    print("Match cleared. Now enter practice.")
end

-- Wait for Player1* to become non-zero.
while readPlayer1() == 0 do sleep(15) end

local t0 = nowMs()
print(string.format("Match entry detected at tick %d. Snapshotting...", t0))

local schedule = { 0, 100, 300, 600, 1000, 1500, 2000, 3000, 5000, 7500, 10000 }
local snaps = {}
for _, t in ipairs(schedule) do
    while (nowMs() - t0) < t do sleep(5) end
    snaps[#snaps+1] = { t = t, data = snapshot() }
end

print("")
print("=== T+0 baseline ===")
local keys = {}
for k in pairs(snaps[1].data) do keys[#keys+1] = k end
table.sort(keys)
for _, k in ipairs(keys) do
    print(string.format("  %-26s = 0x%X", k, snaps[1].data[k]))
end

for i = 2, #snaps do
    local prev = snaps[i-1]
    local cur  = snaps[i]
    print("")
    print(string.format("=== Δ at T+%dms (vs T+%dms) ===", cur.t, prev.t))
    local any = false
    for _, k in ipairs(keys) do
        if cur.data[k] ~= prev.data[k] then
            any = true
            print(string.format("  %-26s : 0x%X -> 0x%X", k, prev.data[k], cur.data[k]))
        end
    end
    if not any then print("  (no changes — state is stable)") end
end

print("")
print("done.")
