-- Disassemble the candidate pool1 allocator function so we can see the
-- alloc call, the size argument, and the function's calling convention.
--
-- Source: probe_pool1_writers.lua found one write to the pool1 pointer
-- slot, with the enclosing function starting at Polaris+0x18E8E00.

local MODULE          = "Polaris-Win64-Shipping.exe"
local FUNC_START_RVA  = 0x18E8E00
local MAX_BYTES       = 0x200      -- 512 bytes
local MAX_INSTRS      = 96

local base = getAddress(MODULE)
if not base or base == 0 then
    print("Polaris not loaded")
    return
end

local start = base + FUNC_START_RVA
print(string.format("Disassembling Polaris+0x%X (abs 0x%X):",
                    FUNC_START_RVA, start))
print("(stops at first RET when followed by INT3 padding)")
print("")

-- CE's disassemble() returns: "ModuleName+RVA - <hex bytes> - <mnemonic>".
-- We just keep everything after the last " - " (plain text split, no regex).
local function extract_mnemonic(text)
    if not text then return "??" end
    local last = text
    local s = 1
    while true do
        local a, b = text:find(" - ", s, true)  -- plain-text find
        if not a then break end
        last = text:sub(b + 1)
        s = b + 1
    end
    return last
end

local pc = start
local last_was_ret = false
for i = 1, MAX_INSTRS do
    if pc - start >= MAX_BYTES then break end

    local size = getInstructionSize(pc) or 0
    if size == 0 then
        print(string.format("  Polaris+0x%X  (decode failed)", pc - base))
        break
    end

    local mnem = extract_mnemonic(disassemble(pc))
    print(string.format("  Polaris+0x%-6X  %s", pc - base, mnem))

    local first_byte = readBytes(pc, 1, false) or 0
    if last_was_ret and first_byte == 0xCC then break end
    last_was_ret = (first_byte == 0xC3 or first_byte == 0xC2)

    pc = pc + size
end

print("")
print("done.")
