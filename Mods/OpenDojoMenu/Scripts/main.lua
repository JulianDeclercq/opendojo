-- OpenDojoMenu — practice-pause-menu entry-point mod.
--
-- Responsibilities:
--   1. Insert an "OpenDojo" row into the practice pause menu's left pane
--      (AddButton1Data + UpdateListView1 on WBP_UI_PracticeMenu_C).
--   2. Bypass Gryphon localization for our row so the label renders as
--      "OpenDojo" instead of "err(OpenDojo)@@@@" — accomplished by a
--      post-hook on UPolarisTextBlock:SetTextID that re-applies
--      SetRawText("OpenDojo") whenever an OpenDojo-bound TB updates.
--   3. When the user clicks our row, signal the OpenDojo DLL to open
--      the ImGui menu. The DLL polls for `opendojo_open_menu.flag` next
--      to the game exe each frame; touching the file is the IPC.
--
-- Hotkey:
--   F4 — insert OpenDojo row + arm both hooks. Idempotent — safe to
--        press again after re-entering the practice menu (the practice
--        menu rebuilds its widget tree on every open, but the inserted
--        row state is per-instance so re-pressing F4 re-arms).
--
-- The native dialog path that previously fired on row click is removed;
-- it was a Phase-1 exploration that the DLL has now superseded.

local MOD = "OpenDojoMenu"

local function log(msg) print(string.format("[%s] %s\n", MOD, msg)) end

local function fmt_obj(o)
    if not o then return "<nil>" end
    local ok_a, a = pcall(function() return o:GetAddress() end)
    local ok_n, n = pcall(function() return o:GetFullName() end)
    return string.format("0x%X %s",
        ok_a and a or 0, ok_n and tostring(n) or "<no name>")
end

local function err_to_str(e, depth)
    depth = depth or 0
    if depth > 6 then return "<deep err>" end
    if type(e) == "string" then return e end
    if type(e) == "function" then
        local ok, v = pcall(e)
        if not ok then return "<fn raised>" end
        return err_to_str(v, depth + 1)
    end
    if type(e) == "table" and e.message then return tostring(e.message) end
    return tostring(e)
end

local function find_menu()
    return FindFirstOf("WBP_UI_PracticeMenu_C")
        or FindFirstOf("PolarisUMGPracticeMenu")
        or FindFirstOf("UPolarisUMGPracticeMenu")
end

local function read_text_prop(v)
    if v == nil then return nil end
    if type(v) == "string" then return v end
    if type(v) == "userdata" and v.ToString then
        local ok, t = pcall(function() return v:ToString() end)
        if ok and t and t ~= "" then return t end
    end
    return tostring(v)
end

local function find_opendojo_row()
    for _, row in ipairs(FindAllOf("WBP_UI_PracticeMenu_Button_1_C") or {}) do
        local item; pcall(function() item = row.list_item end)
        if item then
            local txt; pcall(function() txt = read_text_prop(item.Text) end)
            if txt == "OpenDojo" then return row end
        end
    end
    return nil
end

-- DLL IPC. Write a flag file next to the game executable. The OpenDojo
-- DLL (proxy dinput8.dll) polls for this file each frame and toggles
-- the ImGui menu when present, deleting the file on read.
--
-- Path: the game exe lives at .../Polaris/Binaries/Win64/Polaris-Win64-Shipping.exe.
-- The DLL drops the flag into that same directory. This script lives at
-- .../Polaris/Binaries/Win64/Mods/OpenDojoMenu/Scripts/main.lua, so we walk
-- three directories up from the script's own location to build an absolute
-- path that doesn't depend on process CWD.
local g_flag_path_cached = nil
local function dll_flag_path()
    if g_flag_path_cached then return g_flag_path_cached end
    local src = debug.getinfo(1, "S").source or ""
    if src:sub(1, 1) == "@" then src = src:sub(2) end
    -- src = "<...>/Mods/OpenDojoMenu/Scripts/main.lua"
    -- Walk three "/parent" hops to reach <...> = the Win64 directory.
    local parent = src:gsub("[/\\][^/\\]+$", "")  -- strip /main.lua
    parent       = parent:gsub("[/\\][^/\\]+$", "")  -- strip /Scripts
    parent       = parent:gsub("[/\\][^/\\]+$", "")  -- strip /OpenDojoMenu
    parent       = parent:gsub("[/\\][^/\\]+$", "")  -- strip /Mods
    g_flag_path_cached = parent .. "/opendojo_open_menu.flag"
    log("flag path resolved -> " .. g_flag_path_cached)
    return g_flag_path_cached
end

local function signal_dll_open_menu()
    local path = dll_flag_path()
    local f, err = io.open(path, "wb")
    if not f then
        log("signal: io.open FAIL path=" .. tostring(path)
            .. " err=" .. tostring(err))
        return
    end
    f:close()
    log("signal: wrote " .. path .. " — DLL should toggle ImGui menu next frame")
end

-- Per-session state (carried across F4 presses).
local g_our_tb_addrs    = {}     -- addr -> true: which TB widgets carry "OpenDojo"
local g_setrawtext_hooked = false
local g_decide_hooked   = false
local g_our_row_idx     = nil    -- pane-1 index, set at insert time

