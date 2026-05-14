-- OpenDojo character-detection probe.
--
-- Self-contained — paste into CE's Lua Engine and execute. Prints the
-- resolved player-holder pointer, both player struct addresses, both
-- character_ids, and which side the human is on.
--
-- Method: replicates Irony's pattern-scan approach to find a stable code
-- reference that writes the GlobalPlayerHolder pointer, then walks the
-- chain to Player1 / Player2 / character_id (offset 0x168, u32).
--
-- Pre-flight:
--   1. Attach CE to Polaris-Win64-Shipping.exe.
--   2. Be in a practice match (P1 + CPU loaded). Outside a match the
--      holder is null and the script will say so.
--   3. Run this script. Output lands in CE's Lua Engine bottom pane.
--
-- Validation workflow:
--   - Run once, note IDs. Pause the game, change CPU character via the
--     practice menu, resume, re-run. The P2 ID should change; P1's
--     should stay. Cycle every roster character to build the ID->name
--     table.

local MODULE = "Polaris-Win64-Shipping.exe"

-- 1) Loads GlobalPlayerHolder pointer to R14 then writes a byte to [r14+0x28].
--    Bytes: 4C 89 35 ?? ?? ?? ??    MOV [rip+disp32], r14
--           41 88 5E 28             MOV [r14+0x28], BL
-- The disp32 at +3 gives us the absolute address of the holder pointer slot.
local PAT_PLAYERS = "4C 89 35 ?? ?? ?? ?? 41 88 5E 28"

-- 2) Loads the main_player_info pointer from a global. The 4-byte disp32
--    at +9 is the absolute address of that global pointer slot.
local PAT_MAIN_INFO = "40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? BA 01 00 00 00"

-- Player struct field offsets (Irony T8).
local OFF_CHARACTER_ID = 0x168    -- u32
local OFF_INPUT_SIDE   = 0x27EC   -- u8: 0=left, 1=right

-- PlayerInfo field offsets (Irony T8).
local OFF_PLAYER_ID    = 0x05     -- u8: 0=P1 slot, 1=P2 slot

local function logf(fmt, ...)
    print(string.format(fmt, ...))
end

local function get_module_base()
    local ok, base = pcall(getAddress, MODULE)
    if not ok or not base or base == 0 then return nil end
    return base
end

-- Resolve a 4-byte RIP-relative displacement at `at`.
-- The CPU computes the target as: (at + 4) + signed(disp32).
local function rip_relative(at)
    local disp = readInteger(at)            -- signed i32
    return at + 4 + disp
end

-- Run an AOB scan inside Polaris's image, return the first match address
-- or nil. Restricts to executable, non-writable pages.
local function aob_first(pattern)
    local results = AOBScan(pattern, "+X-W")
    if results == nil then return nil end
    local addr = nil
    if results.Count and results.Count >= 1 then
        addr = tonumber("0x" .. results[0])
        if results.Count > 1 then
            logf("WARN: pattern matched %d times, using first (0x%X). Pattern: %s",
                 results.Count, addr, pattern)
        end
    end
    results.destroy()
    return addr
end

-- Safe pointer read: returns 0 on null/unreadable.
local function safe_ptr(addr)
    if not addr or addr == 0 then return 0 end
    local ok, p = pcall(readPointer, addr)
    if not ok or not p then return 0 end
    return p
end

local function safe_u32(addr)
    if not addr or addr == 0 then return nil end
    local ok, v = pcall(readInteger, addr)
    if not ok or not v then return nil end
    return v & 0xFFFFFFFF
end

local function safe_u8(addr)
    if not addr or addr == 0 then return nil end
    local ok, v = pcall(readBytes, addr, 1, false)
    if not ok or not v then return nil end
    return v
end

-- ---------------------------------------------------------------------------

print("=== OpenDojo character-detection probe ===")

local base = get_module_base()
if not base then
    print("ERROR: Polaris-Win64-Shipping.exe is not loaded. Attach CE to Tekken first.")
    return
end
logf("Polaris base                 = 0x%X", base)

-- Resolve pattern 1: GlobalPlayerHolder pointer slot.
local pat_players_at = aob_first(PAT_PLAYERS)
if not pat_players_at then
    print("ERROR: PAT_PLAYERS not found. Pattern may have drifted on this game version.")
    return
end
logf("PAT_PLAYERS site             = 0x%X", pat_players_at)

local holder_global = rip_relative(pat_players_at + 3)
logf("GlobalPlayerHolder*-addr     = 0x%X", holder_global)

local holder = safe_ptr(holder_global)
logf("GlobalPlayerHolder           = 0x%X%s", holder,
     holder == 0 and "  (null — are you in a practice match?)" or "")

if holder ~= 0 then
    local p1 = safe_ptr(holder + 0x30)
    local p2 = safe_ptr(holder + 0x38)
    logf("Player1*                     = 0x%X", p1)
    logf("Player2*                     = 0x%X", p2)

    if p1 ~= 0 then
        local cid = safe_u32(p1 + OFF_CHARACTER_ID)
        local side = safe_u8(p1 + OFF_INPUT_SIDE)
        logf("Player1.character_id         = %s  (0x%X)",
             cid and tostring(cid) or "?", cid or 0)
        logf("Player1.input_side           = %s  (0=left, 1=right)",
             side and tostring(side) or "?")
    end
    if p2 ~= 0 then
        local cid = safe_u32(p2 + OFF_CHARACTER_ID)
        local side = safe_u8(p2 + OFF_INPUT_SIDE)
        logf("Player2.character_id         = %s  (0x%X)",
             cid and tostring(cid) or "?", cid or 0)
        logf("Player2.input_side           = %s  (0=left, 1=right)",
             side and tostring(side) or "?")
    end
end

-- Resolve pattern 2: main_player_info chain.
print("")
local pat_info_at = aob_first(PAT_MAIN_INFO)
if not pat_info_at then
    print("NOTE: PAT_MAIN_INFO not found. Skipping human-side detection via player_info.")
else
    logf("PAT_MAIN_INFO site           = 0x%X", pat_info_at)
    local info_global = rip_relative(pat_info_at + 9)
    logf("main_player_info*-addr       = 0x%X", info_global)

    -- Irony's trail [info_global, 0x0, 0x0, 0x20, 0x0]:
    --   *info_global              = lvl1
    --   *(lvl1 + 0x0)             = lvl2
    --   *(lvl2 + 0x0)             = lvl3
    --   *(lvl3 + 0x20)            = info_ptr
    --   info_ptr + 0x0            = info_ptr
    local lvl1 = safe_ptr(info_global)
    local lvl2 = safe_ptr(lvl1 + 0x0)
    local lvl3 = safe_ptr(lvl2 + 0x0)
    local info = safe_ptr(lvl3 + 0x20)
    logf("main_player_info             = 0x%X", info)

    if info ~= 0 then
        local pid = safe_u8(info + OFF_PLAYER_ID)
        logf("main_player_info.player_id   = %s  (0=human is P1, 1=human is P2)",
             pid and tostring(pid) or "?")
        if pid == 0 then
            print("  -> CPU is Player2; read CPU character_id from Player2.character_id above.")
        elseif pid == 1 then
            print("  -> CPU is Player1; read CPU character_id from Player1.character_id above.")
        end
    end
end

print("=== probe done ===")
