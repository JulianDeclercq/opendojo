-- PracticeRecHook: discover the call surface for Tekken 8 practice-mode recording.
-- Goal: identify the UFunction call sequence emitted when the user records,
--   saves to a slot, plays a slot, and erases a slot. Capture self pointers so
--   they can be pivoted to in Cheat Engine to find the recording buffer.

local MOD = "PracticeRecHook"

local function fmt_obj(obj)
    if not obj then return "<nil>" end
    local ok_addr, addr = pcall(function() return obj:GetAddress() end)
    local ok_name, name = pcall(function() return obj:GetFullName() end)
    addr = ok_addr and addr or 0
    name = ok_name and name or "<no name>"
    return string.format("0x%X %s", addr, name)
end

local function log(line)
    print(string.format("[%s] %s\n", MOD, line))
end

-- ============================================================
-- Hook helpers
-- ============================================================

-- Track which functions we successfully hooked so we can report at startup.
local hooked = {}
local hook_failures = {}

local function err_to_str(err)
    if type(err) == "string" then return err end
    if type(err) == "function" then
        local ok, v = pcall(err)
        if ok then return tostring(v) end
        return "<function err: " .. tostring(v) .. ">"
    end
    if type(err) == "table" then
        if err.message then return tostring(err.message) end
        return tostring(err)
    end
    return tostring(err)
end

