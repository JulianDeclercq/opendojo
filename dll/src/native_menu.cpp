#include "native_menu.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "log.hpp"
#include "memory.hpp"

// =============================================================================
// PHASE 0 SCAFFOLDING
// =============================================================================
//
// What's implemented:
//   - Pattern-resolve findUnrealClass at module init (Irony's pattern).
//   - Lazily call findUnrealClass to obtain the UPolarisUMGTextMenu UClass*.
//   - Public show / hide / toggle / tick API that compiles into the DLL
//     but currently no-ops at the widget-construction step.
//
// What's STUBBED with [PHASE 1 TODO] comments:
//   - NewObject equivalent (StaticConstructObject_Internal) — need either
//     a pattern or a different construction path.
//   - UUserWidget vtable slots for Initialize and AddToViewport — need a
//     runtime vtable dump to pin.
//   - Item-population API for UPolarisUMGTextMenu — its delegate sig is
//     known (Selectable/Editing/Clamp) but the exact method that adds a
//     row is unknown. Likely a vtable method or a reflection-exposed
//     UFunction.
//   - UWorld* / APlayerController* resolution. Cheapest: walk our
//     existing player-chain (see players.cpp) — Player struct's outer
//     is the world. We just haven't pinned the offsets yet.
//   - Input focus routing (SetInputMode_UIOnlyEx via UFunction) so the
//     gamepad inputs hit our widget, not the active scene.
//
// Everything in the STUBBED list is a runtime-session task. The design
// doc (NATIVE_MENU_DESIGN.md §5) lists each open question explicitly.

namespace opendojo::native_menu {

namespace {

// -----------------------------------------------------------------------------
// Local pattern-scan helpers. Duplicated (deliberately) from players.cpp
// so we don't disturb a working module. Lift to a shared pattern.hpp
// once a third caller appears.
// -----------------------------------------------------------------------------

struct CompiledPattern {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;
};

bool compile_pattern(const char* p, CompiledPattern& out) {
    out.bytes.clear();
    out.mask.clear();
    auto hex_nibble = [](char c, int& v) {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        return false;
    };
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') {
            out.bytes.push_back(0);
            out.mask.push_back(0);
            p += 2;
        } else {
            int hi, lo;
            if (!hex_nibble(p[0], hi)) return false;
            if (!p[1] || !hex_nibble(p[1], lo)) return false;
            out.bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
            out.mask.push_back(1);
            p += 2;
        }
    }
    return !out.bytes.empty();
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
    if (pat.bytes.empty() || size < pat.bytes.size()) return 0;
    const auto base = reinterpret_cast<const std::uint8_t*>(start);
    const std::size_t span = size - pat.bytes.size() + 1;
    const std::size_t n    = pat.bytes.size();
    std::uintptr_t first_hit = 0;
    int hits = 0;
    for (std::size_t i = 0; i < span; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < n; ++j) {
            if (pat.mask[j] && base[i + j] != pat.bytes[j]) { match = false; break; }
        }
        if (match) {
            if (!first_hit) first_hit = start + i;
            if (++hits >= 2) {
                OPENDOJO_LOG("native_menu: WARNING pattern matched %d+ times "
                             "(first=0x%llX)",
                             hits, static_cast<unsigned long long>(first_hit));
                break;
            }
        }
    }
    return first_hit;
}

std::uintptr_t rip_relative(std::uintptr_t at) {
    auto disp = static_cast<std::int32_t>(memory::read_u32(at));
    return at + 4 + static_cast<std::uintptr_t>(static_cast<std::int64_t>(disp));
}

// -----------------------------------------------------------------------------
// UE5 primitives we resolve from Polaris.
// -----------------------------------------------------------------------------

// findUnrealClass — same pattern Irony uses.
//
// Bytes: 45 33 C0          XOR  R8D, R8D       ; exact_class=false / =0
//        49 8B CF          MOV  RCX, R15       ; outer arg (or other reg)
//        E8 ?? ?? ?? ??    CALL FindObject<UClass>
//        48 8B 4C 24 60    MOV  RCX, [RSP+60]  ; epilogue restore
//
// The CALL's E8 is at offset +6 of the pattern. The 4-byte disp32 starts
// at offset +7. add(+0x7, pattern), then resolve RIP-relative — that
// gives us the absolute address of FindObject<UClass>.
constexpr const char* PAT_FIND_UNREAL_CLASS =
    "45 33 C0 49 8B CF E8 ?? ?? ?? ?? 48 8B 4C 24 60";

// UE5 signature (mirrors Irony's FindUnrealClassFunction).
using FindUnrealClassFn = void* (*)(void* outer,
                                    const wchar_t* name,
                                    bool exact_class);

// findUnrealObjectsOfClass — UE5's GetObjectsOfClass. Returns a
// TArray<UObject*> of all live instances of `class_to_look_for` (and
// derived if include_derived=true). Irony's T8 pattern: the CALL site is
// `E8 ?? ?? ?? ?? 90 48 89 6C 24 30` (call rel32, then nop, then a
// `mov rbp, [rsp+30]` epilogue). +0x1 skips the E8, the next 4 bytes are
// the RIP-relative target.
constexpr const char* PAT_FIND_OBJECTS_OF_CLASS =
    "E8 ?? ?? ?? ?? 90 48 89 6C 24 30";

// UE5 TArray<T> layout — 16 bytes. We pre-allocate one on the stack
// (zero-initialized) and pass its address; UE allocates the inner data
// pointer and fills Num/Max.
struct UE_TArray {
    void* data;
    std::int32_t num;
    std::int32_t max;
};

using FindObjectsOfClassFn = void (*)(void* class_to_look_for,
                                      UE_TArray* out_results,
                                      bool include_derived,
                                      std::uint32_t exclude_object_flags,
                                      std::uint32_t exclude_internal_flags);

// -----------------------------------------------------------------------------
// CreateWidget chain (decoded via Ghidra static analysis — see
// project_tekken_textmenu_api memory note).
//
// CreateWidget(APlayerController* pc, UClass* widget_class, FName name)
//   - Validates the PC has a local player attached.
//   - Forwards to FUN_144841d20 which calls StaticConstructObject_Internal.
//   - Return value semantics: TBD (function appears void; widget may be
//     stored on the PC's widget stack or returned via hidden out-param).
//     First-run experiment will pick this apart.
//
// GUObjectArray globals — for finding existing APlayerController
// instances when we don't already have one in hand. UE5's standard
// FUObjectArray has the pages pointer at +DAT_1499fb530 and count at
// +DAT_1499fb544. Each entry is a 24-byte FUObjectItem.
constexpr std::uintptr_t CREATE_WIDGET_RVA           = 0x4842240;
constexpr std::uintptr_t GUOBJECTARRAY_PAGES_RVA     = 0x99fb530;
constexpr std::uintptr_t GUOBJECTARRAY_NUM_RVA       = 0x99fb544;

// Canonical "open practice menu" sequence (from FUN_145db2a30):
//   1. FUN_144842900(world_or_pc, class, name) → widget
//   2. FUN_14483fe70(widget, mode=0x80) ← AddToViewport with mode/ZOrder
//   3. widget[+0x280] = 0; widget[+0x282] = 1  ← input flags
//   4. FUN_142e90970(widget, 0)  ← SetVisibility
// This is what the game itself uses to display WBP_UI_PracticeMenu_C
// (the OUTER class). Static analysis confirms our previous target
// (WBP_UI_PracticeMenu_Menu_3_C) is an inner sub-widget — composed by
// the outer's WidgetTree, not added directly.
constexpr std::uintptr_t CREATE_WIDGET_V2_RVA        = 0x4842900;
constexpr std::uintptr_t ADD_TO_VIEWPORT_WRAPPER_RVA = 0x483fe70;
constexpr std::uintptr_t SET_VISIBILITY_RVA          = 0x2e90970;

// kamui::ui::PracticeMenuImpl — the C++ manager that owns the practice
// menu widgets and binds their delegates. NOT a UObject; allocated via
// FMemory::Malloc(0x1C0). The singleton pointer is at module+0x9B7BC90.
//
// vtable layout (vtable at 0x14855da58, derived) — slot 1 is the
// "Open Practice Menu" entry point. Calling sequence we'll replicate:
//   manager = *(void**)(module + PRACTICE_MENU_SINGLETON_RVA)
//   if (!manager) manager = factory()
//   open_fn(manager, world_ctx)
//
// `world_ctx` is a "tick frame context" struct — typically derived
// from PC->World per Polaris convention. Inside the open fn it gets
// forwarded to a tick/wait helper, so the world must be live.
constexpr std::uintptr_t PRACTICE_MENU_SINGLETON_RVA = 0x9B7BC90;
constexpr std::uintptr_t PRACTICE_MENU_FACTORY_RVA   = 0x5FD0480;
constexpr std::uintptr_t PRACTICE_MENU_OPEN_RVA      = 0x5DB2A30;

// PracticeMenuImpl field offsets (the manager — not a UObject).
//   +0x1A8: TWeakObjectPtr {int32 ObjectIndex, int32 SerialNumber} to
//           the outer WBP_UI_PracticeMenu_C widget.
//   +0x1B0: same shape, holds the S2 sibling.
//   +0x1B8: byte "manager initialized" flag — set to 1 by the open path.
constexpr std::ptrdiff_t PMI_OUTER_WIDGET_WEAKPTR = 0x1A8;
constexpr std::ptrdiff_t PMI_S2_WIDGET_WEAKPTR    = 0x1B0;
constexpr std::ptrdiff_t PMI_INITIALIZED_BYTE     = 0x1B8;

// TWeakObjectPtr<UObject>::Get — resolves a {idx, serial} pair to the
// live UObject* via GUObjectArray. Returns nullptr if pending-kill or
// serial mismatch. Decoded statically from FUN_1431b1100.
constexpr std::uintptr_t WEAK_OBJ_PTR_GET_RVA = 0x31B1100;

// 10 delegate "binder" slots populated by FUN_145db2a30 onto the outer
// practice-menu widget at +0x290, +0x2D0, ..., +0x4D0. Each is 0x40 bytes:
//   slot+0x00: bound function pointer (one of LAB_145dacXXX)
//   slot+0x20: vtable (= &PTR_FUN_146ec6318, shared by all slots)
//   slot+0x28: receiver (= PracticeMenuImpl* manager)
// The vtable's invoke method calls the fn pointer with the receiver in
// rcx and the event args in rdx/r8 — so overwriting just the fn pointer
// at slot+0x00 is enough to intercept the call.
constexpr std::ptrdiff_t WIDGET_DELEGATE_SLOT_BASE   = 0x290;
constexpr std::ptrdiff_t WIDGET_DELEGATE_SLOT_STRIDE = 0x40;
constexpr int            WIDGET_DELEGATE_SLOT_COUNT  = 10;

// AddToViewport chain (from FUN_14485c790 — the exec stub for
// UPolarisUMGTextMenu::AddToViewport — decompiled separately):
//   - Call widget vtable @ offset 0x188 to get a context (returns a
//     player or world or local-player struct)
//   - Call FUN_14479bb60(context) to get the slate root
//   - Call FUN_144787cc0(slate, widget, slot_params) to attach the widget
constexpr std::uintptr_t GET_SLATE_FROM_PLAYER_RVA   = 0x4479bb60;
constexpr std::uintptr_t ADD_WIDGET_TO_SLATE_RVA     = 0x4787cc0;
constexpr int            WIDGET_GET_PLAYER_VT_BYTE_OFF = 0x188;  // byte offset in vtable

// GEngine global. Used by FUN_1454ee240 (the "get game viewport for
// this world" helper). If null, AddToViewport silently fails with
// "No game viewport was found." in the UE log.
constexpr std::uintptr_t GENGINE_PTR_RVA             = 0x9b4f760;

using CreateWidgetFn = void (*)(void* player_controller,
                                void* widget_class,
                                /*FName*/ std::uint64_t name_packed);

// -----------------------------------------------------------------------------
// Resolution state. All addresses are absolute; resolved once per process.
// -----------------------------------------------------------------------------

// Inner functions called by the AddToViewport exec stub. Signatures
// derived from the exec stub decompile. Kept as raw void(*)() since the
// arg types aren't fully UE5-typed — we cast at the call site.
using GetSlateFromCtxFn = void* (*)(void* world_or_player);
using AddWidgetToSlateFn = void (*)(void* slate, void* widget, void* slot_params);

struct Resolved {
    FindUnrealClassFn      find_class             = nullptr;
    FindObjectsOfClassFn   find_objects_of_class  = nullptr;
    CreateWidgetFn         create_widget          = nullptr;
    GetSlateFromCtxFn      get_slate_from_player  = nullptr;
    AddWidgetToSlateFn     add_widget_to_slate    = nullptr;
    void*                  text_menu_cls          = nullptr;
    void*                  dialog_cls             = nullptr;
    // Cache the constructed widget so we don't allocate one per F11.
    void*                  cached_widget          = nullptr;
    // GUObjectArray accessors — kept for diagnostics; use
    // find_objects_of_class for live enumeration.
    void**                 guobj_pages            = nullptr;
    std::int32_t*          guobj_count            = nullptr;
    bool                   attempted              = false;
    bool                   ok                     = false;
};

std::once_flag g_resolve_once;
Resolved       g_resolved;

// SEH-protected qword read. Returns true on success. Used for the
// Children walk where a stale pointer could land on an unmapped page —
// the previous walk crashed Tekken at launch. Plain C body (no C++
// destructors) so __try / __except is legal here.
bool seh_read_u64(std::uintptr_t addr, std::uint64_t* out) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), sizeof(*out));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_read_u32(std::uintptr_t addr, std::uint32_t* out) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), sizeof(*out));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_read_u16(std::uintptr_t addr, std::uint16_t* out) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), sizeof(*out));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_read_u8(std::uintptr_t addr, std::uint8_t* out) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), sizeof(*out));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// FNamePool global. Decoded statically from FUN_142fee6b0 / FUN_142ff82d0:
//   - Pool base at module+POOL_RVA
//   - Pool layout: blocks array at pool+0x10 (each entry = 8-byte block pointer)
//   - FName.idx layout: bits 16..28 (13 bits) = block index,
//                       bits 0..15  (16 bits) = stride (entry byte_off / 2)
//   - Entry layout: 2-byte header, then string data
//   - Header encoding: bit 0 = bIsWide, bits 6..15 = length in characters
constexpr std::uintptr_t FNAME_POOL_RVA      = 0x9955480;
constexpr std::ptrdiff_t POOL_BLOCKS_OFFSET  = 0x10;
constexpr std::uint32_t  FNAME_BLOCK_MASK    = 0x1FFF;   // 13 bits
constexpr std::uint32_t  FNAME_STRIDE_MASK   = 0xFFFF;   // 16 bits
constexpr std::uint32_t  FNAME_BLOCK_SHIFT   = 16;

// Decode an FName index to its string name. Writes a null-terminated
// ASCII string to `out_buf`. Wide-character names are lossy-converted to
// ASCII ('?' for non-ASCII chars). Returns true on success.
bool decode_fname(std::uint32_t idx, char* out_buf, std::size_t out_buf_size) {
    if (!out_buf || out_buf_size == 0) return false;
    out_buf[0] = '\0';
    if (idx == 0) {
        // FName 0 is conventionally "None" by UE5 convention.
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
        || !block_ptr) {
        std::snprintf(out_buf, out_buf_size, "<bad-block-%u>", block_idx);
        return false;
    }

    std::uintptr_t entry = block_ptr + static_cast<std::uintptr_t>(stride) * 2;
    std::uint16_t  header = 0;
    if (!seh_read_u16(entry, &header)) {
        std::snprintf(out_buf, out_buf_size, "<bad-entry-%u>", idx);
        return false;
    }

    bool          is_wide   = (header & 1) != 0;
    std::uint32_t len_chars = header >> 6;
    if (len_chars == 0 || len_chars > 1023) {
        std::snprintf(out_buf, out_buf_size, "<bad-len-%u-idx-%u>", len_chars, idx);
        return false;
    }
    if (len_chars >= out_buf_size) len_chars = static_cast<std::uint32_t>(out_buf_size) - 1;

    if (!is_wide) {
        for (std::uint32_t i = 0; i < len_chars; ++i) {
            std::uint8_t c = 0;
            if (!seh_read_u8(entry + 2 + i, &c)) {
                out_buf[i] = '\0';
                return false;
            }
            out_buf[i] = static_cast<char>(c);
        }
    } else {
        for (std::uint32_t i = 0; i < len_chars; ++i) {
            std::uint16_t wc = 0;
            if (!seh_read_u16(entry + 2 + i * 2, &wc)) {
                out_buf[i] = '\0';
                return false;
            }
            out_buf[i] = (wc < 0x80) ? static_cast<char>(wc) : '?';
        }
    }
    out_buf[len_chars] = '\0';
    return true;
}

