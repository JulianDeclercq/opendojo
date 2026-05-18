#include "dialog.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>

#include "log.hpp"
#include "memory.hpp"

// =============================================================================
// DIALOG PHASE-1 — UPolarisDialogFunctionLibrary via ProcessEvent.
// =============================================================================
//
// Strategy: every reflected UFunction in UE5 can be invoked by
// ProcessEvent(UFunction*, void* parms). We resolve the BPFunctionLibrary
// CDO once, look up its UFunctions by name, build a packed parms blob,
// and call ProcessEvent through the CDO's vtable.
//
// Open question this commit doesn't answer at compile time: the byte
// offset of ProcessEvent in UObject's vtable. UE 5.2 reference is slot
// 67 (= byte offset 0x218 on x64), but the value drifts between game
// builds. dump_pe_vtable() exists to identify the right slot from a
// runtime log; the constant below is the initial guess.

namespace opendojo::dialog {

namespace {

// -----------------------------------------------------------------------------
// UE5 primitives — minimal subset for Phase-1.
//
// We deliberately use bare structs (no UE typedefs) so this TU is
// self-contained and doesn't bind us to specific UE include paths.
// -----------------------------------------------------------------------------

struct UE_FString {
    wchar_t*     data = nullptr;
    std::int32_t num  = 0;   // Number of wchar_t including null terminator.
    std::int32_t max  = 0;
};

template <typename T>
struct UE_TArray {
    T*           data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};

// FPolarisDialogButtonParam — verified from Polaris.hpp:3685.
struct FPolarisDialogButtonParam {
    UE_FString   Text;                // 0x00 (16 bytes)
    bool         isEnable;            // 0x10
    std::uint8_t pad_0x11[3];         // align next field to 4
    // Dynamic delegate is 16 bytes (UObject* + FName). Zero-init = unbound.
    // Layout: { UObject* Object; std::uint64_t FunctionFName; } — 16 bytes.
    std::uint8_t OnDecide[16];        // 0x14
    bool         IsTextId;            // 0x24
    bool         IsGhost;             // 0x25
    std::uint8_t pad_0x26[2];         // tail align to 0x28
};
static_assert(sizeof(FPolarisDialogButtonParam) == 0x28,
              "FPolarisDialogButtonParam expected size 0x28 (Polaris.hpp:3685)");

// OpenDialog(FString, int32, TArray, bool, int32) — Polaris.hpp:11286.
// UE5 packs UFunction parameter frames tightly with platform alignment.
// On x64 the natural layout is:
//   0x00 FString Description       (16)
//   0x10 int32   defaultCursor     (4)
//   0x14 (pad to 8 for TArray)     (4)
//   0x18 TArray  Params            (16)
//   0x28 bool    IsTextId          (1)
//   0x29 (pad)                     (3)
//   0x2C int32   display_side      (4)
//   0x30 total
struct OpenDialog_Args {
    UE_FString                            Description;
    std::int32_t                          defaultCursor;
    std::int32_t                          _pad_after_cursor;
    UE_TArray<FPolarisDialogButtonParam>  Params;
    bool                                  IsTextId;
    std::uint8_t                          _pad_after_textid[3];
    std::int32_t                          display_side;
};
static_assert(sizeof(OpenDialog_Args) == 0x30,
              "OpenDialog_Args expected size 0x30");

// GetDialogCursor() -> int32. UFunction return values live at the tail
// of the parms blob.
struct GetDialogCursor_Args {
    std::int32_t ReturnValue;
};

struct IsDialogDecided_Args {
    bool ReturnValue;
};

struct IsDialogClosed_Args {
    bool ReturnValue;
};

// CloseDialog() — no args, no return.

// -----------------------------------------------------------------------------
// ProcessEvent vtable slot. EDIT AFTER running dump_pe_vtable() once.
//
// Tekken 8 build: slot 77 (byte offset 0x268). Identified by dumping
// the BPFL CDO vtable and decompiling — RVA 0x317D5C0 has the canonical
// ProcessEvent signature: TLS access, FFrame setup via PTR_FUN_147a82b68,
// param-blob memcpy, property-list walk at +0xC0, FFrame::Invoke tail.
// -----------------------------------------------------------------------------
constexpr int PROCESS_EVENT_VTABLE_SLOT = 77;

// Dialog manager singleton + factory — found via runtime analysis.
// OpenDialog's C++ static reads [Polaris+0x9B7A108]; if null it silently
// returns. The factory at Polaris+0x5FD0CB0 allocates a 0x50-byte object
// and runs the constructor at Polaris+0x5E2F310 which stores the result
// at the singleton slot.
constexpr std::uintptr_t DIALOG_MGR_SINGLETON_RVA = 0x9B7A108;
constexpr std::uintptr_t DIALOG_MGR_FACTORY_RVA   = 0x5FD0CB0;

using DialogMgrFactoryFn = void* (*)();

using ProcessEventFn = void (*)(void* self, void* function, void* parms);

// findUnrealClass is resolved by native_menu.cpp. To keep this module
// self-contained, we duplicate the pattern resolution. Tiny cost, avoids
// header coupling.
constexpr const char* PAT_FIND_UNREAL_CLASS =
    "45 33 C0 49 8B CF E8 ?? ?? ?? ?? 48 8B 4C 24 60";

using FindUnrealClassFn = void* (*)(void* outer,
                                    const wchar_t* name,
                                    bool exact_class);

// -----------------------------------------------------------------------------
// UE5.2 layout offsets (confirmed by native_menu.cpp comments).
// -----------------------------------------------------------------------------
constexpr std::ptrdiff_t UCLASS_CDO_OFF             = 0x150;  // ClassDefaultObject*
constexpr std::ptrdiff_t USTRUCT_CHILDREN_OFF       = 0x48;   // UField* Children
constexpr std::ptrdiff_t UFIELD_NEXT_OFF            = 0x28;   // UField* Next
constexpr std::ptrdiff_t UOBJECT_NAME_OFF           = 0x18;   // FName NamePrivate

// FNamePool (from native_menu.cpp, same constants).
constexpr std::uintptr_t FNAME_POOL_RVA     = 0x9955480;
constexpr std::ptrdiff_t POOL_BLOCKS_OFFSET = 0x10;
constexpr std::uint32_t  FNAME_BLOCK_MASK   = 0x1FFF;
constexpr std::uint32_t  FNAME_STRIDE_MASK  = 0xFFFF;
constexpr std::uint32_t  FNAME_BLOCK_SHIFT  = 16;

// SEH-protected reads — native_menu.cpp keeps these in an anon namespace.
// Duplicate locally rather than touch a working module.
bool seh_read_u64(std::uintptr_t addr, std::uint64_t* out) {
    __try { std::memcpy(out, reinterpret_cast<const void*>(addr), 8); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_read_u32(std::uintptr_t addr, std::uint32_t* out) {
    __try { std::memcpy(out, reinterpret_cast<const void*>(addr), 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_read_u16(std::uintptr_t addr, std::uint16_t* out) {
    __try { std::memcpy(out, reinterpret_cast<const void*>(addr), 2); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Decode an FName index to ASCII. Mirrors native_menu.cpp::decode_fname.
bool decode_fname(std::uint32_t idx, char* out_buf, std::size_t out_buf_size) {
    if (!out_buf || out_buf_size == 0) return false;
    out_buf[0] = '\0';
    if (idx == 0) {
        if (out_buf_size >= 5) std::memcpy(out_buf, "None", 5);
        return true;
    }
    auto module_base = memory::polaris_base();
    if (!module_base) return false;
    std::uintptr_t pool = module_base + FNAME_POOL_RVA;
    std::uint32_t block_idx = (idx >> FNAME_BLOCK_SHIFT) & FNAME_BLOCK_MASK;
    std::uint32_t stride    = idx & FNAME_STRIDE_MASK;

    std::uint64_t block_ptr = 0;
    if (!seh_read_u64(pool + POOL_BLOCKS_OFFSET + block_idx * 8, &block_ptr)
        || !block_ptr) return false;

    std::uintptr_t entry = static_cast<std::uintptr_t>(block_ptr)
                         + static_cast<std::uintptr_t>(stride) * 2;
    std::uint16_t header = 0;
    if (!seh_read_u16(entry, &header)) return false;

    bool          is_wide   = (header & 1) != 0;
    std::uint32_t len_chars = header >> 6;
    if (len_chars == 0 || len_chars > 1023) return false;

    std::uintptr_t data = entry + 2;
    std::size_t i;
    for (i = 0; i < len_chars && i + 1 < out_buf_size; ++i) {
        if (is_wide) {
            std::uint16_t w = 0;
            if (!seh_read_u16(data + i * 2, &w)) break;
            out_buf[i] = (w > 0x7F) ? '?' : static_cast<char>(w);
        } else {
            std::uint8_t b = 0;
            __try { b = *reinterpret_cast<const std::uint8_t*>(data + i); }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            out_buf[i] = (b > 0x7F) ? '?' : static_cast<char>(b);
        }
    }
    out_buf[i] = '\0';
    return true;
}

// -----------------------------------------------------------------------------
// Pattern scan — duplicated locally per native_menu.cpp's convention.
// -----------------------------------------------------------------------------
struct CompiledPattern {
    std::uint8_t bytes[64];
    std::uint8_t mask[64];
    std::size_t  len = 0;
};

bool compile_pattern(const char* p, CompiledPattern& out) {
    out.len = 0;
    auto hex_nibble = [](char c, int& v) {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        return false;
    };
    while (*p && out.len < sizeof(out.bytes)) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') {
            out.bytes[out.len] = 0;
            out.mask[out.len]  = 0;
            ++out.len; p += 2;
        } else {
            int hi, lo;
            if (!hex_nibble(p[0], hi)) return false;
            if (!p[1] || !hex_nibble(p[1], lo)) return false;
            out.bytes[out.len] = static_cast<std::uint8_t>((hi << 4) | lo);
            out.mask[out.len]  = 1;
            ++out.len; p += 2;
        }
    }
    return out.len > 0;
}

bool get_text_range(std::uintptr_t& start, std::size_t& size) {
    auto base = memory::polaris_base();
    if (!base) return false;
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto first = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = first[i];
        if (std::strncmp(reinterpret_cast<const char*>(s.Name), ".text", 5) == 0) {
            start = base + s.VirtualAddress;
            size  = s.Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

std::uintptr_t scan(const CompiledPattern& pat,
                    std::uintptr_t start, std::size_t size) {
    if (pat.len == 0 || size < pat.len) return 0;
    const auto base = reinterpret_cast<const std::uint8_t*>(start);
    const std::size_t span = size - pat.len + 1;
    for (std::size_t i = 0; i < span; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < pat.len; ++j) {
            if (pat.mask[j] && base[i + j] != pat.bytes[j]) { match = false; break; }
        }
        if (match) return start + i;
    }
    return 0;
}

// -----------------------------------------------------------------------------
// State.
// -----------------------------------------------------------------------------
struct Resolved {
    FindUnrealClassFn find_class       = nullptr;
    void*             bpfl_cdo         = nullptr;  // CDO of UPolarisDialogFunctionLibrary
    void*             ufn_OpenDialog   = nullptr;
    void*             ufn_IsDecided    = nullptr;
    void*             ufn_GetCursor    = nullptr;
    void*             ufn_IsClosed     = nullptr;
    void*             ufn_CloseDialog  = nullptr;
    bool              attempted        = false;
    bool              ok               = false;
};

std::once_flag g_resolve_once;
Resolved       g_resolved;
std::atomic<bool> g_dialog_in_flight{false};

// -----------------------------------------------------------------------------
// Helpers.
// -----------------------------------------------------------------------------

void make_fstring_leaky(UE_FString& out, const char* utf8) {
    // Phase-1: leak. We can't safely free with FMemory::Free until we sig
    // scan GMalloc. UE may copy the string out of the parms blob during
    // ProcessEvent; if it instead retains the pointer, we'd corrupt the
    // heap on free. Leaking a few bytes per OpenDialog is acceptable
    // prototype cost.
    if (!utf8) utf8 = "";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) { len = 1; }
    auto* buf = static_cast<wchar_t*>(std::malloc(sizeof(wchar_t) * static_cast<size_t>(len)));
    if (!buf) { out = {}; return; }
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, len) == 0) {
        buf[0] = L'\0'; len = 1;
    }
    out.data = buf;
    out.num  = len;
    out.max  = len;
}

// Walk a UClass's Children list, return the UFunction whose name decodes
// to `target` (case-sensitive ASCII compare). Uses native_menu's already-
// proven Children-walk pattern (just inlined here).
void* find_ufunction_by_name(void* uclass, const char* target) {
    if (!uclass) return nullptr;
    auto base = reinterpret_cast<std::uintptr_t>(uclass);
    std::uint64_t child_q = 0;
    if (!seh_read_u64(base + USTRUCT_CHILDREN_OFF, &child_q)) return nullptr;
    std::uintptr_t child = static_cast<std::uintptr_t>(child_q);

    char buf[256];
    int hops = 0;
    while (child && hops++ < 256) {
        std::uint32_t idx = 0;
        if (!seh_read_u32(child + UOBJECT_NAME_OFF, &idx)) break;
        if (decode_fname(idx, buf, sizeof(buf))
            && std::strcmp(buf, target) == 0) {
            return reinterpret_cast<void*>(child);
        }
        std::uint64_t nxt = 0;
        if (!seh_read_u64(child + UFIELD_NEXT_OFF, &nxt)) break;
        child = static_cast<std::uintptr_t>(nxt);
    }
    return nullptr;
}

bool resolve_find_class() {
    auto& r = g_resolved;
    std::uintptr_t ts, sz;
    if (!get_text_range(ts, sz)) return false;
    CompiledPattern pat;
    if (!compile_pattern(PAT_FIND_UNREAL_CLASS, pat)) return false;
    auto hit = scan(pat, ts, sz);
    if (!hit) return false;
    // CALL E8 disp32 is at offset +6 from the pattern start. Read disp32
    // at +7, RIP-relative.
    auto disp_at = hit + 7;
    auto disp = static_cast<std::int32_t>(memory::read_u32(disp_at));
    auto fn = disp_at + 4 + static_cast<std::uintptr_t>(static_cast<std::int64_t>(disp));
    r.find_class = reinterpret_cast<FindUnrealClassFn>(fn);
    return true;
}

void do_resolve() {
    auto& r = g_resolved;
    r.attempted = true;

    if (!resolve_find_class()) {
        OPENDOJO_LOG("dialog: findUnrealClass sig-scan failed");
        return;
    }

    // Find the UClass*. native_menu.cpp uses full object paths.
    void* cls = r.find_class(nullptr,
                             L"/Script/Polaris.PolarisDialogFunctionLibrary",
                             /*exact_class=*/true);
    if (!cls) {
        OPENDOJO_LOG("dialog: findUnrealClass returned null for "
                     "PolarisDialogFunctionLibrary");
        return;
    }
    OPENDOJO_LOG("dialog: UClass* = 0x%llX",
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(cls)));

    // CDO: probe known UClass offsets where ClassDefaultObject lives.
    // Strict validation: the candidate pointer must dereference to a
    // vtable pointer, and the vtable's slot 0 (destructor) must point
    // into Polaris's .text section. UClass also contains a raw
    // ClassConstructor function pointer (looked like a vtable on a
    // looser check), so we have to distinguish UObject* from func*.
    {
        std::uintptr_t text_start = 0;
        std::size_t    text_size  = 0;
        if (!get_text_range(text_start, text_size)) {
            OPENDOJO_LOG("dialog: .text range unavailable, can't validate CDO");
            return;
        }
        std::uintptr_t text_end = text_start + text_size;

        auto cls_addr = reinterpret_cast<std::uintptr_t>(cls);
        OPENDOJO_LOG("dialog: probing UClass @ 0x%llX (text 0x%llX-0x%llX)",
                     (unsigned long long)cls_addr,
                     (unsigned long long)text_start,
                     (unsigned long long)text_end);
        // Scan all 8-byte-aligned offsets up to 0x320 — covers any UE5
        // UClass layout drift. Log every non-null candidate's classification.
        for (std::ptrdiff_t off = 0x100; off <= 0x320; off += 8) {
            std::uint64_t maybe_cdo = 0;
            if (!seh_read_u64(cls_addr + off, &maybe_cdo)) continue;
            if (maybe_cdo < 0x10000) continue;

            std::uint64_t vtbl = 0;
            if (!seh_read_u64(static_cast<std::uintptr_t>(maybe_cdo), &vtbl)) continue;
            if (vtbl < 0x10000) continue;

            std::uint64_t fn0 = 0, fn1 = 0;
            bool got0 = seh_read_u64(static_cast<std::uintptr_t>(vtbl), &fn0);
            bool got1 = seh_read_u64(static_cast<std::uintptr_t>(vtbl) + 8, &fn1);
            bool fn0_text = got0 && fn0 >= text_start && fn0 < text_end;
            bool fn1_text = got1 && fn1 >= text_start && fn1 < text_end;

            // Decode the candidate's FName (UObject NamePrivate at +0x18).
            // The real CDO is named "Default__PolarisDialogFunctionLibrary".
            char name_buf[256] = "<?>";
            std::uint32_t name_idx = 0;
            if (seh_read_u32(static_cast<std::uintptr_t>(maybe_cdo)
                             + UOBJECT_NAME_OFF, &name_idx)) {
                decode_fname(name_idx, name_buf, sizeof(name_buf));
            }

            OPENDOJO_LOG("  off 0x%03llX -> 0x%llX  vtbl=0x%llX  "
                         "fn0=0x%llX(%s) fn1=0x%llX(%s)  name='%s'",
                         (unsigned long long)off,
                         (unsigned long long)maybe_cdo,
                         (unsigned long long)vtbl,
                         (unsigned long long)fn0, fn0_text ? "text" : "-",
                         (unsigned long long)fn1, fn1_text ? "text" : "-",
                         name_buf);

            // Accept by name match — CDO of UPolarisDialogFunctionLibrary
            // is conventionally named "Default__PolarisDialogFunctionLibrary".
            if (!r.bpfl_cdo && fn0_text && fn1_text
                && std::strstr(name_buf, "Default__PolarisDialogFunctionLibrary") != nullptr) {
                r.bpfl_cdo = reinterpret_cast<void*>(static_cast<std::uintptr_t>(maybe_cdo));
                OPENDOJO_LOG("    ^^^ accepted as CDO (name match)");
            }
        }
    }
    if (!r.bpfl_cdo) {
        OPENDOJO_LOG("dialog: CDO offset probe failed — no UClass field "
                     "decoded as a valid UObject");
        return;
    }

    r.ufn_OpenDialog   = find_ufunction_by_name(cls, "OpenDialog");
    r.ufn_IsDecided    = find_ufunction_by_name(cls, "IsDialogDecided");
    r.ufn_GetCursor    = find_ufunction_by_name(cls, "GetDialogCursor");
    r.ufn_IsClosed     = find_ufunction_by_name(cls, "IsDialogClosed");
    r.ufn_CloseDialog  = find_ufunction_by_name(cls, "CloseDialog");

    OPENDOJO_LOG("dialog: UFunction OpenDialog=0x%llX IsDecided=0x%llX "
                 "GetCursor=0x%llX IsClosed=0x%llX CloseDialog=0x%llX",
                 (unsigned long long)reinterpret_cast<std::uintptr_t>(r.ufn_OpenDialog),
                 (unsigned long long)reinterpret_cast<std::uintptr_t>(r.ufn_IsDecided),
                 (unsigned long long)reinterpret_cast<std::uintptr_t>(r.ufn_GetCursor),
                 (unsigned long long)reinterpret_cast<std::uintptr_t>(r.ufn_IsClosed),
                 (unsigned long long)reinterpret_cast<std::uintptr_t>(r.ufn_CloseDialog));

    r.ok = r.bpfl_cdo && r.ufn_OpenDialog && r.ufn_IsDecided
        && r.ufn_GetCursor && r.ufn_CloseDialog;
}

ProcessEventFn pe_from_self(void* self) {
    auto vtable = *reinterpret_cast<void***>(self);
    return reinterpret_cast<ProcessEventFn>(vtable[PROCESS_EVENT_VTABLE_SLOT]);
}

void call_pe(void* self, void* function, void* parms) {
    __try {
        auto pe = pe_from_self(self);
        pe(self, function, parms);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("dialog: ProcessEvent SEH — slot %d likely wrong; "
                     "run dump_pe_vtable() and update PROCESS_EVENT_VTABLE_SLOT",
                     PROCESS_EVENT_VTABLE_SLOT);
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// Public API.
// -----------------------------------------------------------------------------

bool ensure_resolved() {
    std::call_once(g_resolve_once, do_resolve);
    return g_resolved.ok;
}

void dump_pe_vtable() {
    if (!ensure_resolved()) {
        OPENDOJO_LOG("dialog: dump_pe_vtable — resolve failed; aborting");
        return;
    }
    auto self = g_resolved.bpfl_cdo;
    auto vtable = *reinterpret_cast<void***>(self);
    auto base = memory::polaris_base();
    OPENDOJO_LOG("dialog: vtable @ 0x%llX  (CDO 0x%llX, module base 0x%llX)",
                 (unsigned long long)reinterpret_cast<std::uintptr_t>(vtable),
                 (unsigned long long)reinterpret_cast<std::uintptr_t>(self),
                 (unsigned long long)base);
    for (int i = 0; i < 100; ++i) {
        auto fn = vtable[i];
        auto addr = reinterpret_cast<std::uintptr_t>(fn);
        // Sanity: if the entry is not in any executable page we likely
        // walked off the end of the vtable. Stop early but keep logging
        // a few past to confirm.
        if (addr == 0) {
            OPENDOJO_LOG("  slot %3d (off 0x%03X): NULL — end-of-vtable",
                         i, i * 8);
            break;
        }
        auto rva = (addr >= base) ? (addr - base) : 0;
        OPENDOJO_LOG("  slot %3d (off 0x%03X): 0x%llX  RVA 0x%llX",
                     i, i * 8,
                     (unsigned long long)addr,
                     (unsigned long long)rva);
    }
}

bool open_test_dialog(const char* description, const char* button_text) {
    if (!ensure_resolved()) {
        OPENDOJO_LOG("dialog: open_test_dialog — not resolved");
        return false;
    }

    // Auto-init the dialog manager if it hasn't been bootstrapped by the
    // game yet. Practice mode doesn't construct this singleton naturally
    // — it's only built when entering Avatar/Lobby/Customize flows.
    if (auto base = memory::polaris_base()) {
        if (!memory::read_u64(base + DIALOG_MGR_SINGLETON_RVA)) {
            OPENDOJO_LOG("dialog: manager singleton null — calling factory");
            force_init_dialog_manager();
        }
    }

    if (g_dialog_in_flight.exchange(true)) {
        OPENDOJO_LOG("dialog: re-entry blocked — previous dialog still open");
        return false;
    }

    // One button. Allocated on the heap (leaked) so the lifetime extends
    // past the OpenDialog call regardless of whether UE copies or retains.
    auto* btn = static_cast<FPolarisDialogButtonParam*>(
        std::calloc(1, sizeof(FPolarisDialogButtonParam)));
    if (!btn) { g_dialog_in_flight = false; return false; }
    make_fstring_leaky(btn->Text, button_text ? button_text : "Close");
    btn->isEnable = true;
    btn->IsTextId = false;
    btn->IsGhost  = false;
    // OnDecide already zeroed by calloc -> unbound delegate.

    OpenDialog_Args args{};
    make_fstring_leaky(args.Description, description ? description : "OpenDojo");
    args.defaultCursor    = 0;
    args.Params.data      = btn;
    args.Params.num       = 1;
    args.Params.max       = 1;
    args.IsTextId         = false;
    args.display_side     = 0;

    OPENDOJO_LOG("dialog: calling OpenDialog via PE slot %d",
                 PROCESS_EVENT_VTABLE_SLOT);
    call_pe(g_resolved.bpfl_cdo, g_resolved.ufn_OpenDialog, &args);
    OPENDOJO_LOG("dialog: OpenDialog returned");
    return true;
}

void log_dialog_manager_state() {
    auto base = memory::polaris_base();
    if (!base) { OPENDOJO_LOG("dialog: no module base"); return; }
    auto singleton = memory::read_u64(base + DIALOG_MGR_SINGLETON_RVA);
    OPENDOJO_LOG("dialog: manager singleton @ Polaris+0x%llX = 0x%llX (%s)",
                 (unsigned long long)DIALOG_MGR_SINGLETON_RVA,
                 (unsigned long long)singleton,
                 singleton ? "POPULATED" : "NULL");
    if (singleton) {
        std::uint64_t vt = 0;
        seh_read_u64(static_cast<std::uintptr_t>(singleton), &vt);
        OPENDOJO_LOG("  manager vtable = 0x%llX", (unsigned long long)vt);
    }
}

void force_init_dialog_manager() {
    auto base = memory::polaris_base();
    if (!base) { OPENDOJO_LOG("dialog: no module base"); return; }
    auto current = memory::read_u64(base + DIALOG_MGR_SINGLETON_RVA);
    if (current) {
        OPENDOJO_LOG("dialog: manager already exists at 0x%llX, skipping",
                     (unsigned long long)current);
        return;
    }
    auto factory = reinterpret_cast<DialogMgrFactoryFn>(
        base + DIALOG_MGR_FACTORY_RVA);
    OPENDOJO_LOG("dialog: calling factory Polaris+0x%llX",
                 (unsigned long long)DIALOG_MGR_FACTORY_RVA);
    void* result = nullptr;
    __try {
        result = factory();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("dialog: factory SEH — game-state precondition not met");
        return;
    }
    auto after = memory::read_u64(base + DIALOG_MGR_SINGLETON_RVA);
    OPENDOJO_LOG("dialog: factory returned 0x%llX, singleton now 0x%llX",
                 (unsigned long long)reinterpret_cast<std::uintptr_t>(result),
                 (unsigned long long)after);
}

void tick() {
    if (!g_dialog_in_flight.load(std::memory_order_relaxed)) return;
    if (!g_resolved.ok || !g_resolved.bpfl_cdo) {
        g_dialog_in_flight = false;
        return;
    }

    IsDialogDecided_Args d{};
    call_pe(g_resolved.bpfl_cdo, g_resolved.ufn_IsDecided, &d);

    IsDialogClosed_Args c{};
    if (g_resolved.ufn_IsClosed) {
        call_pe(g_resolved.bpfl_cdo, g_resolved.ufn_IsClosed, &c);
    }

    if (d.ReturnValue) {
        GetDialogCursor_Args g{};
        call_pe(g_resolved.bpfl_cdo, g_resolved.ufn_GetCursor, &g);
        OPENDOJO_LOG("dialog: DECIDED cursor=%d", g.ReturnValue);
        if (g_resolved.ufn_CloseDialog) {
            call_pe(g_resolved.bpfl_cdo, g_resolved.ufn_CloseDialog, nullptr);
        }
        g_dialog_in_flight = false;
    } else if (c.ReturnValue) {
        OPENDOJO_LOG("dialog: CLOSED via cancel");
        g_dialog_in_flight = false;
    }
}

}  // namespace opendojo::dialog