local function capture_and_hook(row)
    local off, on
    pcall(function() off = row.TB_Menu_OFF end)
    pcall(function() on  = row.TB_Menu_ON  end)

    local addr_off, addr_on
    if off then pcall(function() addr_off = off:GetAddress() end) end
    if on  then pcall(function() addr_on  = on :GetAddress() end) end
    if addr_off then
        g_our_tb_addrs[addr_off] = true
        log(string.format("  TB_Menu_OFF @ 0x%X", addr_off))
    end
    if addr_on then
        g_our_tb_addrs[addr_on] = true
        log(string.format("  TB_Menu_ON  @ 0x%X", addr_on))
    end

    -- Immediate label fix so the user doesn't see "err(OpenDojo)@@@@".
    if off then pcall(function() off:SetRawText("OpenDojo", false) end) end
    if on  then pcall(function() on :SetRawText("OpenDojo", false) end) end
    log("  SetRawText applied")

    -- Persistent label fix: every time UPolarisTextBlock:SetTextID runs
    -- on a TB whose address we captured, re-apply SetRawText. Hover and
    -- focus state-changes go through SetTextID; without this the err()
    -- label flicks back on each transition.
    if not g_setrawtext_hooked then
        local poly_path = "/Script/Polaris.PolarisTextBlock:SetTextID"
        local ok, err = pcall(function()
            RegisterHook(poly_path,
                function(self_w, id_w) end,
                function(self_w, id_w)
                    local self; pcall(function() self = self_w:get() end)
                    if not self then return end
                    local addr; pcall(function() addr = self:GetAddress() end)
                    if not addr or not g_our_tb_addrs[addr] then return end
                    pcall(function()
                        self:SetRawText("OpenDojo", false)
                    end)
                end)
        end)
        g_setrawtext_hooked = ok
        log(string.format("  SetTextID hook -> %s err=%s",
            ok and "ok" or "FAIL", ok and "-" or err_to_str(err)))
    end
end

local function listview1_count(menu)
    local n
    pcall(function() n = menu.ListView_1.ListItems:GetArrayNum() end)
    return n
end

local function menu_class_path(menu)
    local full
    pcall(function() full = menu:GetClass():GetFullName() end)
    if not full then return nil end
    return tostring(full):match("(/Game/[^%s]+)")
end

local function install_decide_hook(class_path)
    if g_decide_hooked then return end
    local hp = class_path .. ":OnDecideButton1"
    local ok, err = pcall(function()
        RegisterHook(hp,
            function(self_w, id_w)
                if g_our_row_idx == nil then return end
                local id
                pcall(function() id = id_w:get() end)
                if id == g_our_row_idx then
                    log(string.format("OnDecideButton1 ID=%d -> OpenDojo", id))
                    signal_dll_open_menu()
                end
            end,
            function(self_w, id_w) end)
    end)
    g_decide_hooked = ok
    log(string.format("OnDecideButton1 hook -> %s err=%s",
        ok and "ok" or "FAIL", ok and "-" or err_to_str(err)))
end

RegisterKeyBind(Key.F4, function()
    log("=== F4: insert OpenDojo row + arm DLL signal hook ===")
    local m = find_menu()
    if not m then log("  no menu — open practice pause menu first"); return end

    -- OnDecideButton1 lives on the menu's BP class; install once we have
    -- the class path. Safe to call before the row exists.
    local cls_path = menu_class_path(m)
    if cls_path then install_decide_hook(cls_path) end

    -- Idempotent fast path: if the row is already there (re-entry to the
    -- practice menu), just re-capture TBs and update g_our_row_idx.
    local row = find_opendojo_row()
    if row then
        log("  OpenDojo row already exists @ " .. fmt_obj(row))
        -- Refresh row index in case the menu rebuilt with a different layout.
        local n = listview1_count(m)
        if n then g_our_row_idx = n - 1 end
        capture_and_hook(row)
        return
    end

    -- Capture the eventual ID *before* the insert. UListView appends at
    -- end, so our row will land at the pre-insert count (zero-indexed).
    local count_before = listview1_count(m)
    if count_before then
        g_our_row_idx = count_before
        log(string.format("  our row will land at index %d", g_our_row_idx))
    end

    local ok_a, err_a = pcall(function() m:AddButton1Data("OpenDojo", true) end)
    log(string.format("  AddButton1Data -> %s err=%s",
        ok_a and "ok" or "FAIL", ok_a and "-" or err_to_str(err_a)))
    local ok_u, err_u = pcall(function() m:UpdateListView1() end)
    log(string.format("  UpdateListView1 -> %s err=%s",
        ok_u and "ok" or "FAIL", ok_u and "-" or err_to_str(err_u)))

    if not LoopAsync then
        log("  LoopAsync unavailable — press F4 again to capture row")
        return
    end

    -- UListView creates the row widget on a later tick; poll briefly.
    local attempts = 0
    log("  polling for row widget...")
    LoopAsync(50, function()
        attempts = attempts + 1
        local r = find_opendojo_row()
        if r then
            log(string.format("  row materialized after %d attempt(s)", attempts))
            capture_and_hook(r)
            return true
        end
        if attempts >= 40 then   -- 2 seconds at 50ms
            log(string.format("  give up after %d attempts", attempts))
            return true
        end
        return false
    end)
end)

log(string.format(
    "loaded %s — open the practice pause menu, then press F4 to insert "
    .. "the OpenDojo row. Clicking the row opens the OpenDojo ImGui menu.",
    MOD))