// UClass / UFunction / FField layout for UE 5.2 (verified empirically):
//   UClass / UStruct (UObject-derived):
//     +0x18: NamePrivate (FName: 2x u32)
//     +0x20: OuterPrivate (UObject*)
//     +0x28: UField::Next (UField*)
//     +0x40: SuperStruct
//     +0x48: Children (UField*) — head of UFunctions for a UClass
//     +0x50: ChildProperties (FField*) — head of FProperties (NOT used here;
//             on this build, UClass's properties live elsewhere — its
//             ChildProperties were null in the earlier dump)
//
//   UFunction (UStruct + a tail of UFunction-specific fields, 0xE0 bytes):
//     +0x50: ChildProperties (FField*) — head of FProperty list (args + locals)
//     +0xD8: Func (native handler pointer)
//
//   FField:
//     +0x00: vtable
//     +0x08: ClassPrivate (FFieldClass*) — the *type* of this field
//     +0x20: Next (FField*) — sibling in the linked list
//     +0x28: NamePrivate (FName)
//
//   FProperty (extends FField, +0x38..):
//     +0x38: ArrayDim (int32)
//     +0x3C: ElementSize (int32)
//     +0x40: PropertyFlags (uint64)
//     +0x4C: Offset_Internal (int32)
constexpr std::ptrdiff_t UCLASS_CHILDREN_OFF        = 0x48;
constexpr std::ptrdiff_t UFIELD_NEXT_OFF            = 0x28;   // UField::Next on UObject path
constexpr std::ptrdiff_t UOBJECT_NAME_OFF           = 0x18;
constexpr std::ptrdiff_t UFUNCTION_CHILDPROPS_OFF   = 0x50;
constexpr std::ptrdiff_t UFUNCTION_FUNC_OFF         = 0xD8;
constexpr std::ptrdiff_t FFIELD_CLASS_OFF           = 0x08;
constexpr std::ptrdiff_t FFIELD_NEXT_OFF            = 0x20;
constexpr std::ptrdiff_t FFIELD_NAME_OFF            = 0x28;
constexpr std::ptrdiff_t FPROPERTY_ELEMENT_SIZE_OFF = 0x3C;
constexpr std::ptrdiff_t FPROPERTY_FLAGS_OFF        = 0x40;
constexpr std::ptrdiff_t FPROPERTY_OFFSET_INT_OFF   = 0x4C;

// Walk one UFunction's ChildProperties list. Logs each FProperty's
// position in the stack frame, name (FName indices), type (FFieldClass
// pointer — same address means same UE type, e.g. FBoolProperty), and
// size. SEH-guarded throughout.
void walk_function_properties(std::uintptr_t func) {
    std::uint64_t prop = 0;
    if (!seh_read_u64(func + UFUNCTION_CHILDPROPS_OFF, &prop)) return;
    if (!prop) return;
    OPENDOJO_LOG("         params (offset : size : type-class : FName):");
    constexpr int MAX_PROPS = 16;
    int p = 0;
    while (prop && p < MAX_PROPS) {
        std::uint64_t fclass = 0;
        std::uint64_t next   = 0;
        std::uint32_t pidx   = 0;
        std::uint32_t pnum   = 0;
        std::uint32_t poff   = 0;
        std::uint32_t psize  = 0;
        std::uint64_t pflags = 0;
        if (!seh_read_u64(prop + FFIELD_CLASS_OFF,           &fclass)) break;
        if (!seh_read_u64(prop + FFIELD_NEXT_OFF,            &next))   next = 0;
        if (!seh_read_u32(prop + FFIELD_NAME_OFF,            &pidx))   break;
        if (!seh_read_u32(prop + FFIELD_NAME_OFF + 4,        &pnum))   break;
        if (!seh_read_u32(prop + FPROPERTY_OFFSET_INT_OFF,   &poff))   poff = 0;
        if (!seh_read_u32(prop + FPROPERTY_ELEMENT_SIZE_OFF, &psize))  psize = 0;
        if (!seh_read_u64(prop + FPROPERTY_FLAGS_OFF,        &pflags)) pflags = 0;
        OPENDOJO_LOG("           [%d] @0x%llX  +0x%X  size=%u  flags=0x%llX  "
                     "type=0x%llX  FName(%u,%u)",
                     p + 1,
                     static_cast<unsigned long long>(prop),
                     poff, psize,
                     static_cast<unsigned long long>(pflags),
                     static_cast<unsigned long long>(fclass),
                     pidx, pnum);
        prop = next;
        ++p;
    }
}

// True if `cls` equals `target` or any of its SuperStructs (recursive walk
// via UStruct::SuperStruct at +0x40). SEH-guarded — class chains are
// supposed to be intact but we're paranoid since this scans the entire
// GUObjectArray and any one stale chain would crash an unguarded walk.
bool is_class_derived_from(std::uintptr_t cls, std::uintptr_t target) {
    constexpr int MAX_DEPTH = 32;
    for (int d = 0; d < MAX_DEPTH && cls; ++d) {
        if (cls == target) return true;
        std::uint64_t super = 0;
        if (!seh_read_u64(cls + 0x40, &super)) return false;
        cls = super;
    }
    return false;
}

// Walk GUObjectArray and return the first object whose class derives from
// `target_class`. Skips null slots and class-chain SEH faults. Logs and
// returns null if nothing's found. Cost: ~few hundred microseconds for
// 62k objects worst case; called once.
//
// FUObjectItem (UE5 layout):
//   +0x00: UObject* Object
//   +0x08: int32 Flags
//   +0x0C: int32 ClusterRootIndex
//   +0x10: int32 SerialNumber
//   total 0x18 = 24 bytes
//
// pages[] is a sparse array: page index = i >> 16, slot = i & 0xFFFF.
// Each page is a contiguous block of (typically 65536) FUObjectItem.
constexpr std::ptrdiff_t FUOBJ_ITEM_SIZE   = 0x18;
constexpr std::ptrdiff_t FUOBJ_OBJECT_OFF  = 0x00;
constexpr std::ptrdiff_t UOBJECT_CLASS_OFF = 0x10;
constexpr int            GUOBJ_PAGE_SHIFT  = 16;
constexpr int            GUOBJ_PAGE_MASK   = 0xFFFF;

void* find_first_object_of_class(std::uintptr_t target_class) {
    if (!g_resolved.guobj_pages || !g_resolved.guobj_count) return nullptr;
    std::int32_t count = 0;
    if (!seh_read_u32(reinterpret_cast<std::uintptr_t>(g_resolved.guobj_count),
                      reinterpret_cast<std::uint32_t*>(&count))) return nullptr;
    if (count <= 0 || count > 10'000'000) return nullptr;

    for (std::int32_t i = 0; i < count; ++i) {
        int page_idx = i >> GUOBJ_PAGE_SHIFT;
        int slot     = i & GUOBJ_PAGE_MASK;
        std::uint64_t page_ptr = 0;
        auto pages_addr = reinterpret_cast<std::uintptr_t>(g_resolved.guobj_pages)
                        + page_idx * sizeof(void*);
        if (!seh_read_u64(pages_addr, &page_ptr) || !page_ptr) continue;

        auto item_addr = page_ptr + slot * FUOBJ_ITEM_SIZE;
        std::uint64_t obj = 0;
        if (!seh_read_u64(item_addr + FUOBJ_OBJECT_OFF, &obj) || !obj) continue;

        std::uint64_t obj_class = 0;
        if (!seh_read_u64(obj + UOBJECT_CLASS_OFF, &obj_class) || !obj_class) continue;

        if (is_class_derived_from(obj_class, target_class)) {
            return reinterpret_cast<void*>(obj);
        }
    }
    return nullptr;
}

void introspect_uclass(const char* label, std::uintptr_t cls) {
    if (!cls) return;
    OPENDOJO_LOG("native_menu: introspect %s @ 0x%llX",
                 label, static_cast<unsigned long long>(cls));

    auto name_idx = memory::read_u32(cls + 0x18);
    auto name_num = memory::read_u32(cls + 0x1C);
    auto outer    = memory::read_u64(cls + 0x20);
    auto super    = memory::read_u64(cls + 0x40);
    OPENDOJO_LOG("  FName(idx=%u num=%u) outer=0x%llX super=0x%llX",
                 name_idx, name_num,
                 static_cast<unsigned long long>(outer),
                 static_cast<unsigned long long>(super));

    auto child = memory::read_u64(cls + UCLASS_CHILDREN_OFF);
    if (!child) {
        OPENDOJO_LOG("  Children (+0x48) = null");
        return;
    }
    OPENDOJO_LOG("  Children chain:");

    constexpr int MAX_WALK = 80;
    int n = 0;
    while (child && n < MAX_WALK) {
        std::uint32_t cidx = 0;
        std::uint32_t cnum = 0;
        std::uint64_t next = 0;
        std::uint64_t func = 0;

        if (!seh_read_u32(child + UOBJECT_NAME_OFF,     &cidx)) { OPENDOJO_LOG("    [%2d] 0x%llX  SEH @ FName.idx — abort", n + 1, child); break; }
        if (!seh_read_u32(child + UOBJECT_NAME_OFF + 4, &cnum)) { OPENDOJO_LOG("    [%2d] 0x%llX  SEH @ FName.num — abort", n + 1, child); break; }
        if (!seh_read_u64(child + UFIELD_NEXT_OFF,      &next)) next = 0;
        if (!seh_read_u64(child + UFUNCTION_FUNC_OFF,   &func)) func = 0;

        OPENDOJO_LOG("    [%2d] UFunction @0x%llX  FName(%u,%u)  Func=0x%llX",
                     n + 1,
                     static_cast<unsigned long long>(child),
                     cidx, cnum,
                     static_cast<unsigned long long>(func));

        // Properties (args + locals) for this UFunction.
        walk_function_properties(child);

        child = next;
        ++n;
    }
    OPENDOJO_LOG("  Children chain ended after %d entries", n);
}

void do_resolve() {
    g_resolved.attempted = true;

    std::uintptr_t text_start = 0;
    std::size_t    text_size  = 0;
    if (!get_text_range(text_start, text_size)) {
        OPENDOJO_LOG("native_menu: couldn't locate Polaris .text — abort");
        return;
    }

    CompiledPattern pat;
    if (!compile_pattern(PAT_FIND_UNREAL_CLASS, pat)) {
        OPENDOJO_LOG("native_menu: pattern compile failure (findUnrealClass)");
        return;
    }

    auto hit = scan(pat, text_start, text_size);
    if (!hit) {
        OPENDOJO_LOG("native_menu: PAT_FIND_UNREAL_CLASS miss — "
                     "game version may have shifted");
        return;
    }

    // +0x7 lands on the disp32 of the E8 CALL. rip_relative reads the
    // 4 bytes there and adds (instruction-end + disp) to get the target.
    auto fn_addr = rip_relative(hit + 7);
    g_resolved.find_class = reinterpret_cast<FindUnrealClassFn>(fn_addr);

    // findUnrealObjectsOfClass via Irony's T8 pattern. +1 to skip the
    // E8 opcode, resolve disp32 → target.
    CompiledPattern pat_objs;
    if (compile_pattern(PAT_FIND_OBJECTS_OF_CLASS, pat_objs)) {
        if (auto hit_objs = scan(pat_objs, text_start, text_size)) {
            auto objs_fn = rip_relative(hit_objs + 1);
            g_resolved.find_objects_of_class =
                reinterpret_cast<FindObjectsOfClassFn>(objs_fn);
            OPENDOJO_LOG("native_menu: find_objects_of_class=0x%llX",
                         static_cast<unsigned long long>(objs_fn));
        } else {
            OPENDOJO_LOG("native_menu: PAT_FIND_OBJECTS_OF_CLASS miss");
        }
    }

    // Resolve UPolarisUMGTextMenu's UClass*. UE5 FindObject takes the
    // full object path; for engine-registered Polaris classes this is
    // /Script/Polaris.<ClassName> without the leading 'U' or 'A'.
    void* cls = g_resolved.find_class(nullptr,
                                     L"/Script/Polaris.PolarisUMGTextMenu",
                                     /*exact_class=*/true);
    if (!cls) {
        OPENDOJO_LOG("native_menu: FindObject(/Script/Polaris.PolarisUMGTextMenu) "
                     "returned null — class name may have changed");
        return;
    }
    g_resolved.text_menu_cls = cls;
    g_resolved.ok = true;

    OPENDOJO_LOG("native_menu: resolved find_class=0x%llX text_menu_cls=0x%llX",
                 static_cast<unsigned long long>(fn_addr),
                 reinterpret_cast<unsigned long long>(cls));

    // Pin the remaining Phase 1 primitives by hardcoded Polaris RVA.
    auto base = memory::polaris_base();
    if (base) {
        g_resolved.create_widget =
            reinterpret_cast<CreateWidgetFn>(base + CREATE_WIDGET_RVA);
        g_resolved.get_slate_from_player =
            reinterpret_cast<GetSlateFromCtxFn>(base + GET_SLATE_FROM_PLAYER_RVA);
        g_resolved.add_widget_to_slate =
            reinterpret_cast<AddWidgetToSlateFn>(base + ADD_WIDGET_TO_SLATE_RVA);
        g_resolved.guobj_pages =
            reinterpret_cast<void**>(base + GUOBJECTARRAY_PAGES_RVA);
        g_resolved.guobj_count =
            reinterpret_cast<std::int32_t*>(base + GUOBJECTARRAY_NUM_RVA);
        OPENDOJO_LOG("native_menu: create_widget=0x%llX get_slate=0x%llX "
                     "add_to_slate=0x%llX (current obj count=%d)",
                     reinterpret_cast<unsigned long long>(g_resolved.create_widget),
                     reinterpret_cast<unsigned long long>(g_resolved.get_slate_from_player),
                     reinterpret_cast<unsigned long long>(g_resolved.add_widget_to_slate),
                     g_resolved.guobj_count ? *g_resolved.guobj_count : -1);
    }

    // Dump the class shape immediately. Safe — only memory reads. Output
    // lands in opendojo.log; we cross-reference FName indices offline.
    // Also dump UPolarisUMGDialog as a simpler fallback widget candidate
    // (Phase 1's "hello world" target — modal dialog is much simpler than
    // a multi-row selectable list).
    introspect_uclass("UPolarisUMGTextMenu", reinterpret_cast<std::uintptr_t>(cls));

    void* dialog_cls = g_resolved.find_class(nullptr,
                                             L"/Script/Polaris.PolarisUMGDialog",
                                             /*exact_class=*/true);
    if (dialog_cls) {
        g_resolved.dialog_cls = dialog_cls;
        introspect_uclass("UPolarisUMGDialog",
                          reinterpret_cast<std::uintptr_t>(dialog_cls));
    } else {
        OPENDOJO_LOG("native_menu: FindObject(/Script/Polaris.PolarisUMGDialog) returned null");
    }

    // FName decoder self-test. Decode well-known FNames against expected
    // strings — sanity-check our pool layout before relying on it elsewhere.
    {
        char buf[128] = {};
        // The class for text_menu_cls was looked up by path
        // "/Script/Polaris.PolarisUMGTextMenu", so cls.NamePrivate.idx
        // should decode to the leaf name "PolarisUMGTextMenu".
        std::uint32_t leaf_idx = 0;
        seh_read_u32(reinterpret_cast<std::uintptr_t>(cls) + 0x18, &leaf_idx);
        decode_fname(leaf_idx, buf, sizeof(buf));
        OPENDOJO_LOG("native_menu: FName self-test — text_menu_cls FName=%u "
                     "decoded='%s' (expect 'PolarisUMGTextMenu')",
                     leaf_idx, buf);
        // FName 0 is conventionally "None".
        decode_fname(0, buf, sizeof(buf));
        OPENDOJO_LOG("native_menu: FName self-test — idx=0 decoded='%s' "
                     "(expect 'None')", buf);
        // FName 999 is a low-index name. Just dump it for visual inspection.
        decode_fname(999, buf, sizeof(buf));
        OPENDOJO_LOG("native_menu: FName self-test — idx=999 decoded='%s'", buf);
    }

    // PC search lives in show() now — at engine init most scenes have no
    // PlayerController yet. show() retries each time F11 is pressed.
}

