-- OpenDojoMenu — pause-menu mod for Tekken 8 practice mode.
--
-- Pivot: instead of modifying / inserting into the existing practice
-- menu (which the BP's per-frame UpdateData reverts), spawn a wholly
-- new standalone popup overlay we fully own.
--
-- First milestone: get the existing WBP_UI_Dialog_C (the confirm-
-- popup the game already uses for "Save? Yes/No" prompts) to surface
-- with our text. If it works, we have a native-looking popup with no
-- asset surgery and no BP fights. Once visible we layer custom content
-- and intercept the confirm/cancel callbacks.
--
-- Hotkeys (all F-keys to fit the user's keyboard):
--   F1  — reflect WBP_UI_PracticeMenu_Button_1_C  (diagnostic)
--   F2  — reflect WBP_UI_Dialog_C  (diagnostic, NEW)
--   F3  — list live row widgets + their list_item bindings
--   F4  — POPUP TEST: find existing dialog, AddToViewport +
--         SetDescription + PlayAnimIn  (the new direction)
--   F5  — reflect WBP_UI_PracticeMenu_C  (diagnostic)
--   F6  — write-test on item[1].Text  (diagnostic; left in place)
--   F7  — read-only ListView_1.ListItems inspection
--   F8  — AddItem smoke test on ListView_1

local MOD = "OpenDojoMenu"

local function log(msg) print(string.format("[%s] %s\n", MOD, msg)) end

local function fmt_obj(o)
    if not o then return "<nil>" end
    local ok_a, a = pcall(function() return o:GetAddress() end)
    local ok_n, n = pcall(function() return o:GetFullName() end)
    return string.format("0x%X %s",
        ok_a and a or 0, ok_n and tostring(n) or "<no name>")
end

local function read_text(prop)
    if prop == nil then return nil end
    local ok, v = pcall(function()
        if type(prop) == "userdata" and prop.ToString then
            return prop:ToString()
        end
        return tostring(prop)
    end)
    return ok and v or nil
end

-- UE4SS sometimes wraps errors in nested function thunks. Recurse until
-- we land on something useful, or bail at depth 6.
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

-- ============================================================
-- F1 — reflect WBP_UI_PracticeMenu_Button_1_C
-- ============================================================
RegisterKeyBind(Key.F1, function()
    log("=== F1: WBP_UI_PracticeMenu_Button_1_C reflection ===")
    local ok, rows = pcall(FindAllOf, "WBP_UI_PracticeMenu_Button_1_C")
    if not ok or #rows == 0 then log("  no row widgets"); return end
    local cls = rows[1]:GetClass()
    local pcount, fcount = 0, 0
    pcall(function()
        cls:ForEachProperty(function(prop)
            pcount = pcount + 1
            local n, t = "?", "?"
            pcall(function() n = prop:GetFName():ToString() end)
            pcall(function() t = prop:GetClass():GetFName():ToString() end)
            log(string.format("    [%03d] %-40s : %s", pcount, n, t))
        end)
    end)
    pcall(function()
        cls:ForEachFunction(function(fn)
            fcount = fcount + 1
            local n = "?"; pcall(function() n = fn:GetFName():ToString() end)
            log(string.format("    fn[%03d] %s", fcount, n))
        end)
    end)
    log(string.format("  properties=%d functions=%d", pcount, fcount))
end)

-- ============================================================
-- F2 — reflect WBP_UI_Dialog_C  (the popup we want to use)
-- ============================================================
RegisterKeyBind(Key.F2, function()
    log("=== F2: WBP_UI_Dialog_C reflection ===")
    local ok, dialogs = pcall(FindAllOf, "WBP_UI_Dialog_C")
    if not ok or #dialogs == 0 then
        log("  no live WBP_UI_Dialog_C — none cached. Try opening a confirm "
            .. "dialog in-game first, then F2 again.")
        return
    end
    log(string.format("  found %d dialog instances", #dialogs))
    for i, d in ipairs(dialogs) do log(string.format("    [%d] %s", i, fmt_obj(d))) end
    local cls = dialogs[1]:GetClass()
    log("  --- properties ---")
    local pcount = 0
    pcall(function()
        cls:ForEachProperty(function(prop)
            pcount = pcount + 1
            local n, t = "?", "?"
            pcall(function() n = prop:GetFName():ToString() end)
            pcall(function() t = prop:GetClass():GetFName():ToString() end)
            log(string.format("    [%03d] %-40s : %s", pcount, n, t))
        end)
    end)
    log("  --- functions ---")
    local fcount = 0
    pcall(function()
        cls:ForEachFunction(function(fn)
            fcount = fcount + 1
            local n = "?"; pcall(function() n = fn:GetFName():ToString() end)
            log(string.format("    fn[%03d] %s", fcount, n))
        end)
    end)
    log(string.format("  properties=%d functions=%d", pcount, fcount))
end)

-- ============================================================
-- F3 — live row widgets + their list_item bindings
-- ============================================================
RegisterKeyBind(Key.F3, function()
    log("=== F3: live rows ===")
    local ok, rows = pcall(FindAllOf, "WBP_UI_PracticeMenu_Button_1_C")
    if not ok or #rows == 0 then log("  no rows"); return end
    for i, w in ipairs(rows) do
        local info = "<no list_item>"
        local ok_p, p = pcall(function() return w.list_item end)
        if ok_p and p then
            local t = read_text(p.Text) or "<?>"
            info = string.format("list_item=%s Text=%q", fmt_obj(p), t)
        end
        log(string.format("  [%02d] %s  %s", i, fmt_obj(w), info))
    end
end)

-- ============================================================
-- F4 — Add a row via the menu's own BP API.
--
-- F6 reflection showed AddButton1Data takes NO args, so the menu
-- reads from internal state. Try calling the canonical flow
-- (Add → UpdateListView) with zero args first; if that adds a row,
-- we're in. Also try the UListView path AddItem + RegenerateAllEntries
-- as a parallel attempt.
-- ============================================================
-- ============================================================
-- F4 — insert OpenDojo row AND persist clean text.
--
-- Single combined operation:
--   1. (Idempotent) AddButton1Data("OpenDojo", true) + UpdateListView1
--   2. Locate our row widget (the one whose list_item.Text == "OpenDojo"
--      — property access on FString yields Lua string, unlike arg-wrapper
--      :get() which returns opaque userdata).
--   3. Capture TB_Menu_OFF/ON addresses of our row.
--   4. Call SetRawText immediately so the err prefix is replaced now.
--   5. Install a one-time SetTextID post-hook that, when self matches a
--      captured TB address, re-applies SetRawText. Persists across hover
--      / focus / re-bind.
-- ============================================================
local g_our_tb_addrs   = {}
local g_setrawtext_hooked = false

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

-- Capture our row's TB addresses, force-set the text once, and install
-- the global SetTextID hook (if it isn't already). Called immediately
-- once an "OpenDojo" row is visible — either because it already existed
-- or because the retry loop just picked it up.
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

    if off then pcall(function() off:SetRawText("OpenDojo", false) end) end
    if on  then pcall(function() on :SetRawText("OpenDojo", false) end) end
    log("  SetRawText applied")

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

RegisterKeyBind(Key.F4, function()
    log("=== F4: insert OpenDojo row + install persistence hook ===")
    local m = find_menu()
    if not m then log("  no menu"); return end

    -- Idempotent: if the row is already present, just capture and hook.
    local row = find_opendojo_row()
    if row then
        log("  OpenDojo row already exists @ " .. fmt_obj(row))
        capture_and_hook(row)
        return
    end

    -- Insert. UListView creates the row widget on a later tick, so we
    -- can't capture immediately. Poll briefly until the row materializes.
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

    local attempts = 0
    log("  polling for row widget...")
    LoopAsync(50, function()
        attempts = attempts + 1
        local r = find_opendojo_row()
        if r then
            log(string.format("  row materialized after %d attempt(s)",
                attempts))
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

-- ============================================================
-- F5 — install Gryphon localization hooks so "OpenDojo" resolves
-- cleanly. Tekken's BP graph calls UGryphonFunctionLibrary:GetText
-- (and possibly HasText/GetString) with the FString stored on
-- BP_PracticeMenu_Button_1_Item_C.Text. For unknown keys, the lookup
-- returns an "err(<key>)@@@@" FText, which is what we see.
--
-- Pre-hook with return-value override: UE4SS exposes the function's
-- return value as the LAST argument wrapper to the pre-hook callback.
-- Calling :set(...) on it replaces what the caller observes.
--
-- One-shot; idempotent.
-- ============================================================
local g_gryphon_hooked = false

local function install_gryphon_hook(fn_path, label, override_fn)
    local ok, err = pcall(function()
        RegisterHook(fn_path, function(self_w, arg_w, ret_w)
            local arg
            pcall(function() arg = tostring(arg_w:get()) end)
            if arg == "OpenDojo" then
                log(string.format("    %s hook fired textId='OpenDojo'", label))
                pcall(function() override_fn(ret_w) end)
            end
        end)
    end)
    log(string.format("  install %s -> %s  err=%s",
        label, ok and "ok" or "FAIL",
        ok and "-" or err_to_str(err)))
end

RegisterKeyBind(Key.F5, function()
    if g_gryphon_hooked then
        log("=== F5: hooks already installed ===")
        return
    end
    log("=== F5: install row-widget hooks for 'OpenDojo' ===")

    local rows = FindAllOf("WBP_UI_PracticeMenu_Button_1_C") or {}
    if #rows == 0 then
        log("  ERROR: no row widgets live — open the practice menu first")
        return
    end
    local cls_full
    pcall(function() cls_full = rows[1]:GetClass():GetFullName() end)
    local class_path = tostring(cls_full):match("(/Game/[^%s]+)")
    if not class_path then
        log("  ERROR: couldn't parse /Game path from " .. tostring(cls_full))
        return
    end
    log("  row BP class_path = " .. class_path)

    -- UE4SS hands FString back as a userdata wrapper. tostring() on it
    -- yields `FString: 0x...` (the wrapper's address), not the actual
    -- characters. ToString() unwraps to a real Lua string.
    local function read_fstring(prop)
        if prop == nil then return "<nil>" end
        local ok_t, v = pcall(function()
            if type(prop) == "userdata" and prop.ToString then
                return prop:ToString()
            end
            if type(prop) == "string" then return prop end
            return tostring(prop)
        end)
        return ok_t and v or "<err>"
    end

    -- Per-hook fire counters so we can see which paths the engine
    -- actually walks for our new row.
    local counts = {}
    local function maybe_log(label, msg)
        counts[label] = (counts[label] or 0) + 1
        local c = counts[label]
        if c <= 8 or c % 100 == 0 then
            log(string.format("    [%s #%d] %s", label, c, msg))
        end
    end

    local function override(self, label, item_text)
        local ok_off, err_off = pcall(function()
            self.TB_Menu_OFF:SetRawText("OpenDojo", false)
        end)
        local ok_on, err_on = pcall(function()
            self.TB_Menu_ON:SetRawText("OpenDojo", false)
        end)
        log(string.format("    [%s] MATCH text=%q  OFF=%s ON=%s  err_off=%s err_on=%s",
            label, item_text,
            ok_off and "ok" or "FAIL",
            ok_on  and "ok" or "FAIL",
            ok_off and "-" or err_to_str(err_off),
            ok_on  and "-" or err_to_str(err_on)))
    end

    -- 1. Row-widget BP hooks (per-instance, hookable on 3.0.1).
    local function row_hook(fn_name, with_item)
        local hp = class_path .. ":" .. fn_name
        local ok, err = pcall(function()
            RegisterHook(hp,
                function(self_w, arg_w) end,
                function(self_w, arg_w)
                    local self; pcall(function() self = self_w:get() end)
                    if not self then maybe_log(fn_name, "self nil"); return end
                    local txt = "<no item>"
                    if with_item then
                        local item; pcall(function() item = arg_w:get() end)
                        if item then txt = read_fstring(item.Text) end
                    else
                        pcall(function()
                            if self.list_item then
                                txt = read_fstring(self.list_item.Text)
                            end
                        end)
                    end
                    maybe_log(fn_name, "Text=" .. txt)
                    if txt == "OpenDojo" then override(self, fn_name, txt) end
                end)
        end)
        log(string.format("  hook %s -> %s  err=%s",
            fn_name, ok and "ok" or "FAIL", ok and "-" or err_to_str(err)))
    end
    row_hook("UpdateData", true)
    row_hook("OnListItemObjectSet", true)
    row_hook("BP_OnItemSelectionChanged", false)

    -- 2. Native UPolarisTextBlock:SetTextID — fires whenever Gryphon
    -- text-id is applied to ANY text block. If the textId is the one
    -- our item carries, override via SetRawText post-call.
    local poly_path = "/Script/Polaris.PolarisTextBlock:SetTextID"
    log("  hooking " .. poly_path)
    -- Diagnostic probe: dump everything we can about the FString param
    -- for the first 5 SetTextID calls so we discover the right extraction.
    local probe_n = 0
    local ok_p, err_p = pcall(function()
        RegisterHook(poly_path,
            function(self_w, id_w) end,
            function(self_w, id_w)
                local self; pcall(function() self = self_w:get() end)
                if not self then return end
                local raw; pcall(function() raw = id_w:get() end)

                -- One-time deep probe for first 5 calls.
                if probe_n < 5 then
                    probe_n = probe_n + 1
                    local rep = string.format("[probe #%d] raw type=%s",
                        probe_n, type(raw))
                    if type(raw) == "userdata" then
                        local mems = {}
                        for k, v in pairs(getmetatable(raw) or {}) do
                            mems[#mems + 1] = tostring(k)
                        end
                        rep = rep .. " meta_keys=" .. table.concat(mems, ",")
                        if raw.ToString then
                            local ok1, v1 = pcall(function() return raw:ToString() end)
                            rep = rep .. string.format("  ToString()=%q",
                                ok1 and tostring(v1) or "<err>")
                        end
                        for _, m in ipairs({ "Get", "GetString", "Data", "ArrayNum" }) do
                            local ok_m, v_m = pcall(function()
                                if type(raw[m]) == "function" then
                                    return raw[m](raw)
                                else
                                    return raw[m]
                                end
                            end)
                            if ok_m and v_m ~= nil then
                                rep = rep .. string.format("  %s=%s",
                                    m, tostring(v_m))
                            end
                        end
                    elseif type(raw) == "string" then
                        rep = rep .. string.format("  val=%q", raw)
                    else
                        rep = rep .. string.format("  val=%s", tostring(raw))
                    end
                    log("    " .. rep)
                end

                local txt = read_fstring(raw)
                maybe_log("SetTextID", "id=" .. txt)
                if txt ~= "OpenDojo" then return end
                local ok_w, err_w = pcall(function()
                    self:SetRawText("OpenDojo", false)
                end)
                log(string.format("    [SetTextID] MATCH -> SetRawText %s  err=%s",
                    ok_w and "ok" or "FAIL",
                    ok_w and "-" or err_to_str(err_w)))
            end)
    end)
    log(string.format("  hook SetTextID -> %s  err=%s",
        ok_p and "ok" or "FAIL", ok_p and "-" or err_to_str(err_p)))

    g_gryphon_hooked = true
    log("  all hooks installed. Press F4 to insert the row.")
end)

-- ============================================================
-- F12 — wide scan: look for dialog-opener functions across many
-- candidate classes (game instance, player controller, etc.).
-- The function name pattern is the same as F5's heuristic.
-- ============================================================
RegisterKeyBind(Key.F12, function()
    log("=== F12: scan game classes for dialog-opener functions ===")
    local targets = {
        "WBP_UI_PracticeMenu_C",
        "PolarisUMGPracticeMenu",
        "BP_PolarisGameInstance_C",
        "PolarisGameInstance",
        "PolarisPlayerController",
        "BP_PolarisBattleGameMode_C",
        "PolarisBattleGameMode",
        "BP_PolarisHUD_C",
        "PolarisHUD",
    }
    local function looks_like_dialog_opener(name)
        local low = name:lower()
        return low:find("dialog") or low:find("confirm")
            or low:find("open") or low:find("show")
            or low:find("modal") or low:find("popup")
            or low:find("alert") or low:find("prompt")
    end

    for _, target in ipairs(targets) do
        local inst = FindFirstOf(target)
        -- FindFirstOf can return a wrapped null when the class isn't
        -- live; calling :GetClass() on that freezes the game. Probe
        -- via :GetAddress() first.
        local addr
        if inst then pcall(function() addr = inst:GetAddress() end) end
        if not addr or addr == 0 then
            log(string.format("  -- %s -- <not live>", target))
        else
            log(string.format("  -- %s -- %s", target, fmt_obj(inst)))
            local cls
            pcall(function() cls = inst:GetClass() end)
            if cls then
                local hits = 0
                pcall(function()
                    cls:ForEachFunction(function(fn)
                        local fname = "?"
                        pcall(function() fname = fn:GetFName():ToString() end)
                        if looks_like_dialog_opener(fname) then
                            log("      HIT: " .. fname)
                            hits = hits + 1
                        end
                    end)
                end)
                if hits == 0 then log("      (no matches)") end
            end
        end
    end
end)

-- ============================================================
-- F6 — reflect the parameter list of every "AddButton*Data" /
-- "ClearButton*Data" / "UpdateListView*" function so we know the
-- argument signatures before calling them. UE4SS exposes UFunction
-- params via the same ForEachProperty iterator used on UClass.
-- ============================================================
RegisterKeyBind(Key.F6, function()
    log("=== F6: reflect AddButton*/Clear*/UpdateListView* params ===")
    local m = find_menu()
    if not m then log("  no menu"); return end
    local cls = m:GetClass()
    if not cls then log("  no class"); return end

    local function looks_relevant(name)
        local low = name:lower()
        return low:find("addbutton") or low:find("clearbutton")
            or low:find("updatelistview") or low:find("addbutton1data")
    end

    pcall(function()
        cls:ForEachFunction(function(fn)
            local fname = "?"
            pcall(function() fname = fn:GetFName():ToString() end)
            if not looks_relevant(fname) then return end
            log("  fn: " .. fname)
            local pidx = 0
            pcall(function()
                fn:ForEachProperty(function(prop)
                    pidx = pidx + 1
                    local pname, ptype = "?", "?"
                    local flags
                    pcall(function() pname = prop:GetFName():ToString() end)
                    pcall(function() ptype = prop:GetClass():GetFName():ToString() end)
                    pcall(function() flags = prop:GetPropertyFlags() end)
                    log(string.format("    param[%d] %-30s : %s  flags=%s",
                        pidx, pname, ptype, tostring(flags)))
                end)
            end)
            if pidx == 0 then log("    (no params)") end
        end)
    end)
end)

-- ============================================================
-- F7 — ListView_1.ListItems inspection
-- ============================================================
RegisterKeyBind(Key.F7, function()
    log("=== F7: ListItems inspection ===")
    local m = find_menu()
    if not m then log("  no menu"); return end
    local lv = m.ListView_1
    if not lv then log("  no ListView_1"); return end
    local li = lv.ListItems
    if not li then log("  no ListItems"); return end
    local n; pcall(function() n = li:GetArrayNum() end)
    log(string.format("  count = %s", tostring(n)))
end)

-- ============================================================
-- F8 — AddItem smoke test
-- ============================================================
RegisterKeyBind(Key.F8, function()
    log("=== F8: AddItem smoke test ===")
    local m = find_menu()
    local lv = m and m.ListView_1
    local ok, items = pcall(FindAllOf, "BP_PracticeMenu_Button_1_Item_C")
    if not lv or not items or #items == 0 then return end
    pcall(function() lv:AddItem(items[1]) end)
    log("  AddItem fired")
end)

log(string.format(
    "loaded %s — F4 AddButton1Data insert, F5 install Gryphon hooks. "
    .. "Sequence: F5 first (one-shot), then F4. "
    .. "(F1/F2/F3/F6/F7/F8/F12 are diagnostics if needed.)",
    MOD))