local function hook_call(func_path, extract_args)
    local ok, err = pcall(function()
        RegisterHook(func_path, function(self_w, ...)
            local self_obj = self_w and self_w:get() or nil
            local args = ""
            if extract_args then
                local ok_ex, ex = pcall(extract_args, ...)
                args = ok_ex and (" " .. ex) or (" <args err: " .. err_to_str(ex) .. ">")
            end
            log(string.format("CALL %s self=%s%s",
                func_path, fmt_obj(self_obj), args))
        end)
    end)
    if ok then
        hooked[#hooked + 1] = func_path
    else
        hook_failures[#hook_failures + 1] = func_path .. " :: " .. err_to_str(err)
    end
end

-- Common argument extractors for callbacks shaped like (int32 ID)
local function arg_id(id_w)
    if not id_w then return "id=<nil>" end
    local ok, v = pcall(function() return id_w:get() end)
    return "id=" .. (ok and tostring(v) or "<err>")
end

-- Extractor for (FString Text, bool, bool) — dialog SetDescription / button text.
local function arg_str(text_w, ...)
    if not text_w then return "text=<nil>" end
    local ok, v = pcall(function()
        local s = text_w:get()
        if type(s) == "userdata" and s.ToString then return s:ToString() end
        return tostring(s)
    end)
    return "text=" .. (ok and string.format("%q", v) or "<err>")
end

-- Extractor for UpdateButton(int32 ID, FString Text, ...) — label assignment.
local function arg_id_text(id_w, text_w, ...)
    local id = "<nil>"
    if id_w then
        local ok, v = pcall(function() return id_w:get() end)
        id = ok and tostring(v) or "<err>"
    end
    local txt = "<nil>"
    if text_w then
        local ok, v = pcall(function()
            local s = text_w:get()
            if type(s) == "userdata" and s.ToString then return s:ToString() end
            return tostring(s)
        end)
        txt = ok and string.format("%q", v) or "<err>"
    end
    return "id=" .. id .. " text=" .. txt
end

-- ============================================================
-- Native (Polaris) UFunctions worth watching
-- These are always loaded; RegisterHook is safe at mod init.
-- ============================================================

-- Practice menu: button decide callbacks are the entry point for any menu
-- choice. ID parameter tells us which item was picked.
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:InvokeDecideButton1Callback", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:InvokeDecideButton2Callback", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:InvokeDecideButton3Callback", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:InvokeDecideButton2LeftCallback", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:InvokeDecideButton2RightCallback", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:InvokeSelectButton1Callback", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:InvokeSelectButton2Callback", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:InvokeSelectButton3Callback", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:PlayAnimDecide", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:SetActiveLayer", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:SetCursorButton1", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:SetCursorButton2", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:SetCursorButton3", arg_id)
hook_call("/Script/Polaris.PolarisUMGPracticeMenu:SetCursorButton4", arg_id)

-- Replay-related (playback may go through these)
hook_call("/Script/Polaris.PolarisUMGReplayMenu:PlayReplay")
hook_call("/Script/Polaris.PolarisUMGHudReplayIndicator:SetReplayCounter")

-- Generic confirmation dialog — used for "Record this movement?".
-- SetDescription reveals which dialog instance is which; DecideMenu/
-- InvokeDecideCallback is the actual button press.
hook_call("/Script/Polaris.PolarisUMGDialog:SetDescription", arg_str)
hook_call("/Script/Polaris.PolarisUMGDialog:UpdateButton", arg_id_text)
hook_call("/Script/Polaris.PolarisUMGDialog:DecideMenu", arg_id)
hook_call("/Script/Polaris.PolarisUMGDialog:SelectMenu", arg_id)
hook_call("/Script/Polaris.PolarisUMGDialog:InvokeDecideCallback", arg_id)
hook_call("/Script/Polaris.PolarisUMGDialog:InvokeSelectCallback", arg_id)
hook_call("/Script/Polaris.PolarisUMGDialog:EnableMenu", arg_id)
hook_call("/Script/Polaris.PolarisUMGDialog:PlayAnimDecide", arg_id)
hook_call("/Script/Polaris.PolarisUMGDialog:PlayAnimIn", arg_id)
hook_call("/Script/Polaris.PolarisUMGDialog:PlayAnimOut", arg_id)

-- ============================================================
-- Blueprint-class hooks: registered lazily so they bind when the BP
-- widget is constructed (these classes don't exist until practice loads).
-- ============================================================

local bp_hooks = {
    {
        class = "/Game/UI/Practice/WBP_UI_PracticeMenu.WBP_UI_PracticeMenu_C",
        functions = {
            "OnDecideButton1",
            "OnDecideButton2",
            "OnDecideButton3",
            "OnDecideButton2Left",
            "OnDecideButton2Right",
            "OnSelectButton1",
            "OnSelectButton2",
            "OnSelectButton3",
            "SetActiveLayerFunc",
        },
    },
}

for _, entry in ipairs(bp_hooks) do
    NotifyOnNewObject(entry.class, function(obj)
        log(string.format("NEW %s -> %s", entry.class, fmt_obj(obj)))
        for _, fn in ipairs(entry.functions) do
            hook_call(entry.class .. ":" .. fn, arg_id)
        end
    end)
end

-- ============================================================
-- Keybinds for explicit dumps. F9/F10/F11/F12.
-- Use these to capture state right before/after a record/save/play action.
-- ============================================================

local function dump_class(cls)
    local ok, objs = pcall(FindAllOf, cls)
    if not ok then
        log(string.format("  %s: <err: %s>", cls, err_to_str(objs)))
        return
    end
    if not objs then
        log(string.format("  %s: <none>", cls))
        return
    end
    local count = 0
    if type(objs) == "table" then
        for i, obj in ipairs(objs) do
            log(string.format("  %s[%d] = %s", cls, i, fmt_obj(obj)))
            count = count + 1
        end
    elseif type(objs) == "userdata" and objs.ForEach then
        local ok2, err2 = pcall(function()
            objs:ForEach(function(idx, wrap)
                local w = (type(wrap) == "userdata" and wrap.get) and wrap:get() or wrap
                log(string.format("  %s[%d] = %s", cls, idx, fmt_obj(w)))
                count = count + 1
            end)
        end)
        if not ok2 then
            log(string.format("  %s: <foreach err: %s>", cls, err_to_str(err2)))
            return
        end
    else
        log(string.format("  %s: <unknown return type=%s>", cls, type(objs)))
        return
    end
    if count == 0 then log(string.format("  %s: <empty>", cls)) end
end

-- UE UClass FNames don't carry the C++ type-prefix (U for UObject, A for Actor).
-- Try both forms so we don't miss whichever UE4SS expects.
local function dump_class_both(short)
    dump_class(short)
    dump_class("U" .. short)
    dump_class("A" .. short)
end

RegisterKeyBind(Key.F9, function()
    log("=== F9: live practice/replay widgets ===")
    for _, cls in ipairs({
        "PolarisUMGPractice",
        "PolarisUMGPracticeMenu",
        "PolarisUMGDialog",
        "PolarisUMGReplay",
        "PolarisUMGReplayMenu",
        "PolarisUMGReplayControl",
        "PolarisUMGReplayDialog",
        "PolarisUMGCounterPractice",
        "PolarisUMGHudReplayIndicator",
    }) do
        dump_class_both(cls)
    end
end)

RegisterKeyBind(Key.F10, function()
    log("=== F10: outer-chain walk from first PracticeMenu ===")
    local m = FindFirstOf("PolarisUMGPracticeMenu") or FindFirstOf("UPolarisUMGPracticeMenu")
    if not m then log("  no PracticeMenu in memory") return end
    local cur = m
    for depth = 0, 12 do
        log(string.format("  depth=%d %s", depth, fmt_obj(cur)))
        local ok, outer = pcall(function() return cur:GetOuter() end)
        if not ok or not outer then break end
        if outer == cur then break end
        cur = outer
    end
end)

RegisterKeyBind(Key.F11, function()
    log("=== F11: GameMode + PlayerController outer-chain ===")
    for _, cls in ipairs({
        "APolarisBattleGameMode",
        "PolarisBattleGameMode",
        "ABP_PolarisBattleGameMode_C",
        "PolarisPlayerController",
        "APolarisPlayerController",
    }) do
        local obj = FindFirstOf(cls)
        log(string.format("  %s -> %s", cls, obj and fmt_obj(obj) or "<not found>"))
    end
end)

-- F8: dump every BP_PracticeMenu_Button_2_item_C — Button_2 row UObjects.
-- Requires the practice menu to be OPEN so the items are constructed.
-- Also reports the class's full path on the first item (use for hook paths).
RegisterKeyBind(Key.F8, function()
    log("=== F8: practice menu slot items (Button_2_item) ===")
    local cls = "BP_PracticeMenu_Button_2_item_C"
    local ok, objs = pcall(FindAllOf, cls)
    if not ok then
        log(string.format("  err: %s", err_to_str(objs)))
        return
    end
    if not objs then
        log("  none found (open the practice menu first)")
        return
    end
    if type(objs) ~= "table" then
        log(string.format("  unexpected type %s", type(objs)))
        return
    end
    log(string.format("  found %d items", #objs))

    -- Print the class's full UE path once for hook reference.
    if #objs > 0 then
        local ok_c, cls_full = pcall(function()
            local c = objs[1]:GetClass()
            return c:GetFullName()
        end)
        log(string.format("  CLASS_PATH = %s", ok_c and cls_full or "<err>"))
    end

    for i, item in ipairs(objs) do
        local addr = "?"
        local ok_a, a = pcall(function() return item:GetAddress() end)
        if ok_a then addr = string.format("0x%X", a) end

        local function safe_prop(get)
            local ok_p, v = pcall(get)
            return ok_p and v or "<err>"
        end

        local text = safe_prop(function()
            local s = item.Text
            if type(s) == "userdata" and s.ToString then return s:ToString() end
            return tostring(s)
        end)
        local text2 = safe_prop(function()
            local s = item.text2
            if type(s) == "userdata" and s.ToString then return s:ToString() end
            return tostring(s)
        end)
        local rate = safe_prop(function() return item.Rate end)
        local antenna = safe_prop(function() return item.antenna end)
        local enable = safe_prop(function() return item.isEnable end)
        local layer = safe_prop(function() return item.Layer end)

        log(string.format("  [%d] %s Text=%q text2=%q Rate=%s antenna=%s enable=%s layer=%s",
            i, addr, tostring(text), tostring(text2),
            tostring(rate), tostring(antenna), tostring(enable), tostring(layer)))
    end
end)

-- Once a Button_2_item is loaded, register a UFunction hook on its UpdateData
-- using the live class's full path. The model that calls UpdateData is whoever
-- holds persistent slot state — catching it shows the live Text/text2/Rate per
-- slot so we can identify the recorded one without depending on stale items.
local update_data_hooked = false

local function f_args_updatedata(text_w, text2_w, rate_w, antenna_w, enable_w, layer_w, isdef_w)
    local function s(w)
        if not w then return "?" end
        local ok_g, v = pcall(function() return w:get() end)
        if not ok_g then return "<err>" end
        if type(v) == "userdata" and v.ToString then
            local ok_t, t = pcall(function() return v:ToString() end)
            return ok_t and t or tostring(v)
        end
        return tostring(v)
    end
    return string.format("Text=%q text2=%q Rate=%s antenna=%s enable=%s layer=%s isDef=%s",
        s(text_w), s(text2_w), s(rate_w), s(antenna_w), s(enable_w), s(layer_w), s(isdef_w))
end

local function try_register_updatedata()
    if update_data_hooked then return true end
    local ok, objs = pcall(FindAllOf, "BP_PracticeMenu_Button_2_item_C")
    if not ok or type(objs) ~= "table" or #objs == 0 then return false end

    local ok_c, cls_full = pcall(function() return objs[1]:GetClass():GetFullName() end)
    if not ok_c then return false end

    -- GetFullName returns "BlueprintGeneratedClass /Game/.../BP_X.BP_X_C"
    local path = tostring(cls_full):match("(/Game/[%w_%./]+)")
    if not path then
        log(string.format("UpdateData hook: no /Game/ path in %q", tostring(cls_full)))
        return false
    end
    local hook_path = path .. ":UpdateData"
    log(string.format("UpdateData hook: registering %s", hook_path))
    hook_call(hook_path, f_args_updatedata)
    update_data_hooked = true
    return true
end

-- Poll every 2s until we see the class. Stops polling once registered.
if LoopAsync then
    LoopAsync(2000, function()
        if try_register_updatedata() then return true end -- true = stop loop
        return false
    end)
else
    log("LoopAsync unavailable — UpdateData hook will only register on first F8 press")
end

-- Same pattern for the VISIBLE widget UWBP_UI_PracticeMenu_Button_2_C, which is
-- where slot-recording state actually lives (is_active bool + UpdateRateParam).
local visible_hooked = false

local function f_args_visible_updatedata(item_w)
    if not item_w then return "item=<nil>" end
    local ok, item = pcall(function() return item_w:get() end)
    if not ok or not item then return "item=<err>" end
    local fname = "?"
    local ok_n, n = pcall(function() return item:GetFullName() end)
    if ok_n then fname = n end
    local addr = "?"
    local ok_a, a = pcall(function() return item:GetAddress() end)
    if ok_a then addr = string.format("0x%X", a) end
    return string.format("item=%s %s", addr, fname)
end

local function f_args_rate(rate_w)
    if not rate_w then return "Rate=<nil>" end
    local ok, v = pcall(function() return rate_w:get() end)
    return "Rate=" .. (ok and tostring(v) or "<err>")
end

local function try_register_visible_hooks()
    if visible_hooked then return true end
    local ok, objs = pcall(FindAllOf, "WBP_UI_PracticeMenu_Button_2_C")
    if not ok or type(objs) ~= "table" or #objs == 0 then return false end

    local ok_c, cls_full = pcall(function() return objs[1]:GetClass():GetFullName() end)
    if not ok_c then return false end
    local path = tostring(cls_full):match("(/Game/[%w_%./]+)")
    if not path then
        log(string.format("Visible widget hook: no /Game/ path in %q", tostring(cls_full)))
        return false
    end
    log(string.format("Visible widget hook: registering on %s", path))
    hook_call(path .. ":UpdateData", f_args_visible_updatedata)
    hook_call(path .. ":UpdateRateParam", f_args_rate)
    hook_call(path .. ":OnListItemObjectSet", f_args_visible_updatedata)
    visible_hooked = true
    return true
end

if LoopAsync then
    LoopAsync(2000, function()
        if try_register_visible_hooks() then return true end
        return false
    end)
end

-- Dynamically register hooks on the BP dialog class (WBP_UI_Dialog_C).
-- Native UPolarisUMGDialog hooks don't fire because the BP overrides them.
-- We need to hook the BP function path discovered from a live instance.
local dialog_bp_hooked = false

local function try_register_dialog_bp_hooks()
    if dialog_bp_hooked then return true end
    local ok, objs = pcall(FindAllOf, "WBP_UI_Dialog_C")
    if not ok or type(objs) ~= "table" or #objs == 0 then return false end

    local ok_c, cls_full = pcall(function() return objs[1]:GetClass():GetFullName() end)
    if not ok_c then return false end
    local path = tostring(cls_full):match("(/Game/[%w_%./]+)")
    if not path then
        log(string.format("Dialog BP hook: no /Game/ path in %q", tostring(cls_full)))
        return false
    end
    log(string.format("Dialog BP hook: registering on %s", path))
    -- The crucial ones for the YES press:
    hook_call(path .. ":InvokeDecideCallback", arg_id)
    hook_call(path .. ":InvokeSelectCallback", arg_id)
    hook_call(path .. ":DecideMenu", arg_id)
    hook_call(path .. ":SelectMenu", arg_id)
    -- Surface the dialog's text so we know which dialog is which:
    hook_call(path .. ":SetDescription", arg_str)
    hook_call(path .. ":UpdateButton", arg_id_text)
    -- Animation events that bracket the commit:
    hook_call(path .. ":PlayAnimDecide", arg_id)
    hook_call(path .. ":PlayAnimIn", arg_id)
    hook_call(path .. ":PlayAnimOut", arg_id)
    hook_call(path .. ":Construct")
    -- Animation completion events — these fire when each animation ENDS.
    -- The BP graph hooks Anim_Out_B finishing → calls save logic.
    for _, anim_evt in ipairs({
        "WidgetAnimationEvt_Anim_In_K2Node_WidgetAnimationEvent_0",
        "WidgetAnimationEvt_Anim_In_L_K2Node_WidgetAnimationEvent_1",
        "WidgetAnimationEvt_Anim_In_R_K2Node_WidgetAnimationEvent_2",
        "WidgetAnimationEvt_Anim_Out_K2Node_WidgetAnimationEvent_3",
        "WidgetAnimationEvt_Anim_Out_L_K2Node_WidgetAnimationEvent_4",
        "WidgetAnimationEvt_Anim_Out_R_K2Node_WidgetAnimationEvent_5",
        "WidgetAnimationEvt_Anim_In_B_K2Node_WidgetAnimationEvent_6",
        "WidgetAnimationEvt_Anim_Out_B_K2Node_WidgetAnimationEvent_7",
    }) do
        hook_call(path .. ":" .. anim_evt)
    end
    dialog_bp_hooked = true
    return true
end

if LoopAsync then
    LoopAsync(2000, function()
        if try_register_dialog_bp_hooks() then return true end
        return false
    end)
end

-- F7: same idea but for Button_1 items (top-level categories) and Button_3
-- (per-slot sub-action menu items: Record / Erase / Action Interval / ...).
RegisterKeyBind(Key.F7, function()
    log("=== F7: dump Button_1 + Button_3 list items (open the menu first) ===")
    for _, cls in ipairs({ "BP_PracticeMenu_Button_1_Item_C", "BP_PracticeMenu_Button_3_item_C" }) do
        local ok, objs = pcall(FindAllOf, cls)
        if not ok or not objs then
            log(string.format("  %s: <none/err>", cls))
        elseif type(objs) == "table" then
            log(string.format("  %s: %d items", cls, #objs))
            for i, item in ipairs(objs) do
                local addr_ok, addr = pcall(function() return string.format("0x%X", item:GetAddress()) end)
                local txt_ok, txt = pcall(function()
                    local s = item.Text
                    if type(s) == "userdata" and s.ToString then return s:ToString() end
                    return tostring(s)
                end)
                log(string.format("    [%d] %s Text=%q",
                    i, addr_ok and addr or "?", tostring(txt_ok and txt or "<err>")))
            end
        end
    end
end)

-- F6: force-try registering all dynamic hooks now.
RegisterKeyBind(Key.F6, function()
    log("=== F6: try register hooks ===")
    if update_data_hooked then log("  Button_2_item:UpdateData already hooked")
    elseif try_register_updatedata() then log("  Button_2_item:UpdateData hooked")
    else log("  Button_2_item not loaded yet") end

    if visible_hooked then log("  Button_2 (visible) already hooked")
    elseif try_register_visible_hooks() then log("  Button_2 (visible) hooked")
    else log("  Button_2 visible widget not loaded yet") end

    if dialog_bp_hooked then log("  Dialog BP already hooked")
    elseif try_register_dialog_bp_hooks() then log("  Dialog BP hooked")
    else log("  Dialog BP not loaded yet — open a confirm dialog once") end
end)

-- Helper for safely reading a UE FText/FString or any UObject's ToString.
local function safe_str(v)
    if v == nil then return "<nil>" end
    if type(v) == "userdata" and v.ToString then
        local ok, s = pcall(function() return v:ToString() end)
        if ok then return s end
    end
    return tostring(v)
end

local function safe_tb_text(w, field)
    local ok, v = pcall(function()
        local tb = w[field]
        if not tb then return "<no_tb>" end
        return tb:GetText()
    end)
    if not ok then return "<err>" end
    return safe_str(v)
end

-- Dump a single widget with every interesting text-block and property.
local function dump_widget_full(w, idx, prefix)
    prefix = prefix or "  "
    local function sp(get)
        local ok, v = pcall(get)
        if not ok then return "<err>" end
        return safe_str(v)
    end
    local addr = sp(function() return string.format("0x%X", w:GetAddress()) end)
    local cls = sp(function() return w:GetClass():GetFName():ToString() end)
    local is_active = sp(function() return w.is_active end)
    local item_addr = sp(function()
        local li = w.list_item
        if li then return string.format("0x%X", li:GetAddress()) end
        return "<nil>"
    end)
    log(string.format("%s[%d] %s %s active=%s item=%s",
        prefix, idx, cls, addr, is_active, item_addr))
    log(string.format("%s     Menu_OFF=%q Menu_ON=%q",
        prefix, safe_tb_text(w, "TB_Menu_OFF"), safe_tb_text(w, "TB_Menu_ON")))
    log(string.format("%s     Rate1_OFF=%q Rate1_ON=%q Rate2_OFF=%q Rate2_ON=%q",
        prefix,
        safe_tb_text(w, "TB_Rate_1_OFF"), safe_tb_text(w, "TB_Rate_1_ON"),
        safe_tb_text(w, "TB_Rate_2_OFF"), safe_tb_text(w, "TB_Rate_2_ON")))
    log(string.format("%s     value_OFF=%q value_ON=%q",
        prefix, safe_tb_text(w, "TB_value_OFF"), safe_tb_text(w, "TB_value_ON")))
end

-- F5: dump every Button_2 VISIBLE widget with ALL text blocks
RegisterKeyBind(Key.F5, function()
    log("=== F5: Button_2_C visible widgets (all text blocks) ===")
    local ok, objs = pcall(FindAllOf, "WBP_UI_PracticeMenu_Button_2_C")
    if not ok or type(objs) ~= "table" then
        log("  not found")
        return
    end
    log(string.format("  found %d widgets", #objs))
    for i, w in ipairs(objs) do dump_widget_full(w, i) end
end)

-- F4: enumerate every UMG widget class with "Practice" in the name and dump
-- its instances. Use to find the slot-row widget class — open the slot screen
-- showing "Action Frequency 1" and press F4.
RegisterKeyBind(Key.F4, function()
    log("=== F4: dump every Practice-named widget class ===")
    local candidates = {
        "WBP_UI_PracticeMenu_Button_1_C",
        "WBP_UI_PracticeMenu_Button_2_C",
        "WBP_UI_PracticeMenu_Button_3_C",
        "WBP_UI_PracticeMenu_Button_4_C",
        "WBP_UI_PracticeMenu_Menu_3_C",
        "WBP_UI_Practice_S2_C",
        "WBP_UI_Practice_History_C",
        "WBP_UI_Practice_History_Line_C",
        "WBP_UI_Practice_Info_S2_C",
        "WBP_UI_Practice_Info_L_S2_C",
        "WBP_UI_Practice_Info_R_S2_C",
        "WBP_UI_Practice_Simple_C",
        "WBP_UI_Practice_Timer_C",
        "WBP_UI_Practice_CountDown_C",
        "WBP_UI_Practice_TIPS_C",
        "WBP_UI_Practice_ArrowButton_C",
        "WBP_UI_Practice_HP_Text_C",
        "WBP_UI_Practice_ComboChallenge_C",
        "WBP_UI_Practice_ComboChallenge_Line_C",
        "WBP_UI_CounterPractice_C",
        "WBP_UI_CounterPractice_Button_C",
        "WBP_UI_CounterPractice_Command_C",
        "WBP_UI_CounterPractice_Win_C",
        "BP_PracticeMenu_Button_1_Item_C",
        "BP_PracticeMenu_Button_2_item_C",
        "BP_PracticeMenu_Button_3_item_C",
        "BP_PracticeMenu_Parent_item_C",
        "BP_Practice_History_Line_Item_C",
    }
    for _, cls in ipairs(candidates) do
        local ok, objs = pcall(FindAllOf, cls)
        if ok and type(objs) == "table" and #objs > 0 then
            log(string.format("  %s: %d instances", cls, #objs))
        end
    end
end)

RegisterKeyBind(Key.F12, function()
    log("=== F12: marker - timestamp this in the log ===")
end)

-- ============================================================
-- Startup report
-- ============================================================

log("loaded")
log(string.format("hooked %d functions:", #hooked))
for _, f in ipairs(hooked) do log("  + " .. f) end
if #hook_failures > 0 then
    log(string.format("hook failures (%d):", #hook_failures))
    for _, f in ipairs(hook_failures) do log("  ! " .. f) end
end
log("Keybinds: F4=enum practice classes, F5=visible widgets (all TBs), F6=retry hooks, F7=B1/B3 items, F8=slot items, F9=widgets, F10=outer chain, F11=game mode, F12=marker")