// Sanity-check the GUObjectArray walk by hex-dumping the first chunk of
// page 0. We're looking for the actual FUObjectItem layout — the sample
// class pointer was UTF-16 string data last run, so our offset
// assumptions are off. Print the first 12 qwords (96 bytes = 4 items if
// 24-byte stride, or 3 items if 32-byte stride) so we can identify
// where Object* and Flags actually live.
int sanity_scan_objects() {
    if (!g_resolved.guobj_pages || !g_resolved.guobj_count) return -1;
    std::int32_t count = *g_resolved.guobj_count;
    if (count <= 0 || count > 10'000'000) return -2;

    OPENDOJO_LOG("native_menu: sanity scan — total count=%d", count);

    // Hex-dump the pages array header (a few qwords) — confirms each
    // entry is a heap pointer.
    OPENDOJO_LOG("native_menu: pages-array head (3 qwords):");
    for (int i = 0; i < 3; ++i) {
        std::uint64_t v = 0;
        auto addr = reinterpret_cast<std::uintptr_t>(g_resolved.guobj_pages) + i * 8;
        if (seh_read_u64(addr, &v)) {
            OPENDOJO_LOG("    +0x%02X: 0x%016llX  (page %d ptr)",
                         i * 8, static_cast<unsigned long long>(v), i);
        }
    }

    // Read page 0 and dump its first 96 bytes (12 qwords).
    std::uint64_t page0 = 0;
    if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(g_resolved.guobj_pages),
                      &page0) || !page0) {
        OPENDOJO_LOG("native_menu: page 0 ptr is null");
        return 0;
    }
    OPENDOJO_LOG("native_menu: page 0 first 12 qwords:");
    for (int i = 0; i < 12; ++i) {
        std::uint64_t v = 0;
        if (seh_read_u64(page0 + i * 8, &v)) {
            OPENDOJO_LOG("    +0x%02X: 0x%016llX",
                         i * 8, static_cast<unsigned long long>(v));
        }
    }

    // Also probe a few specific offsets within page 0 looking for heap-y
    // values (0x000001..., 0x000002... range based on this build's
    // allocations). If the Object pointer is at item-offset 0x08
    // (not 0x00), we'd expect a heap address at qword indices 1, 4, 7, ...
    OPENDOJO_LOG("native_menu: candidate stride patterns:");
    constexpr int probe_offsets[] = { 0x00, 0x08, 0x10 };
    for (int item = 0; item < 4; ++item) {
        for (int base_off : probe_offsets) {
            std::uint64_t v = 0;
            auto addr = page0 + item * FUOBJ_ITEM_SIZE + base_off;
            if (seh_read_u64(addr, &v)) {
                // Tag if value looks like a heap pointer for this build.
                const bool heap = (v >= 0x100000000ULL && v < 0x100000000000ULL);
                OPENDOJO_LOG("    item[%d] +0x%02X: 0x%016llX %s",
                             item, base_off,
                             static_cast<unsigned long long>(v),
                             heap ? "[heap-shaped]" : "");
            }
        }
    }
    return 0;
}

// Try a single FindObject<UClass> lookup with explicit logging.
void* lookup_class_logged(const wchar_t* path) {
    auto* cls = g_resolved.find_class(nullptr, path, true);
    OPENDOJO_LOG("native_menu:   find_class(%ls) -> 0x%llX",
                 path, reinterpret_cast<unsigned long long>(cls));
    return cls;
}

// Returns the first APlayerController-derived UObject in the live
// scene. Uses UE5's GetObjectsOfClass via the pinned find_objects_of_class
// function — it correctly handles GUObjectArray's chunked layout
// internally, which our manual walk couldn't get right.
void* find_player_controller_now() {
    if (!g_resolved.ok) return nullptr;
    if (!g_resolved.find_objects_of_class) {
        OPENDOJO_LOG("native_menu: find_objects_of_class not resolved");
        return nullptr;
    }

    static const wchar_t* CANDIDATES[] = {
        L"/Script/Polaris.PolarisBattlePlayerController",
        L"/Script/Polaris.PolarisDebugBattlePlayerController",
        L"/Script/Polaris.PolarisBasePlayerController",
        L"/Script/Engine.PlayerController",
    };

    OPENDOJO_LOG("native_menu: searching for PlayerController via GetObjectsOfClass");
    for (auto* path : CANDIDATES) {
        auto* cls = lookup_class_logged(path);
        if (!cls) continue;

        UE_TArray results{};
        // exclude_object_flags=0x30 → skip ClassDefaultObject (0x10) and
        // ArchetypeObject (0x20). Those are template instances, not real
        // player controllers. include_derived=true catches Polaris
        // subclasses for the Engine base.
        constexpr std::uint32_t EXCLUDE_CDO_ARCHETYPE = 0x30;
        __try {
            g_resolved.find_objects_of_class(cls, &results,
                                             /*include_derived=*/true,
                                             EXCLUDE_CDO_ARCHETYPE,
                                             /*exclude_internal_flags=*/0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            OPENDOJO_LOG("native_menu:     SEH inside find_objects_of_class");
            continue;
        }
        OPENDOJO_LOG("native_menu:     -> %d instances (data=0x%llX)",
                     results.num,
                     reinterpret_cast<unsigned long long>(results.data));
        if (results.num <= 0 || !results.data) continue;

        // Iterate all instances; pick the first one that looks like a
        // real local PC. CreateWidget validates two things before
        // constructing:
        //   byte at +0x328 (PC[0x65] as longlong-array): bit 2 set => local
        //   ptr  at +0x338 (PC[0x67]):                    Player must be non-null
        // We test both so we don't pick a CDO that slipped through, or
        // a remote PC that would fail validation.
        auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
        for (std::int32_t i = 0; i < results.num; ++i) {
            std::uint64_t inst = 0;
            if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &inst)
                || !inst) continue;

            std::uint32_t flags328 = 0;
            std::uint64_t player338 = 0;
            seh_read_u32(inst + 0x328, &flags328);
            seh_read_u64(inst + 0x338, &player338);

            // ObjectFlags at +0x08 (first qword of UObject header).
            std::uint32_t obj_flags = 0;
            seh_read_u32(inst + 0x08, &obj_flags);

            std::uint64_t inst_class = 0;
            seh_read_u64(inst + UOBJECT_CLASS_OFF, &inst_class);
            std::uint32_t cls_idx = 0;
            if (inst_class) seh_read_u32(inst_class + 0x18, &cls_idx);

            const bool is_local = (flags328 & 0x02) != 0;
            const bool has_player = (player338 != 0);
            OPENDOJO_LOG("native_menu:     [%d] PC @0x%llX  obj_flags=0x%X  "
                         "+0x328=0x%X (local=%d)  +0x338=0x%llX (player=%d)  "
                         "class-FName=%u",
                         i,
                         static_cast<unsigned long long>(inst),
                         obj_flags, flags328, is_local ? 1 : 0,
                         static_cast<unsigned long long>(player338),
                         has_player ? 1 : 0,
                         cls_idx);

            if (is_local && has_player) {
                OPENDOJO_LOG("native_menu:     -> picked PC [%d] as local-with-player", i);
                return reinterpret_cast<void*>(inst);
            }
        }
        OPENDOJO_LOG("native_menu:     -> no instance passed local+player validation");
    }
    return nullptr;
}

// Sacrificial CreateWidget invocation. SEH-guarded — if our call
// convention or argument assumptions are wrong, we log instead of
// crashing. Returns whatever was in RAX (the C++ caller-side return
// register). The actual function decompiled as void; we use the
// __try/__except wrapper here strictly to neutralize any UE-side
// access violations.
//
// Pre-call state captured before the call so we can correlate later:
// - GUObjectArray count (does it increase by 1?)
// - PC's last widget slot (per UE5 layout, PC has a TArray of widgets)
//
// Post-call signals to inspect:
// - The return value (RAX): if non-null and looks heap-y, that's the widget.
// - GUObjectArray count++: a new UObject was created.
// - opendojo.log for the line below — confirms the function returned at all.
void* try_create_widget(void* pc, void* widget_cls) {
    if (!pc || !widget_cls || !g_resolved.create_widget) return nullptr;
    std::int32_t before_count = g_resolved.guobj_count ? *g_resolved.guobj_count : -1;
    void* result = nullptr;
    __try {
        // CreateWidget signature on Polaris: (PC*, UClass*, FName).
        // FName(0, 0) means "auto-generate a name". We pass it as a
        // packed 64-bit zero (FName is just two uint32 fields).
        using Fn = void* (*)(void*, void*, std::uint64_t);
        auto fn = reinterpret_cast<Fn>(g_resolved.create_widget);
        result = fn(pc, widget_cls, 0ULL);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("native_menu: SEH during CreateWidget call");
        return nullptr;
    }
    std::int32_t after_count = g_resolved.guobj_count ? *g_resolved.guobj_count : -1;
    OPENDOJO_LOG("native_menu: CreateWidget returned 0x%llX  (GUObjectArray count "
                 "%d -> %d, delta=%d)",
                 reinterpret_cast<unsigned long long>(result),
                 before_count, after_count, after_count - before_count);
    return result;
}

// -----------------------------------------------------------------------------
// Visibility state. Phase 0: just a bool. Phase 1: real widget construction.
// -----------------------------------------------------------------------------

std::atomic<bool> g_visible{false};
bool              g_logged_phase1_warning = false;

void log_phase1_warning_once() {
    if (g_logged_phase1_warning) return;
    g_logged_phase1_warning = true;
    OPENDOJO_LOG("native_menu: show() requested — Phase 0 stub, widget "
                 "construction not yet implemented. See NATIVE_MENU_DESIGN.md §5.");
}

// -----------------------------------------------------------------------------
// Hotkey polling. F11 toggles native menu. Doesn't conflict with the
// existing F12 ImGui toggle in render_hook. Both menus coexist until
// Phase 3 of the design doc removes ImGui.
// -----------------------------------------------------------------------------

constexpr int HOTKEY_VK = VK_F11;
bool g_last_hotkey_state = false;

// F10 is independent of the F11 probe cycle and drives the widget-tree
// dump (Phase A of the "modify menu directly" path). One press = one full
// walk of the practice menu's subtree.
constexpr int HOTKEY_TREE_VK = VK_F10;
bool g_last_tree_hotkey_state = false;

// F9 toggles the row-label patch: first press swaps "Help" -> "OpenDojo"
// at every matching widget+0x198 / +0x1C0; second press restores.
constexpr int HOTKEY_PATCH_VK = VK_F9;
bool g_last_patch_hotkey_state = false;
bool g_label_patched           = false;

}  // anonymous namespace

// =============================================================================
// Public API
// =============================================================================

bool ensure_resolved() {
    std::call_once(g_resolve_once, do_resolve);
    return g_resolved.ok;
}

bool is_visible() {
    return g_visible.load(std::memory_order_relaxed);
}

// Same logical chain as before but each Polaris-side call wrapped in its
// own SEH block so we can localize which step actually faults.
//
// SEH note: __try/__except can't share scope with C++ objects that
// require unwinding, so each step is a small helper with no locals
// other than primitives.

void* call_vtable_at_off(void* widget, std::ptrdiff_t byte_off,
                         const char* tag) {
    std::uint64_t vt = 0;
    if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(widget), &vt) || !vt) {
        OPENDOJO_LOG("native_menu: %s — widget vtable unreadable", tag);
        return nullptr;
    }
    std::uint64_t fn = 0;
    if (!seh_read_u64(vt + byte_off, &fn) || !fn) {
        OPENDOJO_LOG("native_menu: %s — vtable[+0x%X] unreadable/null",
                     tag, static_cast<unsigned>(byte_off));
        return nullptr;
    }
    OPENDOJO_LOG("native_menu: %s — vtable=0x%llX vtable[+0x%X]=0x%llX",
                 tag, static_cast<unsigned long long>(vt),
                 static_cast<unsigned>(byte_off),
                 static_cast<unsigned long long>(fn));
    void* result = nullptr;
    __try {
        using Fn = void* (*)(void*);
        result = reinterpret_cast<Fn>(fn)(widget);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("native_menu: %s — SEH inside vtable call", tag);
        return nullptr;
    }
    OPENDOJO_LOG("native_menu: %s — returned 0x%llX", tag,
                 reinterpret_cast<unsigned long long>(result));
    return result;
}

