-- Pool1 allocator probe.
--
-- Pool1 (the practice-mode recording-buffer pool) is currently allocated
-- lazily by the game on the user's first record. We want to allocate it
-- ourselves at practice-mode entry so autoload works without that step.
--
-- This probe finds every `MOV [rip+disp32], <reg64>` instruction in
-- Polaris's .text whose displacement points to the pool1 pointer slot
-- (Polaris+0x986AC70). Most likely 1–3 hits — the allocator's store,
-- and possibly a reset-to-null path.
--
-- Once we have an address, the next step is to find the enclosing
-- function (look backward for `CC CC` int3 padding or a typical prologue
-- like `40 53 48 83 EC ??`) and decompile/disassemble it.
--
-- Run pre-record: Polaris loaded, you in practice mode but haven't
-- pressed Record yet. The probe scans .text — no breakpoints, safe.

local MODULE     = "Polaris-Win64-Shipping.exe"
local POOL1_RVA  = 0x986AC70

local function logf(fmt, ...) print(string.format(fmt, ...)) end

local function moduleBase()
    local b = getAddress(MODULE)
    return (b and b ~= 0) and b or nil
end

local base = moduleBase()
if not base then
    print("Polaris not loaded — attach CE to Tekken first.")
    return
end
local pool1_slot = base + POOL1_RVA
logf("Polaris base: 0x%X", base)
logf("pool1 slot:   0x%X", pool1_slot)
logf("pool1 value:  0x%X (current contents — non-zero means pool already allocated)",
     readPointer(pool1_slot) or 0)

-- Every `MOV [rip+disp32], <reg64>` is REX.W + 0x89 + ModR/M with mod=00
-- and rm=101 (= 5). The ModR/M `reg` field plus REX.R encodes the source.
-- Instruction length: 7 bytes (REX + 0x89 + ModR/M + 4-byte disp32).
local patterns = {
    {"48 89 05 ?? ?? ?? ??", "rax"},
    {"48 89 0D ?? ?? ?? ??", "rcx"},
    {"48 89 15 ?? ?? ?? ??", "rdx"},
    {"48 89 1D ?? ?? ?? ??", "rbx"},
    {"48 89 25 ?? ?? ?? ??", "rsp"},
    {"48 89 2D ?? ?? ?? ??", "rbp"},
    {"48 89 35 ?? ?? ?? ??", "rsi"},
    {"48 89 3D ?? ?? ?? ??", "rdi"},
    {"4C 89 05 ?? ?? ?? ??", "r8"},
    {"4C 89 0D ?? ?? ?? ??", "r9"},
    {"4C 89 15 ?? ?? ?? ??", "r10"},
    {"4C 89 1D ?? ?? ?? ??", "r11"},
    {"4C 89 25 ?? ?? ?? ??", "r12"},
    {"4C 89 2D ?? ?? ?? ??", "r13"},
    {"4C 89 35 ?? ?? ?? ??", "r14"},
    {"4C 89 3D ?? ?? ?? ??", "r15"},
}

-- Also include `MOV qword ptr [rip+disp32], imm32` — clears the slot to
-- a small immediate (typically 0). Encoding: 48 C7 05 disp32 imm32.
-- Instruction length: 11 bytes. Target = addr + 7 + disp32 (the disp32
-- is the 4 bytes BEFORE the imm32, not after).
local imm_pattern = "48 C7 05 ?? ?? ?? ?? ?? ?? ?? ??"

print("scanning .text ...")

local hits = {}

for _, p in ipairs(patterns) do
    local results = AOBScan(p[1], "+X-W")
    if results then
        for i = 0, results.Count - 1 do
            local addr = tonumber("0x" .. results[i])
            local disp = readInteger(addr + 3)
            if addr + 7 + disp == pool1_slot then
                table.insert(hits, {addr=addr, src=p[2], kind="mov-reg"})
            end
        end
        results.destroy()
    end
end

local imm_results = AOBScan(imm_pattern, "+X-W")
if imm_results then
    for i = 0, imm_results.Count - 1 do
        local addr = tonumber("0x" .. imm_results[i])
        local disp = readInteger(addr + 3)
        if addr + 7 + disp == pool1_slot then
            local imm = readInteger(addr + 7)
            table.insert(hits, {addr=addr, src=string.format("imm32=0x%X", imm), kind="mov-imm"})
        end
    end
    imm_results.destroy()
end

logf("matches: %d", #hits)
for i, h in ipairs(hits) do
    -- Print the instruction bytes and 24 bytes of forward context.
    local ctx = {}
    local instr_len = (h.kind == "mov-imm") and 11 or 7
    for off = 0, instr_len - 1 do
        local b = readBytes(h.addr + off, 1, false) or 0
        table.insert(ctx, string.format("%02X", b))
    end
    table.insert(ctx, "|")
    for off = instr_len, instr_len + 23 do
        local b = readBytes(h.addr + off, 1, false) or 0
        table.insert(ctx, string.format("%02X", b))
    end

    -- Walk backward up to 0x600 bytes to find the function start. Stop
    -- at the first `CC CC` int3 padding pair (typical alignment between
    -- functions in MSVC output).
    local func_start = nil
    for back = 1, 0x600 do
        local b  = readBytes(h.addr - back,     1, false) or 0
        local bp = readBytes(h.addr - back - 1, 1, false) or 0
        if b == 0xCC and bp == 0xCC then
            func_start = h.addr - back + 1
            break
        end
    end

    logf("  [%d] kind=%s  src=%s", i, h.kind, h.src)
    logf("       write at: 0x%X  (Polaris+0x%X)", h.addr, h.addr - base)
    if func_start then
        logf("       func start: 0x%X  (Polaris+0x%X)  [first byte after CC-CC]",
             func_start, func_start - base)
    else
        logf("       func start: not found within 0x600 bytes (try wider scan)")
    end
    logf("       bytes: %s", table.concat(ctx, " "))
end

print("done.")
