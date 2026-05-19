-- UPolarisUMGTextMenu UClass introspection probe.
--
-- Re-resolves the same find_class function our DLL uses, then walks the
-- UClass for UPolarisUMGTextMenu (and UPolarisUMGDialog as fallback).
-- Dumps:
--   - UClass header (16 qwords) — vtable + UObject base + UStruct fields
--   - The Children linked list (UStruct::Children at +0x38 in UE 5.2),
--     printing each child's address and head qwords. UFunctions live in
--     that list; their names are FNames at +0x18.
--
-- The user pastes the output and we identify the method that adds a
-- text row (likely "AddItem", "AddRow", "AppendItem", or similar). FName
-- decoding (NamePool) is a separate follow-up if needed — for now we
-- print raw ComparisonIndex values so we can correlate against known
-- UE5 FName tables.
--
-- Pre-flight:
--   1. Attach CE to Polaris-Win64-Shipping.exe.
--   2. Be anywhere in-game (UPolarisUMGTextMenu's UClass is registered
--      at engine init, so it's available the moment Polaris is loaded).
--   3. Execute this script in CE's Lua Engine.

local MODULE = "Polaris-Win64-Shipping.exe"

-- Same pattern our DLL pins (lifted from Irony).
local PAT_FIND_UNREAL_CLASS =
    "45 33 C0 49 8B CF E8 ?? ?? ?? ?? 48 8B 4C 24 60"

-- UE 5.2 UStruct layout — Children is the head of a linked list of UField
-- objects (UFunctions for a class). Each UField has Next at +0x28.
-- UObject's NamePrivate (FName) lives at +0x18.
local UCLASS_CHILDREN_OFF = 0x38   -- UStruct::Children
local UFIELD_NEXT_OFF     = 0x28   -- UField::Next
local UOBJECT_NAME_OFF    = 0x18   -- UObject::NamePrivate (FName, 8 bytes)
local UOBJECT_OUTER_OFF   = 0x20   -- UObject::OuterPrivate

local CLASSES_TO_DUMP = {
    "/Script/Polaris.PolarisUMGTextMenu",
    "/Script/Polaris.PolarisUMGDialog",
}

local MAX_CHILDREN_PER_CLASS = 80   -- safety bound

local function logf(fmt, ...) print(string.format(fmt, ...)) end

local function moduleBase()
    local b = getAddress(MODULE)
    return (b and b ~= 0) and b or nil
end

-- Run an AOB scan in .text (executable, non-writable pages). Returns
-- the first match address, or nil. Warns on duplicates.
local function aob_first(pattern)
    local results = AOBScan(pattern, "+X-W")
    if results == nil then return nil end
    local addr = nil
    if results.Count and results.Count >= 1 then
        addr = tonumber("0x" .. results[0])
        if results.Count > 1 then
            logf("WARN: pattern matched %d times, using first (0x%X)",
                 results.Count, addr)
        end
    end
    results.destroy()
    return addr
end

-- i32 RIP-relative resolve: instruction's disp32 at `at`, target = at+4+disp.
local function rip_relative(at)
    local disp = readInteger(at)            -- signed 32
    return at + 4 + disp
end

-- Convert a Lua string to a UTF-16LE wide null-terminated buffer in
-- process memory. Returns the address. We need this to pass a TCHAR*
-- name into Polaris's findUnrealClass.
local function alloc_widestring(s)
    local total = (#s + 1) * 2  -- nul-terminated wchar_t
    local mem = allocateMemory(total)
    if not mem or mem == 0 then return nil end
    for i = 1, #s do
        local b = string.byte(s, i)
        writeBytes(mem + (i - 1) * 2, b, 0)
    end
    writeBytes(mem + #s * 2, 0, 0)  -- terminator
    return mem
end

-- ---------------------------------------------------------------------------

local base = moduleBase()
if not base then
    print("Polaris not loaded — attach CE to Tekken first.")
    return
end
logf("Polaris base: 0x%X", base)

local pat_addr = aob_first(PAT_FIND_UNREAL_CLASS)
if not pat_addr then
    print("PAT_FIND_UNREAL_CLASS missed — pattern may have shifted")
    return
end
local find_class_addr = rip_relative(pat_addr + 7)
logf("findUnrealClass: 0x%X (Polaris+0x%X)",
     find_class_addr, find_class_addr - base)

-- ---------------------------------------------------------------------------
-- Build a fastcall-callable surface. Windows x64 ABI: RCX, RDX, R8, R9.
-- We use executeCodeEx with a small synthetic call frame so we don't
-- crash; CE's executeCodeEx handles the shadow-space + alignment.
-- ---------------------------------------------------------------------------

local function find_class(outer, name_wstr_addr, exact_class)
    -- executeCodeEx(returnsFloat, address, paramTypes, ...args)
    return executeCodeEx(
        nil,                  -- no special return convention
        find_class_addr,
        outer or 0,
        name_wstr_addr,
        exact_class and 1 or 0
    )
end

-- ---------------------------------------------------------------------------

local function dump_uclass(path)
    print("")
    logf("== %s ==", path)
    local wstr = alloc_widestring(path)
    if not wstr then print("  alloc_widestring failed"); return end
    local cls = find_class(0, wstr, true)
    if not cls or cls == 0 then
        logf("  FindObject returned null")
        deAlloc(wstr)
        return
    end
    logf("  UClass*           = 0x%X", cls)
    logf("  outer             = 0x%X", readPointer(cls + UOBJECT_OUTER_OFF) or 0)

    -- FName at +0x18 is two uint32: ComparisonIndex, Number.
    local name_idx = readInteger(cls + UOBJECT_NAME_OFF) & 0xFFFFFFFF
    local name_num = readInteger(cls + UOBJECT_NAME_OFF + 4) & 0xFFFFFFFF
    logf("  name (FName)      = idx=%u num=%u", name_idx, name_num)

    -- Header dump: 32 qwords = 256 bytes. Lots of info — vtable, flags,
    -- SuperStruct, Children, ChildProperties, PropertiesSize, etc.
    print("  header (16 qwords from offset 0):")
    for i = 0, 15 do
        local q = readQword(cls + i * 8) or 0
        logf("    +0x%02X: 0x%016X", i * 8, q)
    end

    -- Walk Children (UFunctions are stored here for UClass).
    local child = readPointer(cls + UCLASS_CHILDREN_OFF)
    if not child or child == 0 then
        logf("  Children (+0x%X) = null — class has no fields registered",
             UCLASS_CHILDREN_OFF)
        deAlloc(wstr)
        return
    end
    logf("  Children (+0x%X):", UCLASS_CHILDREN_OFF)
    for step = 1, MAX_CHILDREN_PER_CLASS do
        if not child or child == 0 then break end
        local cidx = readInteger(child + UOBJECT_NAME_OFF) & 0xFFFFFFFF
        local cnum = readInteger(child + UOBJECT_NAME_OFF + 4) & 0xFFFFFFFF
        local outer_ptr = readPointer(child + UOBJECT_OUTER_OFF) or 0
        -- Also dump the first qword (vtable) and a few following words
        -- so we can later cross-check class identity if needed.
        local vt = readPointer(child) or 0
        logf("    [%2d] 0x%X  vt=0x%X  FName(idx=%u, num=%u)  outer=0x%X",
             step, child, vt, cidx, cnum, outer_ptr)
        child = readPointer(child + UFIELD_NEXT_OFF)
    end

    deAlloc(wstr)
end

for _, path in ipairs(CLASSES_TO_DUMP) do
    dump_uclass(path)
end

print("")
print("done.")