void* call_get_slate(void* ctx) {
    void* result = nullptr;
    __try {
        result = g_resolved.get_slate_from_player(ctx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("native_menu: SEH in get_slate(ctx=0x%llX)",
                     reinterpret_cast<unsigned long long>(ctx));
        return nullptr;
    }
    OPENDOJO_LOG("native_menu: get_slate(ctx=0x%llX) returned 0x%llX",
                 reinterpret_cast<unsigned long long>(ctx),
                 reinterpret_cast<unsigned long long>(result));
    return result;
}

void call_add_widget(void* slate, void* widget, void* slot_params) {
    __try {
        g_resolved.add_widget_to_slate(slate, widget, slot_params);
        OPENDOJO_LOG("native_menu: add_widget_to_slate completed");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("native_menu: SEH in add_widget_to_slate(slate=0x%llX widget=0x%llX)",
                     reinterpret_cast<unsigned long long>(slate),
                     reinterpret_cast<unsigned long long>(widget));
    }
}

// Locate the live UGameViewportSubsystem instance. Ghidra confirms it's
// what holds the slate-attach machinery — FUN_14479bb60 (which crashed
// for us) is the lazy getter for it, and we can short-circuit by
// looking the instance up directly via GetObjectsOfClass.
void* find_game_viewport_subsystem() {
    if (!g_resolved.find_objects_of_class) return nullptr;
    auto* cls = g_resolved.find_class(
        nullptr, L"/Script/UMG.GameViewportSubsystem", true);
    OPENDOJO_LOG("native_menu: GVS class lookup -> 0x%llX",
                 reinterpret_cast<unsigned long long>(cls));
    if (!cls) return nullptr;

    UE_TArray results{};
    __try {
        g_resolved.find_objects_of_class(cls, &results, /*include_derived=*/true,
                                         /*exclude_object_flags=*/0x30,   // skip CDO/Archetype
                                         /*exclude_internal_flags=*/0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("native_menu: SEH inside find_objects_of_class(GVS)");
        return nullptr;
    }
    OPENDOJO_LOG("native_menu: GVS -> %d instances (data=0x%llX)",
                 results.num, reinterpret_cast<unsigned long long>(results.data));
    if (results.num <= 0 || !results.data) return nullptr;

    std::uint64_t inst = 0;
    seh_read_u64(reinterpret_cast<std::uintptr_t>(results.data), &inst);
    OPENDOJO_LOG("native_menu: GVS instance @0x%llX",
                 static_cast<unsigned long long>(inst));
    return reinterpret_cast<void*>(inst);
}

void dump_object_memory(void* obj, std::uintptr_t len, const char* label);
void inspect_uobject_pointer(std::uintptr_t ptr, const char* tag);

void try_add_to_viewport(void* widget) {
    if (!widget || !g_resolved.add_widget_to_slate) {
        OPENDOJO_LOG("native_menu: AddToViewport pre-reqs missing");
        return;
    }

    // Pre-call diagnostics — figure out which silent-fail path triggers.
    {
        auto pol_base = memory::polaris_base();
        if (pol_base) {
            std::uint64_t gengine = 0;
            seh_read_u64(pol_base + GENGINE_PTR_RVA, &gengine);
            OPENDOJO_LOG("native_menu: GEngine (DAT_149b4f760) = 0x%llX",
                         static_cast<unsigned long long>(gengine));
        }
    }
    // Re-derive the widget's "world" the same way FUN_144787400 will.
    void* derived_world = call_vtable_at_off(widget, WIDGET_GET_PLAYER_VT_BYTE_OFF,
                                             "world-getter (pre-add)");
    if (derived_world) {
        // EWorldType byte at +0x13A. Valid types per FUN_1454f2fa0 mask
        // 0x6A: 1=Game, 3=PIE, 5=GamePreview, 6=GameRPC.
        std::uint32_t world_type = 0;
        seh_read_u32(reinterpret_cast<std::uintptr_t>(derived_world) + 0x138,
                     &world_type);
        std::uint64_t world_vt = 0;
        seh_read_u64(reinterpret_cast<std::uintptr_t>(derived_world), &world_vt);
        std::uint32_t world_class_name_idx = 0;
        std::uint64_t world_class = 0;
        seh_read_u64(reinterpret_cast<std::uintptr_t>(derived_world) + UOBJECT_CLASS_OFF,
                     &world_class);
        if (world_class) {
            seh_read_u32(world_class + 0x18, &world_class_name_idx);
        }
        OPENDOJO_LOG("native_menu: pre-add world=0x%llX vt=0x%llX class=0x%llX "
                     "class-FName=%u type-byte=0x%X",
                     reinterpret_cast<unsigned long long>(derived_world),
                     static_cast<unsigned long long>(world_vt),
                     static_cast<unsigned long long>(world_class),
                     world_class_name_idx,
                     (world_type >> 16) & 0xFF);  // EWorldType is byte at +0x13A, packed in u32 at +0x138
    }

    void* gvs = find_game_viewport_subsystem();
    if (!gvs) {
        OPENDOJO_LOG("native_menu: no GameViewportSubsystem instance");
        return;
    }

    // Build slot_params mirroring the AddToViewport exec stub's layout, but
    // with anchors fixed. Per FUN_144787400 (the real AddToViewport):
    //   bytes  0..15 : FMargin Offsets (4 floats: Left, Top, Right, Bottom)
    //   bytes 16..31 : FAnchors        (4 floats: Min.X, Min.Y, Max.X, Max.Y)
    //   bytes 32..47 : FVector2D Alignment (2 doubles in UE5.4)
    //   bytes 48..63 : another FVector2D (2 doubles) — purpose unknown
    //   bytes 64..67 : ZOrder (int32)
    //   bytes 68..71 : (unused / padding)
    //
    // The exec stub copies 16 bytes from .rdata @+0x6da07a0 as the anchors,
    // but that block is 2 doubles (1.0, 1.0) — when the inner function reads
    // it as 4 floats, you get (0.0, 1.875, 0.0, 1.875), which collapses the
    // widget to a zero-size point ~187% down the screen (invisible).
    //
    // Hardcoding to (0,0,1,1) gives full-screen anchors. The widget will
    // occupy the whole viewport overlay; ZOrder=100 lifts it above the HUD.
    alignas(16) std::uint8_t slot_params[128] = {};

    {
        const float anchors[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        std::memcpy(slot_params + 16, anchors, sizeof(anchors));
        OPENDOJO_LOG("native_menu: slot anchors hardcoded: %.3f %.3f %.3f %.3f",
                     anchors[0], anchors[1], anchors[2], anchors[3]);
    }

    // ZOrder at offset 64 (4 bytes).
    std::int32_t zorder = 100;
    std::memcpy(slot_params + 64, &zorder, sizeof(zorder));

    call_add_widget(gvs, widget, slot_params);

    // Post-add diagnostic. FUN_144787400 OR's 0x20 into widget+0xE1 right
    // after passing the world+viewport checks, before the actual slate
    // construction. So this byte tells us whether we got past the early
    // error paths.
    std::uint32_t post_e0 = 0;
    seh_read_u32(reinterpret_cast<std::uintptr_t>(widget) + 0xE0, &post_e0);
    OPENDOJO_LOG("native_menu: post-add widget+0xE0..E3 = 0x%08X "
                 "(bit 0x20 in +0xE1 = 'added to screen' flag)", post_e0);

    dump_object_memory(widget, 0x300, "OUR widget");

    // Same per-instance field inspection as the live dumps, for direct
    // comparison with LIVE[N]+0x30-> lines above.
    std::uint64_t our_p30 = 0, our_p20 = 0;
    seh_read_u64(reinterpret_cast<std::uintptr_t>(widget) + 0x30, &our_p30);
    seh_read_u64(reinterpret_cast<std::uintptr_t>(widget) + 0x20, &our_p20);
    inspect_uobject_pointer(our_p20, "OUR+0x20 (Outer)->");
    inspect_uobject_pointer(our_p30, "OUR+0x30->");
}

// Static cursor that selects which BP subclass to construct on each show().
// Cycles on each F11 so the user can step through candidates if the first
// one renders nothing. Reset to 0 on every fresh discovery pass.
int g_subclass_cursor = 0;

// Hex-dump a UObject's first `len` bytes. Used to compare the structure of
// our constructed widget vs a live, populated in-game instance.
void dump_object_memory(void* obj, std::uintptr_t len, const char* label) {
    if (!obj) return;
    OPENDOJO_LOG("native_menu: %s memory dump (first 0x%llX bytes) @0x%llX:",
                 label, len, reinterpret_cast<unsigned long long>(obj));
    for (std::uintptr_t off = 0; off < len; off += 0x20) {
        std::uint64_t qw[4] = {};
        bool any_read = false;
        for (int i = 0; i < 4; ++i) {
            any_read |= seh_read_u64(
                reinterpret_cast<std::uintptr_t>(obj) + off + i * 8, &qw[i]);
        }
        if (!any_read) {
            OPENDOJO_LOG("  +0x%03llX  <unreadable>", off);
            continue;
        }
        OPENDOJO_LOG("  +0x%03llX  %016llX %016llX %016llX %016llX",
                     off, qw[0], qw[1], qw[2], qw[3]);
    }
}

// Enumerate all classes deriving from `target_cls` and log their pointers,
// FName indices, instance counts, and a sample instance. Returns the
// `g_subclass_cursor`'th direct subclass (super == target_cls) found, or
// `target_cls` itself if none exist. Used to find BP subclasses of
// UPolarisUMGTextMenu — the pure C++ base has no visual children, so
// constructing a widget from it renders nothing. A BP subclass will have
// the actual widget tree.
void* discover_subclasses_of(std::uintptr_t target_cls, const char* label) {
    if (!g_resolved.find_objects_of_class) return reinterpret_cast<void*>(target_cls);

    UE_TArray results{};
    __try {
        g_resolved.find_objects_of_class(
            reinterpret_cast<void*>(target_cls), &results,
            /*include_derived=*/true,
            /*exclude_object_flags=*/0,    // include CDOs so we see all classes
            /*exclude_internal_flags=*/0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("native_menu: SEH in discover_subclasses(%s)", label);
        return reinterpret_cast<void*>(target_cls);
    }

    OPENDOJO_LOG("native_menu: discover %s — %d total objects",
                 label, results.num);
    if (results.num <= 0 || !results.data) return reinterpret_cast<void*>(target_cls);

    constexpr int MAX_CLASSES = 24;
    std::uint64_t classes[MAX_CLASSES]      = {};
    int           counts[MAX_CLASSES]       = {};
    std::uint64_t sample_inst[MAX_CLASSES]  = {};
    std::uint32_t sample_flags[MAX_CLASSES] = {};
    int           unique_count              = 0;

    // Track live (non-CDO) instances separately so we can dump their memory
    // for comparison. When the user opens the practice menu in-game, that
    // menu's widget IS a UPolarisUMGTextMenu-derived live instance — and
    // its byte layout is exactly what we need to clone.
    constexpr int MAX_LIVE_DUMPS = 4;
    std::uint64_t live_dumps[MAX_LIVE_DUMPS]      = {};
    std::uint64_t live_dump_class[MAX_LIVE_DUMPS] = {};
    int           live_dump_count                 = 0;

    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t obj = 0;
        if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &obj)
            || !obj) continue;
        std::uint64_t cls = 0;
        if (!seh_read_u64(obj + UOBJECT_CLASS_OFF, &cls) || !cls) continue;

        std::uint32_t obj_flags = 0;
        seh_read_u32(obj + 0x08, &obj_flags);
        const bool is_cdo = (obj_flags & 0x10) != 0;
        if (!is_cdo && live_dump_count < MAX_LIVE_DUMPS) {
            live_dumps[live_dump_count]      = obj;
            live_dump_class[live_dump_count] = cls;
            ++live_dump_count;
        }

        int idx = -1;
        for (int c = 0; c < unique_count; ++c) {
            if (classes[c] == cls) { idx = c; break; }
        }
        if (idx == -1) {
            if (unique_count >= MAX_CLASSES) continue;
            idx = unique_count++;
            classes[idx] = cls;
            sample_inst[idx] = obj;
            sample_flags[idx] = obj_flags;
        }
        ++counts[idx];
    }

    // Collect direct subclasses (super == target_cls) into a separate list
    // so we can pick the g_subclass_cursor'th one.
    std::uint64_t subclasses[MAX_CLASSES] = {};
    std::uint32_t subclass_fnames[MAX_CLASSES] = {};
    int subclass_count = 0;

    OPENDOJO_LOG("native_menu: %s — %d unique classes:", label, unique_count);
    for (int c = 0; c < unique_count; ++c) {
        std::uint32_t cls_idx = 0;
        std::uint64_t cls_super = 0;
        seh_read_u32(classes[c] + 0x18, &cls_idx);
        seh_read_u64(classes[c] + 0x40, &cls_super);
        char name_buf[128] = {};
        decode_fname(cls_idx, name_buf, sizeof(name_buf));
        const bool is_cdo = (sample_flags[c] & 0x10) != 0;
        const bool is_direct = (cls_super == target_cls);
        OPENDOJO_LOG("native_menu:   [%d] class=0x%llX  '%s' (FName=%u)  "
                     "super=0x%llX  count=%d  flags=0x%X %s%s",
                     c,
                     static_cast<unsigned long long>(classes[c]),
                     name_buf, cls_idx,
                     static_cast<unsigned long long>(cls_super),
                     counts[c],
                     sample_flags[c],
                     is_cdo ? "(CDO only)" : "(LIVE)",
                     is_direct ? " [DIRECT SUBCLASS]" : "");
        if (is_direct && subclass_count < MAX_CLASSES) {
            subclasses[subclass_count] = classes[c];
            subclass_fnames[subclass_count] = cls_idx;
            ++subclass_count;
        }
    }

    if (subclass_count == 0) {
        OPENDOJO_LOG("native_menu: no direct subclasses found — using base");
        return reinterpret_cast<void*>(target_cls);
    }

    // If any live instances exist, dump them. This is the killer feature
    // for reverse-engineering the practice menu — open the menu in-game,
    // F11, and we capture its byte layout.
    if (live_dump_count > 0) {
        OPENDOJO_LOG("native_menu: found %d LIVE %s instance(s) — "
                     "dumping for comparison:",
                     live_dump_count, label);
        for (int i = 0; i < live_dump_count; ++i) {
            char tag[64];
            std::snprintf(tag, sizeof(tag),
                          "LIVE-%s[%d] class=0x%llX",
                          label, i,
                          static_cast<unsigned long long>(live_dump_class[i]));
            dump_object_memory(reinterpret_cast<void*>(live_dumps[i]),
                               0x300, tag);
        }
    } else {
        OPENDOJO_LOG("native_menu: no live %s instances "
                     "(open the practice menu in-game and press F11 again)",
                     label);
    }

    int pick = g_subclass_cursor % subclass_count;
    OPENDOJO_LOG("native_menu: picking subclass [%d/%d] class=0x%llX FName=%u "
                 "(cursor=%d; F11 cycles to next)",
                 pick, subclass_count,
                 static_cast<unsigned long long>(subclasses[pick]),
                 subclass_fnames[pick],
                 g_subclass_cursor);
    return reinterpret_cast<void*>(subclasses[pick]);
}

// Inspect a heap pointer suspected to be a UObject. Reads its vtable +
// class field, decodes the class FName, and logs both. Falls back to
// "not a UObject" if the header doesn't look right. Useful for figuring
// out per-instance fields like the +0x030 we're trying to identify.
void inspect_uobject_pointer(std::uintptr_t ptr, const char* tag) {
    if (!ptr) {
        OPENDOJO_LOG("native_menu: %s = NULL", tag);
        return;
    }
    std::uint64_t vt = 0;
    std::uint64_t cls = 0;
    std::uint32_t obj_flags = 0;
    std::uint32_t name_idx = 0;
    if (!seh_read_u64(ptr, &vt) || !vt) {
        OPENDOJO_LOG("native_menu: %s = 0x%llX (no vtable - not UObject)",
                     tag, static_cast<unsigned long long>(ptr));
        return;
    }
    seh_read_u32(ptr + 0x08, &obj_flags);
    seh_read_u64(ptr + 0x10, &cls);
    seh_read_u32(ptr + 0x18, &name_idx);

    if (!cls || cls < 0x100000) {
        OPENDOJO_LOG("native_menu: %s = 0x%llX (vtable=0x%llX "
                     "no UObject class)",
                     tag, static_cast<unsigned long long>(ptr),
                     static_cast<unsigned long long>(vt));
        return;
    }
    std::uint32_t cls_name_idx = 0;
    seh_read_u32(cls + 0x18, &cls_name_idx);
    char cls_name[128] = {};
    decode_fname(cls_name_idx, cls_name, sizeof(cls_name));
    char obj_name[128] = {};
    decode_fname(name_idx, obj_name, sizeof(obj_name));
    OPENDOJO_LOG("native_menu: %s = 0x%llX  class='%s' (0x%llX)  name='%s'  "
                 "flags=0x%X  vtable=0x%llX",
                 tag, static_cast<unsigned long long>(ptr),
                 cls_name, static_cast<unsigned long long>(cls),
                 obj_name, obj_flags,
                 static_cast<unsigned long long>(vt));
}

// Quiet variant: return the first live (non-CDO) UUserWidget-derived
// instance whose class FName matches `target_name` exactly. No logging.
// Used by the delegate probe to find the practice menu widget directly
// when the PracticeMenuImpl singleton's initialized flag is unreliable.
void* find_first_live_user_widget_by_class_name(const char* target_name) {
    if (!target_name || !g_resolved.find_class
        || !g_resolved.find_objects_of_class) return nullptr;
    auto* uw_cls = g_resolved.find_class(
        nullptr, L"/Script/UMG.UserWidget", true);
    if (!uw_cls) return nullptr;

    UE_TArray results{};
    __try {
        g_resolved.find_objects_of_class(
            uw_cls, &results,
            /*include_derived=*/true,
            /*exclude_object_flags=*/0x30,  // skip CDOs + Archetypes
            /*exclude_internal_flags=*/0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (results.num <= 0 || !results.data) return nullptr;

    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    std::size_t target_len = std::strlen(target_name);
    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t obj = 0;
        if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &obj)
            || !obj) continue;
        std::uint64_t cls = 0;
        if (!seh_read_u64(obj + UOBJECT_CLASS_OFF, &cls) || !cls) continue;
        std::uint32_t cls_idx = 0;
        seh_read_u32(cls + 0x18, &cls_idx);
        char name_buf[128] = {};
        decode_fname(cls_idx, name_buf, sizeof(name_buf));
        if (std::strncmp(name_buf, target_name, target_len) == 0
            && name_buf[target_len] == '\0') {
            return reinterpret_cast<void*>(obj);
        }
    }
    return nullptr;
}

