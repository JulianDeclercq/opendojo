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

local function drill_path(slot_idx, ext)
    -- File names use 1-based, user-facing slot numbers.
    ext = ext or "drill"
    return OpenLab.DRILL_DIR .. "\\slot_" .. (slot_idx + 1) .. "." .. ext
end

-- ---------------------------------------------------------------------------
-- Drill text format (v1) — canonical, human-editable.
--
-- File shape:
--   # OpenLab drill v1
--   slot:         5
--   events:       9
--   total_frames: 142
--
--   # dir  buttons  frames  [annotations]
--     n    2          8   mark=3
--     n    .          2   mark=3
--     n    .         17
--     n    3+4        1
--     ...
--
-- Per-event columns (whitespace-separated):
--   dir      Tekken notation: n f b u d  uf df ub db.
--            Internally, byte 0 low nibble is a 4-bit direction mask:
--              bit 0 = up   bit 1 = down   bit 2 = forward   bit 3 = back
--            yielding n=0, u=1, d=2, f=4, uf=5, df=6, b=8, ub=9, db=10.
--            Invalid combos (e.g. up+down) are preserved via dir_raw=N.
--   buttons  1=LP 2=RP 3=LK 4=RK, combined with '+'. '.' means no buttons.
--   frames   duration in frames at 60fps (sum across all events = drill length).
--   annotations (optional, key=value):
--     mark=N      byte 0 high nibble, when not the default 0x2.
--     btn_raw=NN  bits of byte 1 outside the named 1/2/3/4 set (hex).
--     aux=NN      byte 2 (auxiliary state, partially decoded), default 0xA0.
--
-- '#' starts a comment to end-of-line. Blank lines are ignored. The header
-- (slot:, events:, total_frames:) is informational — only the event lines
-- determine the imported recording.
-- ---------------------------------------------------------------------------

local DEFAULT_MARK = 0x2    -- byte 0 high nibble for typical CPU events
local DEFAULT_AUX  = 0xA0   -- byte 2 baseline (idle / no animation state)

-- 4-bit direction mask: bit 0 = up, 1 = down, 2 = forward, 3 = back.
local DIR_TO_TEXT = {
    [0]  = "n",
    [1]  = "u",   [2]  = "d",
    [4]  = "f",   [5]  = "uf",  [6]  = "df",
    [8]  = "b",   [9]  = "ub",  [10] = "db",
}
local TEXT_TO_DIR = {
    n = 0,
    u = 1,   d  = 2,
    f = 4,   uf = 5,   df = 6,
    b = 8,   ub = 9,   db = 10,
}

-- Byte 1 bit layout, empirically determined by recording each button alone:
--   0x40 = 1 (LP)   0x80 = 2 (RP)
--   0x10 = 3 (LK)   0x20 = 4 (RK)
-- The bottom nibble (0x01..0x08) shows up rarely (slot_7 ev02 has 0x02 set)
-- and isn't decoded yet — preserved as btn_raw=NN.
local BTN_BIT_TO_NUM = { [0x40] = "1", [0x80] = "2", [0x10] = "3", [0x20] = "4" }
local BTN_NUM_TO_BIT = { ["1"]  = 0x40, ["2"] = 0x80, ["3"] = 0x10, ["4"] = 0x20 }

-- Encode in canonical 1,2,3,4 order regardless of bit order.
local BTN_EMIT_ORDER = { 0x40, 0x80, 0x10, 0x20 }

