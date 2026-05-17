-- CharaIdFinder: locate P2 character ID in Tekken 8 practice mode.
--
-- Same methodology as PracticeRecHook — hook native + BP UE5 functions that
-- fire during character selection, log self addresses + int32 parameters.
-- From those call traces we get both the function name (for Ghidra static
-- analysis) and the live self pointer (to scan nearby memory for the ID).
--
-- Keybinds (hold Alt-key combos to avoid clashing with game hotkeys):
--   F1  snapshot current small-int fields of the PracticeMenu self object
--   F2  diff the last two F1 snapshots (change P2 between F1 presses)
--   F3  enumerate all chara-select widget classes currently in memory
--   F4  retry registering BP-dynamic hooks

local MOD = "CharaIdFinder"

local function log(line)
    print(string.format("[%s] %s\n", MOD, line))
end

local function fmt_obj(obj)
    if not obj then return "<nil>" end
    local ok_a, a = pcall(function() return obj:GetAddress() end)
    local ok_n, n = pcall(function() return obj:GetFullName() end)
    return string.format("0x%X %s", ok_a and a or 0, ok_n and n or "?")
end

-- ── Arg extractors ────────────────────────────────────────────────────────

-- Extract ALL arguments as ints (best-effort).  Each unknown arg that is NOT
-- an int is printed as its type so we don't miss struct/object params.
local function args_all(...)
    local parts = {}
    for i, w in ipairs({...}) do
        if w == nil then
            parts[#parts+1] = string.format("a%d=nil", i)
        else
            local ok, v = pcall(function() return w:get() end)
            if ok then
                if type(v) == "number" then
                    parts[#parts+1] = string.format("a%d=%d(0x%x)", i, v, v)
                elseif v == nil then
                    parts[#parts+1] = string.format("a%d=nil", i)
                elseif type(v) == "boolean" then
                    parts[#parts+1] = string.format("a%d=%s", i, tostring(v))
                elseif type(v) == "userdata" then
                    local ok2, s = pcall(function()
                        if v.GetAddress then return string.format("obj@0x%X", v:GetAddress()) end
                        if v.ToString   then return v:ToString() end
                        return "userdata"
                    end)
                    parts[#parts+1] = string.format("a%d=%s", i, ok2 and s or "udata?")
                else
                    parts[#parts+1] = string.format("a%d=%s", i, tostring(v))
                end
            else
                parts[#parts+1] = string.format("a%d=<err>", i)
            end
        end
    end
    return table.concat(parts, " ")
end

-- ── Hook infrastructure ───────────────────────────────────────────────────

local hooked = {}
local failed = {}

local function hook_fn(path, extract)
    local ok, err = pcall(function()
        RegisterHook(path,
            function(self_w, ...)
                local self_o = self_w and self_w:get() or nil
                local a = ""
                if extract then
                    local ok2, s = pcall(extract, ...)
                    a = ok2 and s or "<args_err>"
                end
                log(string.format("CALL %-80s self=%-30s %s",
                    path, fmt_obj(self_o), a))
            end,
            nil  -- no post-hook needed
        )
    end)
    if ok then
        hooked[#hooked+1] = path
    else
        local e = type(err) == "string" and err or tostring(err)
        failed[#failed+1] = path .. " :: " .. e
    end
end

-- ── Static native (always-loaded) hooks ──────────────────────────────────
--
-- PolarisUMGPracticeMenu is the top-level native menu class.  Its cursor and
-- decide callbacks fire for EVERY sub-menu action, including character change.
-- The `id` parameter encodes the selected item/character index.

for _, fn in ipairs({
    "InvokeDecideButton1Callback",
    "InvokeDecideButton2Callback",
    "InvokeDecideButton2LeftCallback",
    "InvokeDecideButton2RightCallback",
    "InvokeDecideButton3Callback",
    "InvokeSelectButton1Callback",
    "InvokeSelectButton2Callback",
    "InvokeSelectButton3Callback",
    "SetActiveLayer",
    "SetCursorButton1",
    "SetCursorButton2",
    "SetCursorButton3",
    "SetCursorButton4",
    "PlayAnimDecide",
}) do
    hook_fn("/Script/Polaris.PolarisUMGPracticeMenu:" .. fn, args_all)
end

-- Try character-select specific native classes.  Most won't exist but those
-- that do will fire exactly when characters are browsed / confirmed.
for _, path in ipairs({
    "/Script/Polaris.PolarisUMGCharaSelect:SetCharaId",
    "/Script/Polaris.PolarisUMGCharaSelect:SetCursor",
    "/Script/Polaris.PolarisUMGCharaSelect:OnDecide",
    "/Script/Polaris.PolarisUMGCharaSelect:OnSelect",
    "/Script/Polaris.PolarisUMGCharaSelect:UpdateCursor",
    "/Script/Polaris.PolarisUMGCharaSelect:SetCharaIndex",
    "/Script/Polaris.PolarisUMGCharaSelect:ChangeChara",
    "/Script/Polaris.PolarisTrainingMode:SetP2CharaId",
    "/Script/Polaris.PolarisTrainingMode:SetP2Character",
    "/Script/Polaris.PolarisTrainingMode:ChangeP2Chara",
    "/Script/Polaris.PolarisTrainingMode:SetCharaIndex",
    "/Script/Polaris.PolarisBattleManager:SetP2Character",
    "/Script/Polaris.PolarisBattleManager:ChangeP2Chara",
    "/Script/Polaris.PolarisGameMode:SetP2CharaId",
    "/Script/Polaris.PolarisPlayerController:SetCharaId",
    "/Script/Polaris.PolarisPlayerController:ChangeChara",
}) do
    hook_fn(path, args_all)
end

-- ── Snapshot / diff for memory scanning ──────────────────────────────────
--
-- Pressing F1 while in practice mode records all small-int fields on the live
-- PracticeMenu object.  Pressing F1 again after changing P2 records a second
-- snapshot.  F2 diffs them to show which field changed — that field at the
-- reported offset is the character ID.

local snapshots = {}

local function take_snapshot()
    local m = FindFirstOf("PolarisUMGPracticeMenu")
            or FindFirstOf("UPolarisUMGPracticeMenu")
    if not m or not IsValid(m) then
        log("F1: no PracticeMenu in memory (open practice mode first)")
        return
    end
    local addr_ok, base_addr = pcall(function() return m:GetAddress() end)
    if not addr_ok then log("F1: GetAddress failed") return end

    log(string.format("F1: snapshot of PracticeMenu @ 0x%X", base_addr))

    -- Read 0x800 bytes as dwords using UE4SS's memory accessor (if available)
    -- or fall back to struct-walking.  UE4SS 2.5+ exposes read_int32(addr).
    local fields = {}
    local has_mem = (type(read_int32) == "function")

    if has_mem then
        for off = 0, 0x7FC, 4 do
            local ok, v = pcall(read_int32, base_addr + off)
            if ok and type(v) == "number" and v >= 0 and v < 60 then
                fields[off] = v
            end
        end
        local field_count = 0
        for _ in pairs(fields) do field_count = field_count + 1 end
        log(string.format("F1: read %d small-int fields via read_int32", field_count))
    else
        log("F1: read_int32 not available — using property walk instead")
        -- UE4SS property enumeration via ForEachProperty (v2.5+)
        local class = m:GetClass()
        if class and class.ForEachProperty then
            class:ForEachProperty(function(prop)
                if not prop then return end
                local ok_n, name = pcall(function() return prop:GetName() end)
                if not ok_n then return end
                local ok_v, v = pcall(function() return m[name] end)
                if ok_v and type(v) == "number" and v >= 0 and v < 60 then
                    fields[name] = v
                    log(string.format("  .%s = %d", name, v))
                end
            end)
        else
            log("F1: ForEachProperty not available — install UE4SS 2.5+")
        end
    end

    snapshots[#snapshots+1] = { addr = base_addr, fields = fields }
    local snap_field_count = 0
    for _ in pairs(fields) do snap_field_count = snap_field_count + 1 end
    log(string.format("F1: snapshot #%d stored (%d fields)", #snapshots, snap_field_count))
end

RegisterKeyBind(Key.F1, take_snapshot)

RegisterKeyBind(Key.F2, function()
    if #snapshots < 2 then
        log("F2: need at least 2 snapshots (press F1 before AND after changing P2)")
        return
    end
    local a = snapshots[#snapshots - 1]
    local b = snapshots[#snapshots]
    log(string.format("F2: diff snapshot %d (0x%X) vs %d (0x%X)",
        #snapshots-1, a.addr, #snapshots, b.addr))
    local diffs = {}
    -- Compare common keys
    for k, va in pairs(a.fields) do
        local vb = b.fields[k]
        if vb ~= nil and vb ~= va then
            diffs[#diffs+1] = {key=k, before=va, after=vb}
        end
    end
    -- Keys only in b
    for k, vb in pairs(b.fields) do
        if a.fields[k] == nil and vb >= 0 and vb < 60 then
            diffs[#diffs+1] = {key=k, before="(new)", after=vb}
        end
    end
    if #diffs == 0 then
        log("F2: no small-int differences — char ID may be outside 0..59 range or in a different object")
    else
        log(string.format("F2: %d changed fields:", #diffs))
        for _, d in ipairs(diffs) do
            log(string.format("  key=%s  %s -> %s", tostring(d.key), tostring(d.before), tostring(d.after)))
        end
    end
end)

-- ── BP-dynamic character select widget hooks ─────────────────────────────

local chara_bp_hooked = false

local chara_widget_candidates = {
    -- Practice chara change variants
    "WBP_UI_Practice_CharaChange_C",
    "WBP_UI_CharaChange_C",
    "WBP_UI_PracticeMode_CharaChange_C",
    "WBP_UI_Practice_QuickCharaSelect_C",
    "WBP_UI_QuickCharaSelect_C",
    -- Generic chara select variants
    "WBP_UI_CharaSelect_C",
    "WBP_UI_CharaSelectBase_C",
    "WBP_UI_Practice_CharaSelect_C",
    "WBP_UI_PracticeCharaSelect_C",
    "WBP_UI_VSCharaSelect_C",
    "WBP_UI_VS_CharaSelect_C",
    "WBP_UI_VS_CharaSelectBase_C",
    -- BP actor variants
    "BP_CharaSelect_C",
    "BP_PracticeCharaSelect_C",
    "BP_VS_CharaSelect_C",
    -- UMG native wrappers
    "WBP_UI_CharaSelectParts_C",
    "WBP_UI_CharaSelectIcon_C",
    "WBP_UI_CharaIcon_C",
    "WBP_UI_Practice_Button_Chara_C",
    -- Sub-menu layer that might host chara select
    "WBP_UI_PracticeMenu_CharaSelect_C",
    "WBP_UI_PracticeMenu_Sub_CharaSelect_C",
}

-- Functions worth hooking on whatever the chara-select widget class turns out to be.
local chara_widget_fns = {
    "OnDecide", "OnSelect", "OnConfirm", "Confirm", "Decide",
    "SetCursor", "UpdateCursor", "OnCursorMove",
    "SetCharaId", "SetCharaIndex", "UpdateChara", "ChangeChara",
    "OnListItemObjectSet", "UpdateData",
}

local function try_hook_chara_widgets()
    if chara_bp_hooked then return true end
    for _, cls in ipairs(chara_widget_candidates) do
        local ok, objs = pcall(FindAllOf, cls)
        if ok and type(objs) == "table" and #objs > 0 then
            local ok_c, cls_full = pcall(function()
                return objs[1]:GetClass():GetFullName()
            end)
            if ok_c then
                local path = tostring(cls_full):match("(/Game/[%w_%./]+)")
                if path then
                    log(string.format("F3/auto: found %s → hooking BP functions on %s",
                        cls, path))
                    for _, fn in ipairs(chara_widget_fns) do
                        hook_fn(path .. ":" .. fn, args_all)
                    end
                    chara_bp_hooked = true
                    return true
                end
            end
        end
    end
    return false
end

-- F3: broad scan for ANY widget whose class name contains "Chara", "VS", or "Select".
-- Press this WHILE the character select overlay is visibly open on screen.
RegisterKeyBind(Key.F3, function()
    log("=== F3: live scan for any Chara/Select/VS widget ===")

    -- 1. Scan candidate list
    local found_any = false
    for _, cls in ipairs(chara_widget_candidates) do
        local ok, objs = pcall(FindAllOf, cls)
        if ok and type(objs) == "table" and #objs > 0 then
            log(string.format("  FOUND  %-55s  %d instances", cls, #objs))
            for i, obj in ipairs(objs) do
                log(string.format("    [%d] %s", i, fmt_obj(obj)))
            end
            found_any = true
        end
    end

    -- 2. Also scan well-known practice+VS classes as broad net
    local broad = {
        "WBP_UI_PracticeMenu_C",
        "WBP_UI_VSMode_C",
        "WBP_UI_VS_C",
        "WBP_UI_VSSetting_C",
        "WBP_UI_CharacterSetting_C",
        "WBP_UI_Battle_C",
        "BP_PolarisGameInstance_C",
        "BP_PolarisBattleGameMode_C",
        "BP_PolarisPlayerController_C",
        "WBP_UI_Practice_S2_C",
        "WBP_UI_Practice_Simple_C",
    }
    for _, cls in ipairs(broad) do
        local ok, objs = pcall(FindAllOf, cls)
        if ok and type(objs) == "table" and #objs > 0 then
            log(string.format("  broad %-55s  %d instances", cls, #objs))
            for i, obj in ipairs(objs) do
                -- Log the full path to spot any Chara-related widgets in its tree
                log(string.format("    [%d] %s", i, fmt_obj(obj)))
            end
        end
    end

    if not found_any then
        log("  No candidates matched. See broad results above.")
        log("  >>> Press F3 BEFORE confirming, while chara grid is visible <<<")
    end

    -- 3. Try to register BP hooks now (in case the widget just appeared)
    if try_hook_chara_widgets() then
        log("  BP hooks registered this press")
    end
end)

-- F4: force-retry all dynamic hooks
RegisterKeyBind(Key.F4, function()
    log("=== F4: retry BP hooks ===")
    if chara_bp_hooked then
        log("  Chara widget already hooked")
    elseif try_hook_chara_widgets() then
        log("  Chara widget hooked now")
    else
        log("  Chara widget not in memory yet — open character select then press F4")
    end
end)

-- Poll every 1.5s in case the widget appears after mod load
if LoopAsync then
    LoopAsync(1500, function()
        if try_hook_chara_widgets() then return true end
        return false
    end)
end

-- ── Practice HUD hooks: WBP_UI_Practice_Simple_C (_L=P1, _R=P2) updates when ──
-- character changes.  Hook it dynamically the same way PracticeRecHook hooks
-- the slot widgets: wait for the class to appear then register.
local practice_simple_hooked = false

local practice_simple_fns = {
    "SetCharaId",    "SetChara",     "ChangeChara",   "UpdateChara",
    "SetP1Chara",    "SetP2Chara",   "SetCharaIndex", "SetCharaData",
    "SetCharaName",  "SetCharaIcon", "SetPlayer",     "SetPlayerData",
    "Setup",         "UpdateData",   "Init",          "OnInit",
    "SetInfo",       "SetCharaInfo", "UpdateCharaInfo",
}

local function try_hook_practice_simple()
    if practice_simple_hooked then return true end
    local ok, objs = pcall(FindAllOf, "WBP_UI_Practice_Simple_C")
    if not ok or type(objs) ~= "table" or #objs == 0 then return false end

    local ok_c, cls_full = pcall(function() return objs[1]:GetClass():GetFullName() end)
    if not ok_c then return false end
    local path = tostring(cls_full):match("(/Game/[%w_%./]+)")
    if not path then return false end

    log(string.format("Practice_Simple: hooking %s (%d instances)", path, #objs))
    for _, fn in ipairs(practice_simple_fns) do
        hook_fn(path .. ":" .. fn, args_all)
    end
    -- Also hook the S2 container
    local ok2, s2 = pcall(FindFirstOf, "WBP_UI_Practice_S2_C")
    if ok2 and s2 and IsValid(s2) then
        local ok3, cf = pcall(function() return s2:GetClass():GetFullName() end)
        if ok3 then
            local p2 = tostring(cf):match("(/Game/[%w_%./]+)")
            if p2 then
                log(string.format("Practice_S2: hooking %s", p2))
                hook_fn(p2 .. ":SetP2Chara",       args_all)
                hook_fn(p2 .. ":SetP1Chara",       args_all)
                hook_fn(p2 .. ":SetChara",         args_all)
                hook_fn(p2 .. ":UpdateChara",      args_all)
                hook_fn(p2 .. ":ChangeCharaEvent", args_all)
                hook_fn(p2 .. ":Setup",            args_all)
                hook_fn(p2 .. ":Init",             args_all)
            end
        end
    end
    practice_simple_hooked = true
    return true
end

if LoopAsync then
    LoopAsync(1500, function()
        if try_hook_practice_simple() then return true end
        return false
    end)
end

-- F6: enumerate EVERY function and property defined in WBP_UI_Practice_Simple_C.
-- Run once while in practice mode — this tells us the exact names to hook.
RegisterKeyBind(Key.F6, function()
    log("=== F6: WBP_UI_Practice_Simple_C function+property enumeration ===")
    local ok, objs = pcall(FindAllOf, "WBP_UI_Practice_Simple_C")
    if not ok or type(objs) ~= "table" or #objs == 0 then
        log("  not found — enter practice mode first")
        return
    end
    local cls = objs[1]:GetClass()
    if not cls then log("  GetClass() failed") return end

    -- List all functions
    local fn_count = 0
    if cls.ForEachFunction then
        cls:ForEachFunction(function(fn)
            if not fn then return end
            local ok_n, name = pcall(function() return fn:GetName() end)
            if ok_n and name then
                log(string.format("  FN  %s", name))
                fn_count = fn_count + 1
            end
        end)
        log(string.format("  total functions: %d", fn_count))
    else
        log("  ForEachFunction not available — using parent class chain walk")
        -- Walk superclass chain and log class names
        local cur = cls
        for depth = 0, 8 do
            local ok_n, n = pcall(function() return cur:GetFullName() end)
            log(string.format("  class[%d] = %s", depth, ok_n and n or "?"))
            local ok_s, super = pcall(function() return cur:GetSuperClass() end)
            if not ok_s or not super or super == cur then break end
            cur = super
        end
    end

    -- List all properties
    local prop_count = 0
    if cls.ForEachProperty then
        cls:ForEachProperty(function(prop)
            if not prop then return end
            local ok_n, name = pcall(function() return prop:GetName() end)
            if ok_n and name then
                -- Read value from first instance
                local ok_v, v = pcall(function() return objs[1][name] end)
                local vstr = ok_v and tostring(v) or "<err>"
                log(string.format("  PROP %s = %s", name, vstr))
                prop_count = prop_count + 1
            end
        end)
        log(string.format("  total properties: %d", prop_count))
    else
        log("  ForEachProperty not available")
    end

    -- Also log the _R (P2 side) address for CE/memory work
    for i, obj in ipairs(objs) do
        local ok_a, addr = pcall(function() return obj:GetAddress() end)
        local ok_n2, fname = pcall(function() return obj:GetFullName() end)
        log(string.format("  instance[%d] @ 0x%X  %s",
            i,
            ok_a and addr or 0,
            ok_n2 and fname:match("[%w_]+$") or "?"))
    end
end)

-- F5: dump ALL small-int properties of WBP_UI_Practice_Simple_C instances
-- Press while in practice mode — the _R (P2) instance should have a char ID field.
RegisterKeyBind(Key.F5, function()
    log("=== F5: Practice_Simple property dump ===")
    try_hook_practice_simple()  -- register hooks if not yet done
    local ok, objs = pcall(FindAllOf, "WBP_UI_Practice_Simple_C")
    if not ok or type(objs) ~= "table" or #objs == 0 then
        log("  WBP_UI_Practice_Simple_C not in memory")
        return
    end
    for i, obj in ipairs(objs) do
        local name = "?"
        local ok_n, n = pcall(function() return obj:GetFullName() end)
        if ok_n then name = n end
        log(string.format("  [%d] %s", i, name))

        -- Dump via ForEachProperty if available
        local cls = obj:GetClass()
        if cls and cls.ForEachProperty then
            cls:ForEachProperty(function(prop)
                if not prop then return end
                local ok_n2, pname = pcall(function() return prop:GetName() end)
                if not ok_n2 then return end
                local ok_v, v = pcall(function() return obj[pname] end)
                if ok_v and type(v) == "number" and v >= 0 and v < 100 then
                    log(string.format("    .%s = %d", pname, v))
                end
            end)
        else
            -- Fall back: try known property names
            local prop_guesses = {
                "CharaId","CharaID","CharacterId","CharacterID",
                "charaId","charaID","Chara","chara",
                "PlayerIndex","TeamIndex","SideIndex",
                "CharaIndex","CharaNum","CharaNo",
                "SideId","PlayerId","PlayerNo",
                "CharaDataId","CharaModelId",
            }
            for _, pn in ipairs(prop_guesses) do
                local ok_p, v = pcall(function() return obj[pn] end)
                if ok_p and type(v) == "number" then
                    log(string.format("    .%s = %d", pn, v))
                end
            end
        end
    end
    -- Also dump S2 container
    local ok2, s2 = pcall(FindFirstOf, "WBP_UI_Practice_S2_C")
    if ok2 and s2 and IsValid(s2) then
        log(string.format("  WBP_UI_Practice_S2_C @ 0x%X", s2:GetAddress()))
        local prop_guesses = {
            "P1CharaId","P2CharaId","P1Chara","P2Chara",
            "CharaId","CharaID","Player1CharaId","Player2CharaId",
        }
        for _, pn in ipairs(prop_guesses) do
            local ok_p, v = pcall(function() return s2[pn] end)
            if ok_p and type(v) == "number" then
                log(string.format("    S2.%s = %d", pn, v))
            end
        end
    end
end)

-- ── Broad NotifyOnNewObject: catch the chara-select overlay the instant it spawns ──
-- We don't know its exact class name, so register for every plausible variant.
-- When one fires, it logs the exact class path — use that path in Ghidra.
local notify_candidates = {
    "/Game/UI/Widget/Practice/WBP_UI_Practice_CharaChange.WBP_UI_Practice_CharaChange_C",
    "/Game/UI/Widget/CharaSelect/WBP_UI_CharaChange.WBP_UI_CharaChange_C",
    "/Game/UI/Widget/CharaSelect/WBP_UI_CharaSelect.WBP_UI_CharaSelect_C",
    "/Game/UI/Widget/VS/WBP_UI_VS_CharaSelect.WBP_UI_VS_CharaSelect_C",
    "/Game/UI/Widget/VS/WBP_UI_VSCharaSelect.WBP_UI_VSCharaSelect_C",
    "/Game/UI/Widget/Practice/WBP_UI_Practice_QuickCharaSelect.WBP_UI_Practice_QuickCharaSelect_C",
    "/Game/UI/WBP_UI_CharaChange.WBP_UI_CharaChange_C",
    "/Game/UI/WBP_UI_CharaSelect.WBP_UI_CharaSelect_C",
    "/Game/UI/Widget/WBP_UI_CharaChange.WBP_UI_CharaChange_C",
    "/Game/UI/Widget/WBP_UI_CharaSelect.WBP_UI_CharaSelect_C",
}

for _, path in ipairs(notify_candidates) do
    local short = path:match("%.([%w_]+)$") or path
    pcall(NotifyOnNewObject, path, function(obj)
        log(string.format("NEW OBJECT: %s", fmt_obj(obj)))
        -- Hook all possible functions on this newly-found class
        if chara_bp_hooked then return end
        local ok_c, cls_full = pcall(function() return obj:GetClass():GetFullName() end)
        if ok_c then
            local bp_path = tostring(cls_full):match("(/Game/[%w_%./]+)")
            if bp_path then
                log(string.format("  >>> hooking %s", bp_path))
                for _, fn in ipairs(chara_widget_fns) do
                    hook_fn(bp_path .. ":" .. fn, args_all)
                end
                chara_bp_hooked = true
            end
        end
    end)
end

-- ── Startup report ────────────────────────────────────────────────────────

log("loaded")
log(string.format("  %d native hooks registered, %d failed", #hooked, #failed))
for _, f in ipairs(failed) do log("  FAIL: " .. f) end
log("Keybinds:")
log("  F1  snapshot PracticeMenu small-int fields")
log("  F2  diff last two F1 snapshots")
log("  F3  scan for chara-select widgets (press while overlay is open)")
log("  F4  retry BP hook registration")
log("  F5  dump Practice_Simple properties")
log("  F6  *** enumerate ALL functions+properties of WBP_UI_Practice_Simple_C ***")
log("When on character select: scroll cursor → watch InvokeDecide/SetCursor log lines for the char index")