// Find a UClass object whose name (FName) decodes to exactly `target_name`,
// among classes derived from UUserWidget. Returns the class pointer or
// nullptr if nothing matches. Logs every live instance found (and dumps the
// first 4 of them in detail), so we can compare multiple instances to find
// the actually-displayed one.
void* find_widget_class_by_name(const char* target_name) {
    if (!target_name || !g_resolved.find_class
        || !g_resolved.find_objects_of_class) return nullptr;
    auto* uw_cls = g_resolved.find_class(
        nullptr, L"/Script/UMG.UserWidget", true);
    if (!uw_cls) return nullptr;

    UE_TArray results{};
    __try {
        g_resolved.find_objects_of_class(
            uw_cls, &results,
            /*include_derived=*/true,
            /*exclude_object_flags=*/0,    // include CDOs (we want class pointers)
            /*exclude_internal_flags=*/0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (results.num <= 0 || !results.data) return nullptr;

    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    void* matched = nullptr;
    std::size_t target_len = std::strlen(target_name);
    constexpr int MAX_DUMPS = 4;
    int dump_count = 0;
    int total_live = 0;

    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t obj = 0;
        if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &obj)
            || !obj) continue;
        std::uint64_t cls = 0;
        if (!seh_read_u64(obj + UOBJECT_CLASS_OFF, &cls) || !cls) continue;
        std::uint32_t cls_idx = 0;
        seh_read_u32(cls + 0x18, &cls_idx);
        char name_buf[128] = {};
        decode_fname(cls_idx, name_buf, sizeof(name_buf));
        if (std::strncmp(name_buf, target_name, target_len) == 0
            && name_buf[target_len] == '\0') {
            if (!matched) {
                matched = reinterpret_cast<void*>(cls);
                OPENDOJO_LOG("native_menu: matched '%s' -> class=0x%llX",
                             target_name,
                             reinterpret_cast<unsigned long long>(matched));
            }
            std::uint32_t obj_flags = 0;
            seh_read_u32(obj + 0x08, &obj_flags);
            const bool is_cdo = (obj_flags & 0x10) != 0;
            if (is_cdo) continue;

            ++total_live;
            // Read a couple of key fields to summarize state.
            std::uint64_t widget_tree = 0;
            std::uint64_t slot_at_28  = 0;
            std::uint64_t slate_at_100 = 0;
            seh_read_u64(obj + 0x228, &widget_tree);
            seh_read_u64(obj + 0x28,  &slot_at_28);
            seh_read_u64(obj + 0x100, &slate_at_100);
            OPENDOJO_LOG("native_menu: live[%d] @0x%llX flags=0x%X "
                         "+0x28(slate-ish)=0x%llX +0x100(slate-ref)=0x%llX "
                         "+0x228(WidgetTree)=0x%llX",
                         total_live - 1,
                         static_cast<unsigned long long>(obj), obj_flags,
                         static_cast<unsigned long long>(slot_at_28),
                         static_cast<unsigned long long>(slate_at_100),
                         static_cast<unsigned long long>(widget_tree));

            if (dump_count < MAX_DUMPS) {
                char tag[64];
                std::snprintf(tag, sizeof(tag), "LIVE[%d]", dump_count);
                dump_object_memory(reinterpret_cast<void*>(obj), 0x300, tag);

                // Inspect the per-instance fields that differ from ours.
                // +0x030 is the prime suspect (our null, all live set).
                std::uint64_t p30 = 0, p20 = 0;
                seh_read_u64(obj + 0x30, &p30);
                seh_read_u64(obj + 0x20, &p20);
                char tag30[64], tag20[64];
                std::snprintf(tag30, sizeof(tag30),
                              "LIVE[%d]+0x30->", dump_count);
                std::snprintf(tag20, sizeof(tag20),
                              "LIVE[%d]+0x20 (Outer)->", dump_count);
                inspect_uobject_pointer(p20, tag20);
                inspect_uobject_pointer(p30, tag30);
                ++dump_count;
            }
        }
    }
    OPENDOJO_LOG("native_menu: total live instances of '%s' = %d",
                 target_name, total_live);
    if (!matched) {
        OPENDOJO_LOG("native_menu: no class found with name '%s' "
                     "(searched %d UUserWidget-derived objects)",
                     target_name, results.num);
    }
    return matched;
}

// Enumerate ALL live (non-CDO) UUserWidget-derived instances. Logs each
// one's class FName + super chain so we can identify what widget the
// practice menu actually is. Caller is responsible for resolution prereqs.
void enumerate_live_user_widgets() {
    if (!g_resolved.find_objects_of_class || !g_resolved.find_class) return;
    auto* uw_cls = g_resolved.find_class(nullptr, L"/Script/UMG.UserWidget", true);
    OPENDOJO_LOG("native_menu: enumerate_live_user_widgets — UUserWidget class=0x%llX",
                 reinterpret_cast<unsigned long long>(uw_cls));
    if (!uw_cls) return;

    UE_TArray results{};
    __try {
        g_resolved.find_objects_of_class(
            uw_cls, &results,
            /*include_derived=*/true,
            /*exclude_object_flags=*/0x30,   // skip CDO + Archetype
            /*exclude_internal_flags=*/0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("native_menu: SEH in enumerate_live_user_widgets");
        return;
    }
    OPENDOJO_LOG("native_menu: live UUserWidget instances: %d", results.num);
    if (results.num <= 0 || !results.data) return;

    // Group by class. We want to see unique classes with counts.
    constexpr int MAX = 64;
    std::uint64_t classes[MAX] = {};
    int           counts[MAX]  = {};
    std::uint64_t samples[MAX] = {};
    int unique = 0;
    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t obj = 0;
        if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &obj)
            || !obj) continue;
        std::uint64_t cls = 0;
        if (!seh_read_u64(obj + UOBJECT_CLASS_OFF, &cls) || !cls) continue;

        int idx = -1;
        for (int c = 0; c < unique; ++c) {
            if (classes[c] == cls) { idx = c; break; }
        }
        if (idx == -1) {
            if (unique >= MAX) continue;
            idx = unique++;
            classes[idx] = cls;
            samples[idx] = obj;
        }
        ++counts[idx];
    }

    OPENDOJO_LOG("native_menu: %d unique UUserWidget-derived classes live:", unique);
    for (int c = 0; c < unique; ++c) {
        std::uint32_t cls_idx = 0;
        std::uint64_t cls_super = 0;
        std::uint64_t cls_outer = 0;
        seh_read_u32(classes[c] + 0x18, &cls_idx);
        seh_read_u64(classes[c] + 0x40, &cls_super);
        seh_read_u64(classes[c] + 0x20, &cls_outer);
        char cls_name[128] = {};
        decode_fname(cls_idx, cls_name, sizeof(cls_name));

        // Walk the super chain. Log decoded name of immediate super only
        // (full chain is noise once we have the leaf name).
        std::uint32_t super_idx = 0;
        char super_name[128] = "(none)";
        if (cls_super) {
            seh_read_u32(cls_super + 0x18, &super_idx);
            decode_fname(super_idx, super_name, sizeof(super_name));
        }

        OPENDOJO_LOG("native_menu:   [%d] %s : %s (count=%d class=0x%llX sample=0x%llX)",
                     c, cls_name, super_name, counts[c],
                     static_cast<unsigned long long>(classes[c]),
                     static_cast<unsigned long long>(samples[c]));
    }
}

// -----------------------------------------------------------------------------
// Practice-menu delegate probe.
//
// The outer WBP_UI_PracticeMenu_C widget has 10 std::function-like
// "binder" slots at widget+0x290..+0x4D0. The manager (PracticeMenuImpl)
// binds them at open time so the widget can call back into native C++
// when the user navigates, confirms, cancels, transitions, etc. We don't
// yet know which slot handles which user action — this probe replaces
// each slot's fn pointer with a per-slot logging stub that forwards to
// the original, so a single navigation pass through the menu maps each
// user action to its slot.
//
// Once mapped, the same swap mechanism becomes the Phase 1 hijack:
// replace just the slot we care about with our own handler.
// -----------------------------------------------------------------------------

struct DelegateProbe {
    void*          widget                          = nullptr;
    std::uintptr_t original_fn[WIDGET_DELEGATE_SLOT_COUNT] = {0};
    bool           slot_hooked[WIDGET_DELEGATE_SLOT_COUNT] = {false};
    int            calls_per_slot[WIDGET_DELEGATE_SLOT_COUNT] = {0};
};
DelegateProbe g_probe;

// One stub per slot index. The widget's binder vtable calls into us with
//   rcx = receiver (PracticeMenuImpl*),
//   rdx = arg0 (event-specific; for selection callbacks this is an int*),
//   r8  = arg1, r9 = arg2.
// We log the args and forward to the original fn pointer so the menu
// stays interactive. Templated so each slot keeps a distinct entry
// address (we need 10 unique stub addresses to write into the 10 slots).
template <int N>
__declspec(noinline)
std::uintptr_t probe_stub(std::uintptr_t rcx,
                          std::uintptr_t rdx,
                          std::uintptr_t r8,
                          std::uintptr_t r9) {
    int rdx_as_int = -1;
    if (rdx > 0x10000) {
        std::uint32_t v = 0;
        if (seh_read_u32(rdx, &v)) rdx_as_int = static_cast<int>(v);
    }
    g_probe.calls_per_slot[N]++;
    OPENDOJO_LOG("probe: SLOT %d fired (call #%d) "
                 "recv=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX *(i32*)rdx=%d",
                 N, g_probe.calls_per_slot[N],
                 static_cast<unsigned long long>(rcx),
                 static_cast<unsigned long long>(rdx),
                 static_cast<unsigned long long>(r8),
                 static_cast<unsigned long long>(r9),
                 rdx_as_int);
    if (g_probe.original_fn[N]) {
        using Fn4 = std::uintptr_t(*)(std::uintptr_t,
                                      std::uintptr_t,
                                      std::uintptr_t,
                                      std::uintptr_t);
        __try {
            return reinterpret_cast<Fn4>(g_probe.original_fn[N])(
                rcx, rdx, r8, r9);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            OPENDOJO_LOG("probe: SLOT %d original threw SEH — swallowed", N);
        }
    }
    return 0;
}

const std::uintptr_t kProbeStubs[WIDGET_DELEGATE_SLOT_COUNT] = {
    reinterpret_cast<std::uintptr_t>(&probe_stub<0>),
    reinterpret_cast<std::uintptr_t>(&probe_stub<1>),
    reinterpret_cast<std::uintptr_t>(&probe_stub<2>),
    reinterpret_cast<std::uintptr_t>(&probe_stub<3>),
    reinterpret_cast<std::uintptr_t>(&probe_stub<4>),
    reinterpret_cast<std::uintptr_t>(&probe_stub<5>),
    reinterpret_cast<std::uintptr_t>(&probe_stub<6>),
    reinterpret_cast<std::uintptr_t>(&probe_stub<7>),
    reinterpret_cast<std::uintptr_t>(&probe_stub<8>),
    reinterpret_cast<std::uintptr_t>(&probe_stub<9>),
};

// Resolve the live outer practice-menu widget. First tries the
// PracticeMenuImpl singleton path (cheapest); if that's unusable —
// singleton null, +0x1B8 cleared, or weak ptr stale — falls back to
// GetObjectsOfClass(WBP_UI_PracticeMenu_C). Returns nullptr only when
// no matching live widget exists at all, which is the real "menu not
// open" signal.
void* get_practice_menu_outer_widget() {
    auto pol_base = memory::polaris_base();
    if (!pol_base) return nullptr;

    void* widget_via_singleton = nullptr;
    std::uint64_t singleton = 0;
    if (seh_read_u64(pol_base + PRACTICE_MENU_SINGLETON_RVA, &singleton)
        && singleton) {
        std::uint8_t initialized = 0;
        seh_read_u8(singleton + PMI_INITIALIZED_BYTE, &initialized);
        if (initialized) {
            using GetFn = void* (*)(void* weak_ptr);
            auto get_fn = reinterpret_cast<GetFn>(
                pol_base + WEAK_OBJ_PTR_GET_RVA);
            __try {
                widget_via_singleton = get_fn(reinterpret_cast<void*>(
                    singleton + PMI_OUTER_WIDGET_WEAKPTR));
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                widget_via_singleton = nullptr;
            }
        }
    }
    if (widget_via_singleton) return widget_via_singleton;

    // Fallback: enumerate live UUserWidget instances and match by class
    // FName. WBP_UI_PracticeMenu_C is the class the in-game pause menu
    // is built from (per RE notes).
    void* widget = find_first_live_user_widget_by_class_name(
        "WBP_UI_PracticeMenu_C");
    if (!widget) {
        OPENDOJO_LOG("probe: no live WBP_UI_PracticeMenu_C "
                     "(singleton=0x%llX init=%d) — open the practice menu",
                     static_cast<unsigned long long>(singleton),
                     singleton ? 1 : 0);
        return nullptr;
    }
    OPENDOJO_LOG("probe: outer widget via class enum = 0x%llX "
                 "(singleton was unusable)",
                 reinterpret_cast<unsigned long long>(widget));
    return widget;
}

// -----------------------------------------------------------------------------
// Phase A — practice-menu widget subtree dump.
//
// The 14 left-list rows live somewhere inside the WidgetTree at outer+0x228.
// dump_practice_menu_delegate_slots reports 12 "immediate children" via the
// existing widget_tree+0x40 heuristic — likely panels + sub-panels + a list
// container, not the rows themselves. Rather than trust an offset that may
// have shifted in the Polaris build, this helper enumerates every live
// UWidget whose Outer chain reaches the practice menu widget. That gives
// us a complete listing of the subtree without depending on UWidgetTree
// internals.
//
// Output per widget: depth (hop count to menu), instance FName, class
// FName, address, and — when the widget looks UPanelWidget-shaped — the
// number of Slots at the standard +0x150 / +0x158 offset. That's enough
// to spot the items container.
// -----------------------------------------------------------------------------

// Quiet helper: decode the class FName of an arbitrary UObject pointer.
// Writes "(null)" / "(?)" on failure so the caller's log line stays
// formatted. out_sz must be non-zero.
void decode_uobj_class_name(std::uint64_t obj, char* out, std::size_t out_sz) {
    if (out_sz == 0) return;
    out[0] = '\0';
    if (!obj) { std::snprintf(out, out_sz, "(null)"); return; }
    std::uint64_t cls = 0;
    if (!seh_read_u64(obj + UOBJECT_CLASS_OFF, &cls) || !cls) {
        std::snprintf(out, out_sz, "(?)"); return;
    }
    std::uint32_t idx = 0;
    if (!seh_read_u32(cls + 0x18, &idx)) {
        std::snprintf(out, out_sz, "(?)"); return;
    }
    decode_fname(idx, out, out_sz);
}

// Read the canonical UPanelWidget Slots TArray. The standard UMG layout
// has data ptr at +0x150 and Num at +0x158. Returns true and fills
// *out_num when the contents look plausible (non-zero data ptr and a
// sane count). Plenty of non-panel widgets will *also* have non-null
// QWORDs at +0x150 — treat the result as a heuristic, not a guarantee.
bool read_panel_slot_count(std::uint64_t panel, std::uint32_t* out_num) {
    std::uint64_t data = 0;
    std::uint32_t num  = 0;
    if (!seh_read_u64(panel + 0x150, &data)) return false;
    if (!seh_read_u32(panel + 0x158, &num))  return false;
    if (!data || num == 0 || num > 128) return false;
    if (out_num) *out_num = num;
    return true;
}

// Try to interpret `field_addr` as the address of a UE5 FText member.
// FText layout (stock UE5):
//   FText { TSharedRef<ITextData> Data; uint32 Flags; }   ; 24 bytes
//   ITextData {
//     +0x00 vtable           (module addr)
//     +0x08 FString Data*    (heap addr to wide chars)
//     +0x10 int32 Num        (chars including NUL, 1..256-ish)
//     +0x14 int32 Max        (>= Num)
//   }
// If the chain looks plausible we ASCII-fold the wide string and log it.
// Returns true on a successful extraction; quiet on failures. The caller
// owns `tag` — it's prefixed verbatim. `out_buf`/`out_buf_sz` are
// optional: when supplied, the ASCII-folded string is also returned.
bool try_extract_ftext_at(std::uint64_t field_addr,
                          const char*    tag,
                          char*          out_buf,
                          std::size_t    out_buf_sz) {
    auto pol_base = memory::polaris_base();

    std::uint64_t text_data = 0;
    if (!seh_read_u64(field_addr, &text_data) || !text_data) return false;

    std::uint64_t td_vtable = 0;
    if (!seh_read_u64(text_data, &td_vtable) || !td_vtable) return false;

    // The TextData impl has its vtable in module .rdata. If we know the
    // module base, gate on that — keeps the false-positive rate low.
    // (0x30000000 is a comfortable upper bound for Polaris's image size.)
    if (pol_base && (td_vtable < pol_base
                     || td_vtable > pol_base + 0x30000000ULL)) {
        return false;
    }

    std::uint64_t str_data = 0;
    std::uint32_t str_num  = 0;
    std::uint32_t str_max  = 0;
    if (!seh_read_u64(text_data + 0x08, &str_data) || !str_data) return false;
    if (!seh_read_u32(text_data + 0x10, &str_num))               return false;
    if (!seh_read_u32(text_data + 0x14, &str_max))               return false;
    if (str_num == 0 || str_num > 256)                           return false;
    if (str_max < str_num)                                       return false;

    // Read up to 200 wide chars, ASCII-fold for log readability.
    char ascii[208] = {};
    std::uint32_t n = str_num > 200 ? 200 : str_num;
    bool any_printable = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint16_t ch = 0;
        if (!seh_read_u16(str_data + i * 2, &ch)) { ascii[i] = '?'; continue; }
        if (ch == 0) { ascii[i] = '\0'; break; }
        if (ch >= 0x20 && ch <= 0x7E) {
            ascii[i] = static_cast<char>(ch);
            any_printable = true;
        } else {
            ascii[i] = '?';
        }
    }
    ascii[n] = '\0';
    if (!any_printable) return false;   // junk binary read

    OPENDOJO_LOG("%s FText TextData=0x%llX str=0x%llX num=%u '%s'",
                 tag,
                 static_cast<unsigned long long>(text_data),
                 static_cast<unsigned long long>(str_data),
                 str_num,
                 ascii);
    if (out_buf && out_buf_sz) {
        std::snprintf(out_buf, out_buf_sz, "%s", ascii);
    }
    return true;
}