local function encode_buttons(byte1)
    local parts = {}
    for _, bit in ipairs(BTN_EMIT_ORDER) do
        if (byte1 & bit) ~= 0 then parts[#parts + 1] = BTN_BIT_TO_NUM[bit] end
    end
    return #parts == 0 and "." or table.concat(parts, "+"), byte1 & 0x0F
end

local function parse_buttons(token)
    if token == "." then return 0 end
    local mask = 0
    for sub in token:gmatch("[^+]+") do
        local bit = BTN_NUM_TO_BIT[sub]
        if not bit then return nil, "unknown button token: " .. sub end
        mask = mask | bit
    end
    return mask
end

local function encode_event_line(b0, b1, b2, b3)
    local mark = (b0 >> 4) & 0x0F
    local dir  = b0 & 0x0F
    local dir_text = DIR_TO_TEXT[dir]
    local ann = {}
    if not dir_text then
        dir_text = "n"
        ann[#ann + 1] = string.format("dir_raw=%X", dir)
    end
    local btn_str, btn_unknown = encode_buttons(b1)
    if mark ~= DEFAULT_MARK then ann[#ann + 1] = string.format("mark=%X",     mark)        end
    if btn_unknown ~= 0       then ann[#ann + 1] = string.format("btn_raw=%02X", btn_unknown) end
    if b2 ~= DEFAULT_AUX      then ann[#ann + 1] = string.format("aux=%02X",   b2)          end
    local line = string.format("  %-2s   %-5s  %4d", dir_text, btn_str, b3)
    if #ann > 0 then line = line .. "   " .. table.concat(ann, " ") end
    return line
end

local function encode_drill_text(bytes, slot_idx)
    local event_count = bytes[1] + bytes[2] * 256
    local total = 0
    for i = 0, event_count - 1 do
        total = total + bytes[3 + i * 4 + 3]
    end
    local lines = {
        "# OpenLab drill v1",
        string.format("slot:         %d", slot_idx + 1),
        string.format("events:       %d", event_count),
        string.format("total_frames: %d", total),
        "",
        "# dir   buttons  frames   [mark=N | btn_raw=NN | aux=NN | dir_raw=N]",
    }
    for i = 0, event_count - 1 do
        local off = 3 + i * 4
        lines[#lines + 1] =
            encode_event_line(bytes[off], bytes[off + 1], bytes[off + 2], bytes[off + 3])
    end
    return table.concat(lines, "\n") .. "\n"
end

local function parse_event_line(stripped)
    local dir_tok, btn_tok, frame_tok, rest =
        stripped:match("^(%S+)%s+(%S+)%s+(%S+)%s*(.-)$")
    if not dir_tok then return nil, "bad event line" end
    local dir = TEXT_TO_DIR[dir_tok]
    if not dir then return nil, "unknown direction: " .. dir_tok end
    local btn_mask, err = parse_buttons(btn_tok)
    if not btn_mask then return nil, err end
    local frames = tonumber(frame_tok)
    if not frames or frames < 0 or frames > 255 then
        return nil, "bad frame count: " .. frame_tok
    end
    local mark, btn_raw, aux, dir_raw = DEFAULT_MARK, 0, DEFAULT_AUX, nil
    for k, v in (rest or ""):gmatch("([%w_]+)%s*=%s*(%w+)") do
        local n = tonumber(v, 16)
        if not n then return nil, "bad annotation value: " .. k .. "=" .. v end
        if     k == "mark"    then mark    = n
        elseif k == "btn_raw" then btn_raw = n
        elseif k == "aux"     then aux     = n
        elseif k == "dir_raw" then dir_raw = n
        else return nil, "unknown annotation: " .. k end
    end
    if dir_raw then dir = dir_raw end
    return {
        ((mark & 0xF) << 4) | (dir & 0xF),
        btn_mask | btn_raw,
        aux,
        frames,
    }
end

local function decode_drill_text(text)
    local events = {}
    for raw_line in (text .. "\n"):gmatch("([^\r\n]*)\r?\n") do
        local stripped = raw_line:gsub("#.*$", "")
                                  :gsub("^%s+", "")
                                  :gsub("%s+$", "")
        -- Skip blanks and header lines (key: value).
        if stripped ~= "" and not stripped:match("^[%w_]+%s*:%s*%S") then
            local ev, err = parse_event_line(stripped)
            if not ev then
                return nil, (err or "parse error") .. " — line: " .. raw_line
            end
            events[#events + 1] = ev
        end
    end
    local max_events = (OpenLab.SLOT_PITCH - 2) // 4
    if #events > max_events then
        return nil, string.format("too many events: %d (max %d)", #events, max_events)
    end
    local data = {}
    data[1] = #events & 0xFF
    data[2] = (#events >> 8) & 0xFF
    for i, e in ipairs(events) do
        local off = 3 + (i - 1) * 4
        data[off]     = e[1]
        data[off + 1] = e[2]
        data[off + 2] = e[3]
        data[off + 3] = e[4]
    end
    for i = #data + 1, OpenLab.SLOT_PITCH do data[i] = 0 end
    return data
end

-- Parse the legacy 7218-byte binary container. Returns a byte table of the
-- 7202-byte payload, or (nil, err).
local function parse_drill_binary(content)
    local need = OpenLab.HEADER_SIZE + OpenLab.SLOT_PITCH
    if #content < need then
        return nil, string.format("too small: %d bytes (need %d)", #content, need)
    end
    if content:sub(1, 4) ~= OpenLab.MAGIC then
        return nil, "bad magic " .. string.format("%q", content:sub(1, 4))
    end
    local version = unpack_le32(content, 5)
    if version ~= OpenLab.VERSION then
        return nil, string.format("unsupported version %d (this is v%d)",
                                  version, OpenLab.VERSION)
    end
    local payload = content:sub(OpenLab.HEADER_SIZE + 1,
                                OpenLab.HEADER_SIZE + OpenLab.SLOT_PITCH)
    return string_to_bytes(payload)
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

    -- Canonical text drill.
    local text = encode_drill_text(bytes, slot_idx)
    local text_path = drill_path(slot_idx)
    local ft, err = io.open(text_path, "wb")
    if not ft then
        log("open(%q) failed: %s", text_path, tostring(err))
        return false
    end
    ft:write(text)
    ft:close()

    -- Binary safety net (byte-clone).
    local bin_path = drill_path(slot_idx, "bin")
    local fb, berr = io.open(bin_path, "wb")
    if not fb then
        log("open(%q) failed: %s", bin_path, tostring(berr))
        return false
    end
    fb:write(OpenLab.MAGIC)
    fb:write(pack_le32(OpenLab.VERSION))
    fb:write(pack_le32(slot_idx))
    fb:write(pack_le32(0))
    fb:write(bytes_to_string(bytes))
    fb:close()

    log("exported slot %d (%d events) -> slot_%d.drill + slot_%d.bin",
        slot_idx + 1, n, slot_idx + 1, slot_idx + 1)
    return true
end

local function import_slot(slot_idx)
    local text_path = drill_path(slot_idx)
    local bin_path  = drill_path(slot_idx, "bin")
    local data, source

    local ft = io.open(text_path, "rb")
    if ft then
        local content = ft:read("*all")
        ft:close()
        if content:sub(1, 4) == OpenLab.MAGIC then
            -- Legacy binary stored under the .drill extension. Auto-promote on
            -- next export by leaving the new files in place when we write back.
            local payload, perr = parse_drill_binary(content)
            if not payload then
                log("%s (legacy binary): %s", text_path, perr)
                return false
            end
            data, source = payload, text_path .. " (legacy binary)"
        else
            local decoded, derr = decode_drill_text(content)
            if not decoded then
                log("%s: %s", text_path, derr)
                return false
            end
            data, source = decoded, text_path
        end
    else
        local fb = io.open(bin_path, "rb")
        if not fb then
            log("neither %s nor %s found", text_path, bin_path)
            return false
        end
        local content = fb:read("*all")
        fb:close()
        local payload, perr = parse_drill_binary(content)
        if not payload then
            log("%s: %s", bin_path, perr)
            return false
        end
        data, source = payload, bin_path
    end

    local addr = slot_address(slot_idx)
    if not addr then return false end
    writeBytes(addr, data)
    if not set_recorded_flags(slot_idx, true) then
        log("WARNING: pool1 written but flags not set — menu may show 'Not Set'")
    end
    local event_count = data[1] + data[2] * 256
    log("imported %s -> slot %d (%d events). Close+reopen the practice menu to see the update.",
        source, slot_idx + 1, event_count)
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

-- Round-trip check: read a slot, encode to text, decode back, and compare to
-- the original bytes. Returns true if they match.
local function round_trip_check(slot_idx)
    local addr = slot_address(slot_idx)
    if not addr then return false end
    local original = readBytes(addr, OpenLab.SLOT_PITCH, true)
    if not original then return false end
    local n = event_count_from_bytes(original)
    if n == 0 then
        log("slot %d is empty — nothing to check", slot_idx + 1)
        return false
    end
    local text = encode_drill_text(original, slot_idx)
    local decoded, err = decode_drill_text(text)
    if not decoded then
        log("round-trip decode failed: %s", err)
        return false
    end
    -- Compare only the meaningful prefix (event count header + N event records).
    -- The trailing pad is zeros in both — we don't require those to match if the
    -- original had any pre-zero garbage past the events (the game shouldn't).
    local meaningful = 2 + n * 4
    for i = 1, meaningful do
        if original[i] ~= decoded[i] then
            log("round-trip MISMATCH at byte %d: original=0x%02X decoded=0x%02X",
                i - 1, original[i], decoded[i])
            return false
        end
    end
    log("round-trip OK for slot %d (%d events, %d bytes compared)",
        slot_idx + 1, n, meaningful)
    return true
end

-- Make the API accessible from the CE Lua console for manual testing.
_G.OpenLab = {
    export_slot       = export_slot,
    import_slot       = import_slot,
    show_status       = show_status,
    round_trip_check  = round_trip_check,
    encode_drill_text = encode_drill_text,
    decode_drill_text = decode_drill_text,
    config            = OpenLab,
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