// Phase C — point an FText field at a freshly-allocated TextData that we
// own, leaving the original TextData untouched. This isolates our edit:
// other widgets that share the original TextData (localization caches,
// duplicate labels) keep displaying the old text.
//
// We allocate two buffers in the process heap and leak them — Phase C is
// a research patch, not a production feature. Both buffers stay valid
// for the lifetime of the process, which is what the FText assumes.
//
// Caveat: we copy the first 0x40 bytes of the original TextData verbatim
// to preserve its vtable + any other state, then rewrite the embedded
// FString fields. If TextData implementations in this Polaris build have
// internal pointers past 0x40, this clones them incorrectly. Worth
// auditing once we see one in the wild.
bool patch_ftext_in_place(std::uint64_t field_addr,
                          const wchar_t* new_text,
                          const char*    tag) {
    std::uint64_t orig_td = 0;
    if (!seh_read_u64(field_addr, &orig_td) || !orig_td) {
        OPENDOJO_LOG("%s: patch — field 0x%llX empty", tag,
                     static_cast<unsigned long long>(field_addr));
        return false;
    }
    // Sanity check: TextData[+0x00] should be a vtable.
    std::uint64_t td_vtable = 0;
    if (!seh_read_u64(orig_td, &td_vtable) || !td_vtable) {
        OPENDOJO_LOG("%s: patch — orig TextData 0x%llX has no vtable",
                     tag, static_cast<unsigned long long>(orig_td));
        return false;
    }

    std::size_t new_len = 0;
    while (new_text[new_len]) ++new_len;
    const std::size_t bytes = (new_len + 1) * sizeof(wchar_t);

    HANDLE heap = GetProcessHeap();
    void* str_buf = HeapAlloc(heap, 0, bytes);
    if (!str_buf) return false;
    std::memcpy(str_buf, new_text, bytes);

    constexpr std::size_t CLONE_SIZE = 0x40;
    void* clone = HeapAlloc(heap, HEAP_ZERO_MEMORY, CLONE_SIZE);
    if (!clone) { HeapFree(heap, 0, str_buf); return false; }
    __try {
        std::memcpy(clone, reinterpret_cast<void*>(orig_td), CLONE_SIZE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        HeapFree(heap, 0, clone);
        HeapFree(heap, 0, str_buf);
        OPENDOJO_LOG("%s: patch — SEH reading orig TextData", tag);
        return false;
    }

    // Rewrite the embedded FString in our clone.
    auto* cb = reinterpret_cast<std::uint8_t*>(clone);
    *reinterpret_cast<std::uint64_t*>(cb + 0x08) =
        reinterpret_cast<std::uint64_t>(str_buf);
    *reinterpret_cast<std::uint32_t*>(cb + 0x10) =
        static_cast<std::uint32_t>(new_len + 1);
    *reinterpret_cast<std::uint32_t*>(cb + 0x14) =
        static_cast<std::uint32_t>(new_len + 1);

    // Swap the field. The field itself lives in heap-allocated widget
    // memory which is normally R/W; VirtualProtect just-in-case.
    DWORD old_prot = 0;
    VirtualProtect(reinterpret_cast<void*>(field_addr), 8,
                   PAGE_READWRITE, &old_prot);
    *reinterpret_cast<std::uint64_t*>(field_addr) =
        reinterpret_cast<std::uint64_t>(clone);
    if (old_prot) {
        VirtualProtect(reinterpret_cast<void*>(field_addr), 8,
                       old_prot, &old_prot);
    }

    OPENDOJO_LOG("%s: patch  field=0x%llX  orig_td=0x%llX "
                 "-> clone=0x%llX  str=0x%llX  new_len=%zu",
                 tag,
                 static_cast<unsigned long long>(field_addr),
                 static_cast<unsigned long long>(orig_td),
                 reinterpret_cast<unsigned long long>(clone),
                 reinterpret_cast<unsigned long long>(str_buf),
                 new_len);
    return true;
}

// Scan every QWORD slot in a widget's first `len` bytes; log any FText-
// shaped field that decodes to printable text. Bounded work — used to
// pull labels out of UTextBlock-derived widgets when we don't yet know
// the exact FText offset.
int scan_widget_for_ftext(std::uint64_t widget, std::uint32_t len) {
    int found = 0;
    for (std::uint32_t off = 0; off < len; off += 8) {
        char tag[80];
        std::snprintf(tag, sizeof(tag),
                      "tree:     ftext widget+0x%03X", off);
        if (try_extract_ftext_at(widget + off, tag, nullptr, 0)) ++found;
    }
    return found;
}

// Looser sibling of try_extract_ftext_at: at `ptr_addr` we expect a
// pointer-to-wide-string (heap-allocated). No vtable / TextData wrapper
// assumed. We sample the destination's first character; only accept it
// if it's printable ASCII (the high byte of the first UTF-16 unit must
// be 0). Catches labels stored as bare FString or as TCHAR* without an
// FText wrapper. Logs the decoded string on success.
bool try_extract_wide_string_at(std::uint64_t ptr_addr, const char* tag) {
    std::uint64_t str_ptr = 0;
    if (!seh_read_u64(ptr_addr, &str_ptr) || !str_ptr) return false;
    if (str_ptr < 0x10000ULL) return false;
    std::uint16_t first = 0;
    if (!seh_read_u16(str_ptr, &first)) return false;
    if (first < 0x20 || first > 0x7E) return false;

    char ascii[129] = {};
    int len = 0;
    for (int i = 0; i < 128; ++i) {
        std::uint16_t ch = 0;
        if (!seh_read_u16(str_ptr + i * 2, &ch)) break;
        if (ch == 0) break;
        if (ch >= 0x20 && ch <= 0x7E) {
            ascii[i] = static_cast<char>(ch);
        } else {
            ascii[i] = '?';
        }
        len = i + 1;
    }
    if (len < 2) return false;
    ascii[len] = '\0';
    OPENDOJO_LOG("%s wstr=0x%llX len=%d '%s'",
                 tag,
                 static_cast<unsigned long long>(str_ptr),
                 len, ascii);
    return true;
}

// Scan every QWORD slot in `widget[0..len]` for pointers to wide strings.
// Looser than scan_widget_for_ftext — catches labels stored as plain
// TCHAR pointers (no FText wrapper) when Polaris stripped the standard
// layout.
int scan_widget_for_wide_strings(std::uint64_t widget, std::uint32_t len) {
    int found = 0;
    for (std::uint32_t off = 0; off < len; off += 8) {
        char tag[80];
        std::snprintf(tag, sizeof(tag),
                      "tree:     wstr widget+0x%03X", off);
        if (try_extract_wide_string_at(widget + off, tag)) ++found;
    }
    return found;
}

// -----------------------------------------------------------------------------
// Phase C — row-label pointer swap.
//
// Every practice-menu row widget caches a direct wchar_t* to its label at
// widget+0x198 and widget+0x1C0 (verified from the F10 dump — both
// offsets hold pointers to the same FString-style buffer). Patching just
// these pointers is the smallest possible change: no FText struct
// emulation, no TextData clone, no FString length to maintain.
//
// If the menu visually still says "Help" after a swap, the rendering
// pipeline is reading from a deeper field (likely a PolarisTextBlock
// child's FText at +0x100/+0x108) and we'd need to chase that next.
// -----------------------------------------------------------------------------

struct RowLabelPatch {
    std::uint64_t widget        = 0;
    std::uint64_t orig_198      = 0;
    std::uint64_t orig_1C0      = 0;
};
constexpr int MAX_LABEL_PATCHES = 32;
RowLabelPatch g_label_patches[MAX_LABEL_PATCHES] = {};
int           g_label_patch_count = 0;
void*         g_label_new_buffer  = nullptr;  // leaked; owned for the run

// Each FString-style ref records the FString struct address (= QWORD that
// pointed at the help-text buffer) plus its original Data/Num/Max so we
// can roll back exactly.
struct FStringRefPatch {
    std::uint64_t fstring_addr  = 0;   // the FString struct (data,num,max)
    std::uint64_t orig_data     = 0;
    std::uint32_t orig_num      = 0;
    std::uint32_t orig_max      = 0;
};
constexpr int MAX_FSTRING_PATCHES = 64;
FStringRefPatch g_fstring_patches[MAX_FSTRING_PATCHES] = {};
int             g_fstring_patch_count = 0;

// Compare the wide string at `str_addr` against an ASCII `target`.
// Match requires equal length AND NUL terminator. Used to locate the row
// widget whose +0x198 holds the row text we want to replace.
bool wstr_equals_ascii(std::uint64_t str_addr, const char* target) {
    std::size_t len = std::strlen(target);
    for (std::size_t i = 0; i <= len; ++i) {
        std::uint16_t ch = 0;
        if (!seh_read_u16(str_addr + i * 2, &ch)) return false;
        std::uint16_t expect = (i < len)
            ? static_cast<std::uint16_t>(static_cast<unsigned char>(target[i]))
            : 0;
        if (ch != expect) return false;
    }
    return true;
}

// Locate every widget in the practice menu subtree whose +0x198 wide
// string equals `target_label`, and swap +0x198 + +0x1C0 to point at a
// fresh heap buffer containing `new_text`. Records originals so the swap
// is reversible. Returns the number of widgets patched.
int patch_menu_row_label(const char* target_label, const wchar_t* new_text) {
    void* menu = get_practice_menu_outer_widget();
    if (!menu) {
        OPENDOJO_LOG("patch-label: no practice menu widget");
        return 0;
    }
    auto menu_addr = reinterpret_cast<std::uint64_t>(menu);

    if (!g_resolved.find_objects_of_class || !g_resolved.find_class) {
        OPENDOJO_LOG("patch-label: resolver not initialised");
        return 0;
    }
    auto* widget_cls = g_resolved.find_class(
        nullptr, L"/Script/UMG.Widget", true);
    if (!widget_cls) return 0;

    UE_TArray results{};
    __try {
        g_resolved.find_objects_of_class(
            widget_cls, &results,
            /*include_derived=*/true, /*flags=*/0x30, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("patch-label: SEH in GetObjectsOfClass");
        return 0;
    }
    if (!results.data || results.num <= 0) return 0;

    // Allocate the shared replacement buffer on first run.
    if (!g_label_new_buffer) {
        std::size_t n = 0;
        while (new_text[n]) ++n;
        g_label_new_buffer = HeapAlloc(GetProcessHeap(), 0,
                                       (n + 1) * sizeof(wchar_t));
        if (!g_label_new_buffer) return 0;
        std::memcpy(g_label_new_buffer, new_text, (n + 1) * sizeof(wchar_t));
    }
    auto new_ptr = reinterpret_cast<std::uint64_t>(g_label_new_buffer);

    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    int patched         = 0;
    int n_reachable     = 0;   // widgets whose Outer chain reaches menu
    int n_with_198      = 0;   // reachable & +0x198 non-null
    int n_198_wstr      = 0;   // reachable & +0x198 dereferences to wide str
    int n_label_match   = 0;   // reachable & +0x198 wstring == target
    char first_wstr[64] = {};  // record the first non-empty +0x198 wstring
    for (std::int32_t i = 0;
         i < results.num && g_label_patch_count < MAX_LABEL_PATCHES;
         ++i) {
        std::uint64_t w = 0;
        if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &w)
            || !w) continue;

        // Only widgets reachable from the menu — avoids touching look-
        // alikes (e.g. shared template widgets owned elsewhere).
        bool in_menu = false;
        std::uint64_t cur = w;
        for (int hop = 0; hop < 10; ++hop) {
            std::uint64_t outer = 0;
            if (!seh_read_u64(cur + 0x20, &outer) || !outer) break;
            if (outer == menu_addr) { in_menu = true; break; }
            cur = outer;
        }
        if (!in_menu) continue;
        ++n_reachable;

        // Check +0x198 string.
        std::uint64_t str_198 = 0;
        if (!seh_read_u64(w + 0x198, &str_198) || !str_198) continue;
        ++n_with_198;

        // Read a small preview of whatever wstring lives at +0x198.
        // We need this to know what +0x198 *does* hold in the current
        // session, even if it doesn't match "Help".
        std::uint16_t ch0 = 0;
        if (!seh_read_u16(str_198, &ch0)) continue;
        if (ch0 >= 0x20 && ch0 <= 0x7E) {
            ++n_198_wstr;
            if (first_wstr[0] == '\0') {
                for (int j = 0; j < 60; ++j) {
                    std::uint16_t ch = 0;
                    if (!seh_read_u16(str_198 + j * 2, &ch) || !ch) break;
                    first_wstr[j] = (ch >= 0x20 && ch <= 0x7E)
                        ? static_cast<char>(ch) : '?';
                }
            }
        }

        if (!wstr_equals_ascii(str_198, target_label)) continue;
        ++n_label_match;

        std::uint64_t str_1C0 = 0;
        seh_read_u64(w + 0x1C0, &str_1C0);

        // Record originals.
        g_label_patches[g_label_patch_count] = { w, str_198, str_1C0 };
        ++g_label_patch_count;

        // Swap pointers. Widget memory is heap-allocated and already R/W;
        // VirtualProtect is belt-and-braces.
        DWORD old_prot = 0;
        VirtualProtect(reinterpret_cast<void*>(w + 0x198), 0x30,
                       PAGE_READWRITE, &old_prot);
        *reinterpret_cast<std::uint64_t*>(w + 0x198) = new_ptr;
        *reinterpret_cast<std::uint64_t*>(w + 0x1C0) = new_ptr;
        if (old_prot) {
            VirtualProtect(reinterpret_cast<void*>(w + 0x198), 0x30,
                           old_prot, &old_prot);
        }
        OPENDOJO_LOG("patch-label: widget @0x%llX  +0x198/+0x1C0  '%s' -> "
                     "%ls (new_ptr=0x%llX  orig_198=0x%llX  orig_1C0=0x%llX)",
                     static_cast<unsigned long long>(w),
                     target_label, new_text,
                     static_cast<unsigned long long>(new_ptr),
                     static_cast<unsigned long long>(str_198),
                     static_cast<unsigned long long>(str_1C0));
        ++patched;
    }
    OPENDOJO_LOG("patch-label: target='%s' new='%ls' patched=%d widget(s) "
                 "(reachable=%d  with+0x198=%d  +0x198 looks like wstr=%d  "
                 "label match=%d  first wstr seen='%s'  total recorded=%d)",
                 target_label, new_text, patched,
                 n_reachable, n_with_198, n_198_wstr, n_label_match,
                 first_wstr, g_label_patch_count);

    // -------------------------------------------------------------------
    // Phase C deeper pass: the +0x198 cache wasn't read by the renderer.
    // The renderer likely uses a sibling PolarisTextBlock's FString. So
    // find the *original* Help wide-string address (we just recorded it
    // in g_label_patches), then sweep every widget reachable from the
    // menu for QWORDs that either (a) equal that address directly or (b)
    // point to an FString-shaped record { data=that_addr, num, max }.
    // Patch every match so the renderer can't miss us.
    // -------------------------------------------------------------------
    if (g_label_patch_count == 0) return patched;

    std::uint64_t help_wstr_addr = g_label_patches[0].orig_198;
    if (!help_wstr_addr) return patched;

    // Length of the new text including NUL — used to update FString.Num /
    // .Max so the renderer reads the full replacement.
    std::uint32_t new_len_inc_nul = 0;
    while (new_text[new_len_inc_nul]) ++new_len_inc_nul;
    ++new_len_inc_nul;

    int direct_refs  = 0;
    int fstring_refs = 0;

    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t w = 0;
        if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &w)
            || !w) continue;
        // Only widgets reachable from the menu.
        bool in_menu = false;
        std::uint64_t cur = w;
        for (int hop = 0; hop < 10; ++hop) {
            std::uint64_t outer = 0;
            if (!seh_read_u64(cur + 0x20, &outer) || !outer) break;
            if (outer == menu_addr) { in_menu = true; break; }
            cur = outer;
        }
        if (!in_menu) continue;

        for (std::uint32_t off = 0; off < 0x300; off += 8) {
            std::uint64_t q = 0;
            if (!seh_read_u64(w + off, &q) || !q) continue;

            // Direct ref: widget+off == wstr addr.
            if (q == help_wstr_addr) {
                // Already handled +0x198 / +0x1C0 above; skip those.
                if (off == 0x198 || off == 0x1C0) continue;
                DWORD oldp = 0;
                VirtualProtect(reinterpret_cast<void*>(w + off), 8,
                               PAGE_READWRITE, &oldp);
                *reinterpret_cast<std::uint64_t*>(w + off) =
                    reinterpret_cast<std::uint64_t>(g_label_new_buffer);
                if (oldp) VirtualProtect(reinterpret_cast<void*>(w + off),
                                         8, oldp, &oldp);
                OPENDOJO_LOG("patch-label: direct-ref  widget @0x%llX +0x%X "
                             "(was 0x%llX)",
                             static_cast<unsigned long long>(w),
                             off,
                             static_cast<unsigned long long>(q));
                ++direct_refs;
                continue;
            }

            // FString ref: widget+off -> FString { data=help_addr, num, max }.
            // Plausible FString heap pointer.
            if (q < 0x10000ULL) continue;
            std::uint64_t fstr_data = 0;
            std::uint32_t fstr_num = 0, fstr_max = 0;
            if (!seh_read_u64(q + 0x00, &fstr_data) || !fstr_data) continue;
            if (fstr_data != help_wstr_addr) continue;
            if (!seh_read_u32(q + 0x08, &fstr_num))  continue;
            if (!seh_read_u32(q + 0x0C, &fstr_max))  continue;
            if (fstr_num == 0 || fstr_num > 1024)    continue;

            if (g_fstring_patch_count >= MAX_FSTRING_PATCHES) continue;
            g_fstring_patches[g_fstring_patch_count] = {
                q, fstr_data, fstr_num, fstr_max };
            ++g_fstring_patch_count;

            // Rewrite FString fields. Bump Max only if the new string
            // is longer than the old slack so the renderer doesn't
            // truncate; otherwise keep the original Max.
            std::uint32_t new_max = (new_len_inc_nul > fstr_max)
                                    ? new_len_inc_nul
                                    : fstr_max;
            DWORD oldp = 0;
            VirtualProtect(reinterpret_cast<void*>(q), 16,
                           PAGE_READWRITE, &oldp);
            *reinterpret_cast<std::uint64_t*>(q + 0x00) =
                reinterpret_cast<std::uint64_t>(g_label_new_buffer);
            *reinterpret_cast<std::uint32_t*>(q + 0x08) = new_len_inc_nul;
            *reinterpret_cast<std::uint32_t*>(q + 0x0C) = new_max;
            if (oldp) VirtualProtect(reinterpret_cast<void*>(q), 16,
                                     oldp, &oldp);
            OPENDOJO_LOG("patch-label: fstring-ref widget @0x%llX +0x%X -> "
                         "fstr@0x%llX {data:0x%llX->%p num:%u->%u max:%u->%u}",
                         static_cast<unsigned long long>(w), off,
                         static_cast<unsigned long long>(q),
                         static_cast<unsigned long long>(fstr_data),
                         g_label_new_buffer,
                         fstr_num, new_len_inc_nul,
                         fstr_max, new_max);
            ++fstring_refs;
        }
    }

    OPENDOJO_LOG("patch-label: deep pass  direct_refs=%d  fstring_refs=%d",
                 direct_refs, fstring_refs);
    return patched;
}

// Restore every label patch we've recorded. Idempotent; clears the record.
void unpatch_menu_row_labels() {
    int restored_198 = 0;
    for (int i = 0; i < g_label_patch_count; ++i) {
        auto& p = g_label_patches[i];
        if (!p.widget) continue;
        DWORD old_prot = 0;
        VirtualProtect(reinterpret_cast<void*>(p.widget + 0x198), 0x30,
                       PAGE_READWRITE, &old_prot);
        *reinterpret_cast<std::uint64_t*>(p.widget + 0x198) = p.orig_198;
        *reinterpret_cast<std::uint64_t*>(p.widget + 0x1C0) = p.orig_1C0;
        if (old_prot) {
            VirtualProtect(reinterpret_cast<void*>(p.widget + 0x198), 0x30,
                           old_prot, &old_prot);
        }
        ++restored_198;
    }
    int restored_fstr = 0;
    for (int i = 0; i < g_fstring_patch_count; ++i) {
        auto& p = g_fstring_patches[i];
        if (!p.fstring_addr) continue;
        DWORD oldp = 0;
        VirtualProtect(reinterpret_cast<void*>(p.fstring_addr), 16,
                       PAGE_READWRITE, &oldp);
        *reinterpret_cast<std::uint64_t*>(p.fstring_addr + 0x00) = p.orig_data;
        *reinterpret_cast<std::uint32_t*>(p.fstring_addr + 0x08) = p.orig_num;
        *reinterpret_cast<std::uint32_t*>(p.fstring_addr + 0x0C) = p.orig_max;
        if (oldp) VirtualProtect(reinterpret_cast<void*>(p.fstring_addr),
                                 16, oldp, &oldp);
        ++restored_fstr;
    }
    OPENDOJO_LOG("patch-label: restored %d direct + %d fstring patch(es)",
                 restored_198, restored_fstr);
    g_label_patch_count   = 0;
    g_fstring_patch_count = 0;
}

// Walk a UPanelWidget-shaped widget's Slots array, log each slot + its
// Content child. UPanelSlot::Content is at slot+0x30 in stock UMG.
// Useful once the subtree dump points us at the row container.
void dump_panel_slots(std::uint64_t panel, const char* tag) {
    char panel_cls[64] = {};
    decode_uobj_class_name(panel, panel_cls, sizeof(panel_cls));

    std::uint64_t data = 0;
    std::uint32_t num  = 0;
    seh_read_u64(panel + 0x150, &data);
    seh_read_u32(panel + 0x158, &num);
    OPENDOJO_LOG("tree: %s panel @0x%llX (%s) Slots data=0x%llX num=%u",
                 tag,
                 static_cast<unsigned long long>(panel),
                 panel_cls,
                 static_cast<unsigned long long>(data),
                 num);
    if (!data || num == 0 || num > 128) return;
    for (std::uint32_t i = 0; i < num; ++i) {
        std::uint64_t slot = 0;
        if (!seh_read_u64(data + i * 8, &slot) || !slot) continue;
        std::uint64_t content = 0;
        seh_read_u64(slot + 0x30, &content);
        char slot_cls[64]    = {};
        char content_cls[64] = {};
        decode_uobj_class_name(slot,    slot_cls,    sizeof(slot_cls));
        decode_uobj_class_name(content, content_cls, sizeof(content_cls));
        OPENDOJO_LOG("  slot[%u] @0x%llX (%s)  content @0x%llX (%s)",
                     i,
                     static_cast<unsigned long long>(slot),    slot_cls,
                     static_cast<unsigned long long>(content), content_cls);
    }
}

// Enumerate every live UWidget; log the ones whose Outer chain reaches
// the practice menu widget. Depth is the hop count when we land on the
// menu (1 = direct Outer, 2 = grandchild, ...). Output is unordered, but
// every entry tags its depth so the tree shape is recoverable by sorting
// on (depth, address). Read-only — pure diagnostic.
void dump_practice_menu_widget_subtree() {
    void* menu = get_practice_menu_outer_widget();
    if (!menu) return;
    auto menu_addr = reinterpret_cast<std::uint64_t>(menu);
    OPENDOJO_LOG("tree: practice menu widget @0x%llX",
                 static_cast<unsigned long long>(menu_addr));

    if (!g_resolved.find_objects_of_class || !g_resolved.find_class) {
        OPENDOJO_LOG("tree: resolver not initialised");
        return;
    }
    auto* widget_cls = g_resolved.find_class(
        nullptr, L"/Script/UMG.Widget", true);
    if (!widget_cls) {
        OPENDOJO_LOG("tree: /Script/UMG.Widget class not found");
        return;
    }

    UE_TArray results{};
    __try {
        g_resolved.find_objects_of_class(
            widget_cls, &results,
            /*include_derived=*/true,
            /*exclude_object_flags=*/0x30,  // skip CDO + Archetype
            /*exclude_internal_flags=*/0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("tree: SEH in GetObjectsOfClass(UWidget)");
        return;
    }
    if (!results.data || results.num <= 0) {
        OPENDOJO_LOG("tree: no live UWidget instances");
        return;
    }
    OPENDOJO_LOG("tree: scanning %d live UWidgets for Outer-chain reach",
                 results.num);

    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    int matched = 0;

    // Track first-seen address per "interesting" class so we dump exactly
    // one specimen of each (instead of dumping all 17 buttons). Keyed by
    // class pointer — comparing UClass* is cheaper than strcmp on names.
    constexpr int MAX_DEEP = 6;
    std::uint64_t dumped_classes[MAX_DEEP] = {};
    int dumped_count = 0;

    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t w = 0;
        if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &w)
            || !w) continue;
        // Walk Outer chain up to 10 hops looking for the menu widget.
        int depth = -1;
        std::uint64_t cur = w;
        for (int hop = 1; hop <= 10; ++hop) {
            std::uint64_t outer = 0;
            if (!seh_read_u64(cur + 0x20, &outer) || !outer) break;
            if (outer == menu_addr) { depth = hop; break; }
            cur = outer;
        }
        if (depth < 0) continue;

        char cls_name[64] = {}, obj_name[64] = {};
        decode_uobj_class_name(w, cls_name, sizeof(cls_name));
        std::uint32_t name_idx = 0;
        seh_read_u32(w + 0x18, &name_idx);
        decode_fname(name_idx, obj_name, sizeof(obj_name));

        std::uint32_t slot_num = 0;
        bool panelish = read_panel_slot_count(w, &slot_num);

        if (panelish) {
            OPENDOJO_LOG("tree: d=%d  %-32s  %-34s  @0x%llX  Slots=%u",
                         depth, obj_name, cls_name,
                         static_cast<unsigned long long>(w),
                         slot_num);
        } else {
            OPENDOJO_LOG("tree: d=%d  %-32s  %-34s  @0x%llX",
                         depth, obj_name, cls_name,
                         static_cast<unsigned long long>(w));
        }
        // FText + bare-wide-string scans. We don't yet know the canonical
        // text offset for PolarisTextBlock so both heuristics fire — sparse
        // output, since they only log on a confirmed match.
        scan_widget_for_ftext(w, 0x200);
        scan_widget_for_wide_strings(w, 0x300);

        // For each interesting class we encounter, dump exactly one
        // specimen's first 0x200 bytes so we can manually identify the
        // FText / FString offsets the heuristic missed. Targeted at
        // text-bearing classes (Polaris*TextBlock, *Button*).
        if (dumped_count < MAX_DEEP) {
            bool is_text  = std::strstr(cls_name, "TextBlock") != nullptr;
            bool is_btn   = std::strstr(cls_name, "Button")    != nullptr;
            if (is_text || is_btn) {
                std::uint64_t cls = 0;
                seh_read_u64(w + UOBJECT_CLASS_OFF, &cls);
                bool seen = false;
                for (int k = 0; k < dumped_count; ++k) {
                    if (dumped_classes[k] == cls) { seen = true; break; }
                }
                if (!seen) {
                    dumped_classes[dumped_count++] = cls;
                    char tag[96];
                    std::snprintf(tag, sizeof(tag),
                                  "tree: SPECIMEN[%s @0x%llX]",
                                  cls_name,
                                  static_cast<unsigned long long>(w));
                    dump_object_memory(reinterpret_cast<void*>(w), 0x200, tag);
                }
            }
        }
        ++matched;
    }
    OPENDOJO_LOG("tree: total widgets reachable from menu = %d", matched);
}

// Dump the 10 fn-pointers + vtable + receiver of the outer widget's
// delegate slots. Pure diagnostic — no writes.
void dump_practice_menu_delegate_slots() {
    void* widget = get_practice_menu_outer_widget();
    if (!widget) return;
    auto pol_base = memory::polaris_base();
    auto base_addr = reinterpret_cast<std::uintptr_t>(widget);
    OPENDOJO_LOG("probe: outer widget = 0x%llX",
                 static_cast<unsigned long long>(base_addr));
    for (int i = 0; i < WIDGET_DELEGATE_SLOT_COUNT; ++i) {
        auto slot_off = WIDGET_DELEGATE_SLOT_BASE
                      + static_cast<std::ptrdiff_t>(i)
                            * WIDGET_DELEGATE_SLOT_STRIDE;
        auto slot_addr = base_addr + slot_off;
        std::uint64_t fn_ptr = 0, vtable = 0, recv = 0;
        seh_read_u64(slot_addr + 0x00, &fn_ptr);
        seh_read_u64(slot_addr + 0x20, &vtable);
        seh_read_u64(slot_addr + 0x28, &recv);
        unsigned long long fn_rva = 0;
        if (pol_base && fn_ptr > pol_base) {
            fn_rva = static_cast<unsigned long long>(fn_ptr - pol_base);
        }
        unsigned long long vt_rva = 0;
        if (pol_base && vtable > pol_base) {
            vt_rva = static_cast<unsigned long long>(vtable - pol_base);
        }
        OPENDOJO_LOG("  slot %d (widget+0x%03llX): fn=0x%llX (rva=0x%llX) "
                     "vtbl=0x%llX (rva=0x%llX) recv=0x%llX",
                     i,
                     static_cast<unsigned long long>(slot_off),
                     static_cast<unsigned long long>(fn_ptr),
                     fn_rva,
                     static_cast<unsigned long long>(vtable),
                     vt_rva,
                     static_cast<unsigned long long>(recv));
    }

    // The receiver is the manager class instance — same across all 10
    // slots in practice. Dump the head of it so we can identify which
    // class it is (vtable RVA), and see its state fields. The body of
    // the slot 3 callback reads fields at +0x68, +0x6c, +0xac, +0xbc on
    // this object.
    std::uint64_t mgr = 0;
    seh_read_u64(base_addr + WIDGET_DELEGATE_SLOT_BASE + 0x28, &mgr);
    if (mgr) {
        OPENDOJO_LOG("probe: manager (slot 0 recv) = 0x%llX — first 0x100 bytes:",
                     static_cast<unsigned long long>(mgr));
        for (int off = 0; off < 0x100; off += 0x20) {
            std::uint64_t q[4] = {};
            for (int k = 0; k < 4; ++k) {
                seh_read_u64(mgr + off + k * 8, &q[k]);
            }
            unsigned long long vt_rva0 = 0;
            if (off == 0 && pol_base && q[0] > pol_base) {
                vt_rva0 = static_cast<unsigned long long>(q[0] - pol_base);
            }
            OPENDOJO_LOG("  mgr+0x%02X: %016llX %016llX %016llX %016llX%s",
                         off,
                         static_cast<unsigned long long>(q[0]),
                         static_cast<unsigned long long>(q[1]),
                         static_cast<unsigned long long>(q[2]),
                         static_cast<unsigned long long>(q[3]),
                         vt_rva0 ? (" (vtbl rva=0x" ", look up in Ghidra)") : "");
            (void)vt_rva0;
        }
        // Also log the specific fields used by slot 3 body.
        std::uint32_t f68 = 0, f6c = 0, fbc = 0;
        std::uint8_t fac = 0;
        seh_read_u32(mgr + 0x68, &f68);
        seh_read_u32(mgr + 0x6C, &f6c);
        seh_read_u8(mgr + 0xAC, &fac);
        seh_read_u32(mgr + 0xBC, &fbc);
        OPENDOJO_LOG("  mgr state: +0x68(mode)=%u +0x6C(sub)=%u "
                     "+0xAC(flag)=%u +0xBC(cached_info_idx)=%u",
                     f68, f6c, fac, fbc);
    }

    // Walk the widget's WidgetTree (at +0x228) — its children are the
    // panels/items that make up the menu. Knowing what classes the
    // children are will tell us where the left-list items live.
    std::uint64_t widget_tree = 0;
    std::uint32_t child_count = 0;
    seh_read_u64(base_addr + 0x228, &widget_tree);
    seh_read_u32(base_addr + 0x230, &child_count);
    OPENDOJO_LOG("probe: WidgetTree=0x%llX child_count=%u",
                 static_cast<unsigned long long>(widget_tree),
                 child_count);
    if (widget_tree && child_count > 0 && child_count < 64) {
        std::uint64_t children_arr = 0;
        seh_read_u64(widget_tree + 0x40, &children_arr);
        if (children_arr) {
            for (std::uint32_t i = 0; i < child_count; ++i) {
                std::uint64_t child = 0;
                seh_read_u64(children_arr + i * 8, &child);
                if (!child) continue;
                std::uint64_t child_cls = 0;
                seh_read_u64(child + UOBJECT_CLASS_OFF, &child_cls);
                std::uint32_t cls_idx = 0;
                if (child_cls) {
                    seh_read_u32(child_cls + 0x18, &cls_idx);
                }
                char name[128] = {};
                if (cls_idx) decode_fname(cls_idx, name, sizeof(name));
                OPENDOJO_LOG("  child[%u] @0x%llX class='%s'",
                             i,
                             static_cast<unsigned long long>(child),
                             name);
            }
        }
    }
}

// Overwrite each slot's fn pointer with our matching probe_stub<N>.
// Originals are stashed in g_probe so the stubs can forward; menu stays
// fully functional, just logs every callback into the manager.
void hook_all_practice_menu_delegate_slots() {
    void* widget = get_practice_menu_outer_widget();
    if (!widget) return;
    if (g_probe.widget && g_probe.widget != widget) {
        OPENDOJO_LOG("probe: live widget changed since last hook "
                     "(stale=0x%llX -> new=0x%llX) — refusing to re-hook",
                     reinterpret_cast<unsigned long long>(g_probe.widget),
                     reinterpret_cast<unsigned long long>(widget));
        return;
    }
    auto base_addr = reinterpret_cast<std::uintptr_t>(widget);
    g_probe.widget = widget;
    for (int i = 0; i < WIDGET_DELEGATE_SLOT_COUNT; ++i) {
        if (g_probe.slot_hooked[i]) continue;
        auto slot_off = WIDGET_DELEGATE_SLOT_BASE
                      + static_cast<std::ptrdiff_t>(i)
                            * WIDGET_DELEGATE_SLOT_STRIDE;
        auto slot_addr = base_addr + slot_off;
        std::uint64_t original = 0;
        if (!seh_read_u64(slot_addr, &original) || !original) {
            OPENDOJO_LOG("probe: slot %d fn ptr null/unreadable — skipping", i);
            continue;
        }
        g_probe.original_fn[i] = static_cast<std::uintptr_t>(original);
        g_probe.calls_per_slot[i] = 0;
        DWORD oldp = 0;
        VirtualProtect(reinterpret_cast<void*>(slot_addr), 8,
                       PAGE_READWRITE, &oldp);
        *reinterpret_cast<volatile std::uintptr_t*>(slot_addr) =
            kProbeStubs[i];
        VirtualProtect(reinterpret_cast<void*>(slot_addr), 8, oldp, &oldp);
        g_probe.slot_hooked[i] = true;
        OPENDOJO_LOG("probe: hooked slot %d "
                     "(orig=0x%llX -> stub=0x%llX)",
                     i,
                     static_cast<unsigned long long>(original),
                     static_cast<unsigned long long>(kProbeStubs[i]));
    }
}

// Restore the original fn pointers we previously wrote in.
void unhook_all_practice_menu_delegate_slots() {
    if (!g_probe.widget) {
        OPENDOJO_LOG("probe: nothing to unhook");
        return;
    }
    auto base_addr = reinterpret_cast<std::uintptr_t>(g_probe.widget);
    int restored = 0;
    for (int i = 0; i < WIDGET_DELEGATE_SLOT_COUNT; ++i) {
        if (!g_probe.slot_hooked[i]) continue;
        auto slot_addr = base_addr + WIDGET_DELEGATE_SLOT_BASE
                       + static_cast<std::ptrdiff_t>(i)
                             * WIDGET_DELEGATE_SLOT_STRIDE;
        DWORD oldp = 0;
        VirtualProtect(reinterpret_cast<void*>(slot_addr), 8,
                       PAGE_READWRITE, &oldp);
        *reinterpret_cast<volatile std::uintptr_t*>(slot_addr) =
            g_probe.original_fn[i];
        VirtualProtect(reinterpret_cast<void*>(slot_addr), 8, oldp, &oldp);
        g_probe.slot_hooked[i] = false;
        OPENDOJO_LOG("probe: unhooked slot %d (calls during probe=%d)",
                     i, g_probe.calls_per_slot[i]);
        ++restored;
    }
    g_probe.widget = nullptr;
    OPENDOJO_LOG("probe: restored %d slot(s)", restored);
}

bool any_slot_hooked() {
    for (int i = 0; i < WIDGET_DELEGATE_SLOT_COUNT; ++i) {
        if (g_probe.slot_hooked[i]) return true;
    }
    return false;
}

// Invoke kamui::ui::PracticeMenuImpl::Open(world) — opens the game's
// own practice pause menu. Allocates the singleton if needed.
void invoke_game_practice_menu_open() {
    auto pol_base = memory::polaris_base();
    if (!pol_base) return;

    // 1. Read singleton pointer.
    std::uint64_t singleton = 0;
    seh_read_u64(pol_base + PRACTICE_MENU_SINGLETON_RVA, &singleton);
    OPENDOJO_LOG("native_menu: PracticeMenuImpl singleton @"
                 "(module+0x%llX) = 0x%llX",
                 static_cast<unsigned long long>(PRACTICE_MENU_SINGLETON_RVA),
                 static_cast<unsigned long long>(singleton));

    // 2. Factory-allocate if null. The factory both constructs and
    // writes the singleton, so we just call and re-read.
    if (!singleton) {
        OPENDOJO_LOG("native_menu: singleton null — calling factory");
        using FactoryFn = void* (*)();
        auto factory_fn = reinterpret_cast<FactoryFn>(
            pol_base + PRACTICE_MENU_FACTORY_RVA);
        void* factory_result = nullptr;
        __try {
            factory_result = factory_fn();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            OPENDOJO_LOG("native_menu: SEH in PracticeMenuImpl factory");
            return;
        }
        seh_read_u64(pol_base + PRACTICE_MENU_SINGLETON_RVA, &singleton);
        OPENDOJO_LOG("native_menu: factory returned 0x%llX, "
                     "singleton now 0x%llX",
                     reinterpret_cast<unsigned long long>(factory_result),
                     static_cast<unsigned long long>(singleton));
        if (!singleton) {
            OPENDOJO_LOG("native_menu: factory failed to populate singleton");
            return;
        }
    }

    // 3. Get a world context. The game gets this from the practice
    // controller's tick state. Easiest from our side: PC's world via
    // vtable[+0x188]. We already use this for the world-getter in
    // try_add_to_viewport.
    void* pc = find_player_controller_now();
    if (!pc) {
        OPENDOJO_LOG("native_menu: no PC for world context");
        return;
    }
    void* world_ctx = nullptr;
    {
        std::uint64_t vt = 0;
        if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(pc), &vt) || !vt) {
            OPENDOJO_LOG("native_menu: PC vtable unreadable");
            return;
        }
        std::uint64_t fn = 0;
        if (!seh_read_u64(vt + 0x188, &fn) || !fn) {
            OPENDOJO_LOG("native_menu: PC vtable[+0x188] missing");
            return;
        }
        __try {
            using WorldGetFn = void* (*)(void*);
            world_ctx = reinterpret_cast<WorldGetFn>(fn)(pc);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            OPENDOJO_LOG("native_menu: SEH getting world from PC");
            return;
        }
    }
    OPENDOJO_LOG("native_menu: world_ctx = 0x%llX",
                 reinterpret_cast<unsigned long long>(world_ctx));
    if (!world_ctx) {
        OPENDOJO_LOG("native_menu: world is null — aborting");
        return;
    }

    // 4. Call PracticeMenuImpl::Open(this, world).
    using OpenFn = void (*)(void* manager, void* world_ctx);
    auto open_fn = reinterpret_cast<OpenFn>(pol_base + PRACTICE_MENU_OPEN_RVA);
    OPENDOJO_LOG("native_menu: calling PracticeMenuImpl::Open(0x%llX, 0x%llX)",
                 static_cast<unsigned long long>(singleton),
                 reinterpret_cast<unsigned long long>(world_ctx));
    __try {
        open_fn(reinterpret_cast<void*>(singleton), world_ctx);
        OPENDOJO_LOG("native_menu: PracticeMenuImpl::Open returned");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("native_menu: SEH in PracticeMenuImpl::Open");
    }
}

void show() {
    if (g_visible.exchange(true, std::memory_order_relaxed)) return;
    if (!ensure_resolved()) {
        OPENDOJO_LOG("native_menu: show() — patterns not resolved");
        return;
    }

    // Invoke the game's own practice menu open path. If successful,
    // the game's practice pause menu will appear on screen.
    invoke_game_practice_menu_open();
    return;
}

void show_OLD_BP_WIDGET_CONSTRUCTION() {
    if (!ensure_resolved()) {
        OPENDOJO_LOG("native_menu: show() — patterns not resolved");
        return;
    }

    // Construct the widget once, reuse on subsequent shows.
    if (!g_resolved.cached_widget) {
        // Target the OUTER WBP_UI_PracticeMenu_C class — the top-level
        // widget the game itself adds to viewport. WBP_UI_PracticeMenu_Menu_3_C
        // is a sub-widget composed via the outer's WidgetTree and won't
        // render standalone.
        void* use_class = find_widget_class_by_name("WBP_UI_PracticeMenu_C");
        if (!use_class) {
            OPENDOJO_LOG("native_menu: outer practice menu BP not loaded — "
                         "trying inner as fallback");
            use_class = find_widget_class_by_name(
                "WBP_UI_PracticeMenu_Menu_3_C");
        }
        if (!use_class) {
            OPENDOJO_LOG("native_menu: no practice menu class loaded yet");
            return;
        }

        void* pc = find_player_controller_now();
        if (!pc) {
            OPENDOJO_LOG("native_menu: show() — no PC available");
            return;
        }

        // Use the canonical CreateWidget variant the game itself uses
        // for the practice menu open path. Returns the widget pointer in
        // RAX (Ghidra shows void return but RAX carries the result).
        auto pol_base = memory::polaris_base();
        if (!pol_base) return;
        using CreateWidgetV2Fn = void* (*)(void*, void*, std::uint64_t);
        auto create_fn = reinterpret_cast<CreateWidgetV2Fn>(
            pol_base + CREATE_WIDGET_V2_RVA);

        std::int32_t before_count =
            g_resolved.guobj_count ? *g_resolved.guobj_count : -1;
        void* widget = nullptr;
        __try {
            widget = create_fn(pc, use_class, 0ULL);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            OPENDOJO_LOG("native_menu: SEH in canonical CreateWidget");
            return;
        }
        std::int32_t after_count =
            g_resolved.guobj_count ? *g_resolved.guobj_count : -1;
        OPENDOJO_LOG("native_menu: canonical CreateWidget returned 0x%llX  "
                     "(GUObjectArray %d -> %d, delta=%d)",
                     reinterpret_cast<unsigned long long>(widget),
                     before_count, after_count, after_count - before_count);

        if (!widget) {
            OPENDOJO_LOG("native_menu: CreateWidget v2 returned null");
            return;
        }

        std::uint64_t w_class = 0;
        seh_read_u64(reinterpret_cast<std::uintptr_t>(widget)
                     + UOBJECT_CLASS_OFF, &w_class);
        OPENDOJO_LOG("native_menu: widget @0x%llX class=0x%llX",
                     reinterpret_cast<unsigned long long>(widget),
                     static_cast<unsigned long long>(w_class));
        g_resolved.cached_widget = widget;
    } else {
        OPENDOJO_LOG("native_menu: show() — reusing cached widget @0x%llX",
                     reinterpret_cast<unsigned long long>(g_resolved.cached_widget));
    }

    // Canonical AddToViewport with mode/ZOrder = 0x80.
    {
        auto pol_base = memory::polaris_base();
        if (!pol_base) return;
        using AddToViewportWrapperFn = void (*)(void*, std::int32_t);
        auto add_fn = reinterpret_cast<AddToViewportWrapperFn>(
            pol_base + ADD_TO_VIEWPORT_WRAPPER_RVA);
        __try {
            add_fn(g_resolved.cached_widget, 0x80);
            OPENDOJO_LOG("native_menu: canonical AddToViewport(mode=0x80) "
                         "completed");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            OPENDOJO_LOG("native_menu: SEH in canonical AddToViewport");
            return;
        }
    }

    // Game sets two flag bytes in the widget after AddToViewport (per
    // FUN_145db2a30): widget[+0x280] = 0 and widget[+0x282] = 1.
    // Their semantic is unconfirmed (likely input mode flags), but the
    // game does this every time, so we mirror.
    {
        auto* wb = reinterpret_cast<std::uint8_t*>(g_resolved.cached_widget);
        __try {
            wb[0x280] = 0;
            wb[0x282] = 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            OPENDOJO_LOG("native_menu: SEH setting input flag bytes");
        }
    }

    // Canonical SetVisibility(widget, 0) — makes the widget actually visible.
    {
        auto pol_base = memory::polaris_base();
        if (!pol_base) return;
        using SetVisibilityFn = void (*)(void*, char);
        auto vis_fn = reinterpret_cast<SetVisibilityFn>(
            pol_base + SET_VISIBILITY_RVA);
        __try {
            vis_fn(g_resolved.cached_widget, 0);
            OPENDOJO_LOG("native_menu: SetVisibility(0) completed");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            OPENDOJO_LOG("native_menu: SEH in SetVisibility");
        }
    }

    // Final memory dump for comparison with live[2] from the discovery
    // pass above.
    dump_object_memory(g_resolved.cached_widget, 0x300,
                       "OUR widget (post canonical sequence)");
}

void hide() {
    if (!g_visible.exchange(false, std::memory_order_relaxed)) return;

    // For now (while we're cycling subclasses to find a visible one), drop
    // the cached widget so the next show() reconstructs from a different
    // BP class. Once we lock onto the right class this should switch to
    // RemoveFromParent (keep widget alive across show/hide cycles).
    g_resolved.cached_widget = nullptr;
    g_subclass_cursor++;
    OPENDOJO_LOG("native_menu: hide() — cleared cached widget, "
                 "subclass cursor -> %d", g_subclass_cursor);
}

void toggle() {
    if (is_visible()) hide(); else show();
}

// F11 drives the delegate-probe cycle while we map which slot handles
// which user action. State machine: each press advances by one.
//   0 -> dump the live widget's 10 slots, then advance
//   1 -> hook all 10 slots with forwarding stubs
//   2 -> unhook everything
// After unhook the cycle returns to 0 so we can re-dump if needed.
enum class ProbeState : int { kDump = 0, kHook = 1, kUnhook = 2 };
int g_probe_state = 0;

void advance_probe() {
    auto state = static_cast<ProbeState>(g_probe_state % 3);
    switch (state) {
    case ProbeState::kDump:
        OPENDOJO_LOG("probe: F11 #%d — DUMP", g_probe_state + 1);
        dump_practice_menu_delegate_slots();
        break;
    case ProbeState::kHook:
        OPENDOJO_LOG("probe: F11 #%d — HOOK ALL", g_probe_state + 1);
        hook_all_practice_menu_delegate_slots();
        break;
    case ProbeState::kUnhook:
        OPENDOJO_LOG("probe: F11 #%d — UNHOOK", g_probe_state + 1);
        unhook_all_practice_menu_delegate_slots();
        break;
    }
    g_probe_state++;
}

void tick() {
    // Resolve on first tick (cheap once cached).
    ensure_resolved();

    // F11 edge-trigger drives the delegate probe (see advance_probe).
    // GetAsyncKeyState's high bit is the down state; we want the leading
    // edge so each press fires exactly once.
    bool now = (GetAsyncKeyState(HOTKEY_VK) & 0x8000) != 0;
    if (now && !g_last_hotkey_state) {
        advance_probe();
    }
    g_last_hotkey_state = now;

    // F10 edge-trigger: dump the practice menu's widget subtree.
    bool now_tree = (GetAsyncKeyState(HOTKEY_TREE_VK) & 0x8000) != 0;
    if (now_tree && !g_last_tree_hotkey_state) {
        OPENDOJO_LOG("tree: F10 — DUMP WIDGET SUBTREE");
        dump_practice_menu_widget_subtree();
    }
    g_last_tree_hotkey_state = now_tree;

    // F9 edge-trigger: toggle "Help" -> "OpenDojo" label patch.
    bool now_patch = (GetAsyncKeyState(HOTKEY_PATCH_VK) & 0x8000) != 0;
    if (now_patch && !g_last_patch_hotkey_state) {
        if (!g_label_patched) {
            OPENDOJO_LOG("patch-label: F9 — APPLY ('Help' -> 'OpenDojo')");
            int n = patch_menu_row_label("Help", L"OpenDojo");
            g_label_patched = (n > 0);
        } else {
            OPENDOJO_LOG("patch-label: F9 — REVERT");
            unpatch_menu_row_labels();
            g_label_patched = false;
        }
    }
    g_last_patch_hotkey_state = now_patch;
}

}  // namespace opendojo::native_menu
