#include "practice_menu.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "MinHook.h"

#include "log.hpp"
#include "memory.hpp"
#include "render_hook.hpp"

// Decide-event dispatcher (from Ghidra static analysis at 0x142e5ef30):
//   void __fastcall FUN_142e5ef30(WBP_UI_PracticeMenu_C* self, uint32_t id);
// Called by InvokeDecideButton1Callback's exec stub on every row Decide.
// It reads widget+0x290 (the bound delegate ptr) and invokes it with id.
// Hooking here gives us (self, id) cleanly. Polaris-internal — no
// UE4SS conflict since UE4SS hooks UObject::ProcessEvent, a different
// address.
constexpr std::uintptr_t INVOKE_DECIDE_DISPATCH_RVA = 0x2E5EF30;

// =============================================================================
// PRACTICE-MENU INTEGRATION (DLL-native)
// =============================================================================

namespace opendojo::practice_menu {

namespace {

// ----- Pattern compile / scan / RIP-resolve / .text-range ---------------

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

std::uintptr_t rip_relative(std::uintptr_t at) {
    auto disp = static_cast<std::int32_t>(memory::read_u32(at));
    return at + 4 + static_cast<std::uintptr_t>(static_cast<std::int64_t>(disp));
}

// ----- SEH-protected reads -----------------------------------------------

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
bool seh_read_u8(std::uintptr_t addr, std::uint8_t* out) {
    __try { std::memcpy(out, reinterpret_cast<const void*>(addr), 1); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ----- UE5 reflection primitives ----------------------------------------

constexpr const char* PAT_FIND_UNREAL_CLASS =
    "45 33 C0 49 8B CF E8 ?? ?? ?? ?? 48 8B 4C 24 60";
constexpr const char* PAT_FIND_OBJECTS_OF_CLASS =
    "E8 ?? ?? ?? ?? 90 48 89 6C 24 30";

using FindUnrealClassFn = void* (*)(void* outer, const wchar_t* name,
                                    bool exact_class);

struct UE_TArrayRaw {
    void*        data;
    std::int32_t num;
    std::int32_t max;
};

using FindObjectsOfClassFn = void (*)(void* class_to_look_for,
                                      UE_TArrayRaw* out,
                                      bool include_derived,
                                      std::uint32_t exclude_object_flags,
                                      std::uint32_t exclude_internal_flags);

// Documented at: project_tekken_processevent_primitives memory note.
constexpr int PROCESS_EVENT_VTABLE_SLOT = 77;
using ProcessEventFn = void (*)(void* self, void* function, void* parms);

// UE 5.2 layout offsets — verified empirically (see project_opendojo_v1_imgui_strategy memory).
constexpr std::ptrdiff_t UOBJECT_CLASS_OFF         = 0x10;
constexpr std::ptrdiff_t UOBJECT_NAME_OFF          = 0x18;
constexpr std::ptrdiff_t USTRUCT_SUPER_OFF         = 0x40;
constexpr std::ptrdiff_t USTRUCT_CHILDREN_OFF      = 0x48;
constexpr std::ptrdiff_t USTRUCT_CHILDPROPS_OFF    = 0x50;
constexpr std::ptrdiff_t USTRUCT_PROPERTY_LINK_OFF = 0x70;
constexpr std::ptrdiff_t UFIELD_NEXT_OFF           = 0x28;
constexpr std::ptrdiff_t FFIELD_NEXT_OFF           = 0x20;
constexpr std::ptrdiff_t FFIELD_NAME_OFF           = 0x28;
constexpr std::ptrdiff_t FPROPERTY_OFFSET_INT_OFF  = 0x4C;
constexpr std::ptrdiff_t FPROPERTY_PROPLINK_NEXT_OFF = 0x58;
constexpr std::ptrdiff_t UFUNCTION_FUNC_OFF        = 0xD8;  // native fn ptr

constexpr std::uint32_t RF_CLASS_DEFAULT_OBJECT = 0x10;
constexpr std::uint32_t RF_ARCHETYPE_OBJECT     = 0x20;

// FNamePool. From project_tekken_fname_pool memory.
constexpr std::uintptr_t FNAME_POOL_RVA     = 0x9955480;
constexpr std::ptrdiff_t POOL_BLOCKS_OFFSET = 0x10;
constexpr std::uint32_t  FNAME_BLOCK_MASK   = 0x1FFF;
constexpr std::uint32_t  FNAME_STRIDE_MASK  = 0xFFFF;
constexpr std::uint32_t  FNAME_BLOCK_SHIFT  = 16;

bool decode_fname(std::uint32_t idx, char* out_buf, std::size_t out_buf_size) {
    if (!out_buf || out_buf_size == 0) return false;
    out_buf[0] = '\0';
    if (idx == 0) {
        if (out_buf_size >= 5) std::memcpy(out_buf, "None", 5);
        return true;
    }
    auto base = memory::polaris_base();
    if (!base) return false;
    auto pool = base + FNAME_POOL_RVA;
    auto block_idx = (idx >> FNAME_BLOCK_SHIFT) & FNAME_BLOCK_MASK;
    auto stride    = idx & FNAME_STRIDE_MASK;
    std::uint64_t block_ptr = 0;
    if (!seh_read_u64(pool + POOL_BLOCKS_OFFSET + block_idx * 8, &block_ptr)
        || !block_ptr) return false;
    auto entry = static_cast<std::uintptr_t>(block_ptr)
               + static_cast<std::uintptr_t>(stride) * 2;
    std::uint16_t header = 0;
    if (!seh_read_u16(entry, &header)) return false;
    bool is_wide = (header & 1) != 0;
    std::uint32_t len = header >> 6;
    if (len == 0 || len > 1023) return false;
    std::size_t i;
    for (i = 0; i < len && i + 1 < out_buf_size; ++i) {
        if (is_wide) {
            std::uint16_t w = 0;
            if (!seh_read_u16(entry + 2 + i * 2, &w)) break;
            out_buf[i] = (w > 0x7F) ? '?' : static_cast<char>(w);
        } else {
            std::uint8_t b = 0;
            if (!seh_read_u8(entry + 2 + i, &b)) break;
            out_buf[i] = (b > 0x7F) ? '?' : static_cast<char>(b);
        }
    }
    out_buf[i] = '\0';
    return true;
}

// Walk a UStruct's Children chain (UFunction list) and return the
// UFunction matching `target`. Walks SuperStruct on miss.
void* find_ufunction_by_name(void* uclass, const char* target) {
    if (!uclass) return nullptr;
    auto cls = reinterpret_cast<std::uintptr_t>(uclass);
    for (int depth = 0; depth < 16 && cls; ++depth) {
        std::uint64_t child = 0;
        if (seh_read_u64(cls + USTRUCT_CHILDREN_OFF, &child) && child) {
            auto fn = static_cast<std::uintptr_t>(child);
            int hops = 0;
            char buf[256];
            while (fn && hops++ < 512) {
                std::uint32_t name_idx = 0;
                if (!seh_read_u32(fn + UOBJECT_NAME_OFF, &name_idx)) break;
                if (decode_fname(name_idx, buf, sizeof(buf))
                    && std::strcmp(buf, target) == 0) {
                    return reinterpret_cast<void*>(fn);
                }
                std::uint64_t nxt = 0;
                if (!seh_read_u64(fn + UFIELD_NEXT_OFF, &nxt)) break;
                fn = static_cast<std::uintptr_t>(nxt);
            }
        }
        std::uint64_t super = 0;
        if (!seh_read_u64(cls + USTRUCT_SUPER_OFF, &super)) break;
        cls = static_cast<std::uintptr_t>(super);
    }
    return nullptr;
}

// Walk an FField chain.
std::uintptr_t walk_field_chain_for_name(std::uintptr_t head,
                                         std::ptrdiff_t next_off,
                                         const char* target) {
    auto f = head;
    int hops = 0;
    char buf[256];
    while (f && hops++ < 2048) {
        std::uint32_t name_idx = 0;
        if (!seh_read_u32(f + FFIELD_NAME_OFF, &name_idx)) break;
        if (decode_fname(name_idx, buf, sizeof(buf))
            && std::strcmp(buf, target) == 0) {
            return f;
        }
        std::uint64_t nxt = 0;
        if (!seh_read_u64(f + next_off, &nxt)) break;
        f = static_cast<std::uintptr_t>(nxt);
    }
    return 0;
}

// Find an FProperty offset. Tries ChildProperties+SuperStruct, then
// PropertyLink. Returns -1 on miss.
std::int32_t find_fproperty_offset(void* uclass, const char* target) {
    if (!uclass) return -1;
    auto cls = reinterpret_cast<std::uintptr_t>(uclass);
    for (int depth = 0; depth < 16 && cls; ++depth) {
        std::uint64_t head = 0;
        if (seh_read_u64(cls + USTRUCT_CHILDPROPS_OFF, &head) && head) {
            auto hit = walk_field_chain_for_name(
                static_cast<std::uintptr_t>(head), FFIELD_NEXT_OFF, target);
            if (hit) {
                std::uint32_t off = 0;
                if (seh_read_u32(hit + FPROPERTY_OFFSET_INT_OFF, &off)) {
                    return static_cast<std::int32_t>(off);
                }
            }
        }
        std::uint64_t super = 0;
        if (!seh_read_u64(cls + USTRUCT_SUPER_OFF, &super)) break;
        cls = static_cast<std::uintptr_t>(super);
    }
    cls = reinterpret_cast<std::uintptr_t>(uclass);
    std::uint64_t plink = 0;
    if (seh_read_u64(cls + USTRUCT_PROPERTY_LINK_OFF, &plink) && plink) {
        auto hit = walk_field_chain_for_name(
            static_cast<std::uintptr_t>(plink),
            FPROPERTY_PROPLINK_NEXT_OFF, target);
        if (hit) {
            std::uint32_t off = 0;
            if (seh_read_u32(hit + FPROPERTY_OFFSET_INT_OFF, &off)) {
                return static_cast<std::int32_t>(off);
            }
        }
    }
    return -1;
}

// SEH wrapper: __try can't sit in a function that needs C++ object
// unwinding (std::vector).
bool seh_call_find_objects(FindObjectsOfClassFn fn, void* cls,
                           std::uint32_t exclude_flags,
                           UE_TArrayRaw* out) {
    __try {
        fn(cls, out, /*include_derived=*/true,
           exclude_flags, /*exclude_internal_flags=*/0);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* find_first_live_object_of_class(FindObjectsOfClassFn fn, void* cls) {
    if (!fn || !cls) return nullptr;
    UE_TArrayRaw results{};
    if (!seh_call_find_objects(fn, cls,
            RF_CLASS_DEFAULT_OBJECT | RF_ARCHETYPE_OBJECT, &results)) {
        return nullptr;
    }
    if (!results.data || results.num <= 0) return nullptr;
    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    std::uint64_t obj = 0;
    seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[0]), &obj);
    return reinterpret_cast<void*>(obj);
}

std::int32_t count_live_objects_of_class(FindObjectsOfClassFn fn, void* cls) {
    if (!fn || !cls) return -1;
    UE_TArrayRaw results{};
    if (!seh_call_find_objects(fn, cls,
            RF_CLASS_DEFAULT_OBJECT | RF_ARCHETYPE_OBJECT, &results)) {
        return -1;
    }
    return results.num;
}

// Find a UClass by short name. Scans UUserWidget-derived classes (or
// derivatives of `base_cls` if non-null).
void* find_class_by_name(FindUnrealClassFn fc, FindObjectsOfClassFn fo,
                         void* base_cls, const char* target_name) {
    if (!fc || !fo || !base_cls || !target_name) return nullptr;
    UE_TArrayRaw results{};
    if (!seh_call_find_objects(fo, base_cls,
            /*exclude_object_flags=*/0,  // INCLUDE CDOs so we get class ptrs
            &results)) return nullptr;
    if (!results.data || results.num <= 0) return nullptr;

    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    auto target_len = std::strlen(target_name);
    char name_buf[128];
    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t obj = 0;
        if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &obj) || !obj) continue;
        std::uint64_t cls = 0;
        if (!seh_read_u64(obj + UOBJECT_CLASS_OFF, &cls) || !cls) continue;
        std::uint32_t cls_idx = 0;
        if (!seh_read_u32(cls + UOBJECT_NAME_OFF, &cls_idx)) continue;
        if (!decode_fname(cls_idx, name_buf, sizeof(name_buf))) continue;
        if (std::strncmp(name_buf, target_name, target_len) == 0
            && name_buf[target_len] == '\0') {
            return reinterpret_cast<void*>(cls);
        }
    }
    return nullptr;
}

std::vector<void*> find_all_live_objects_of_class(FindObjectsOfClassFn fn,
                                                   void* cls) {
    std::vector<void*> out;
    if (!fn || !cls) return out;
    UE_TArrayRaw results{};
    if (!seh_call_find_objects(fn, cls,
            RF_CLASS_DEFAULT_OBJECT | RF_ARCHETYPE_OBJECT, &results)) {
        return out;
    }
    if (!results.data || results.num <= 0) return out;
    auto* arr = reinterpret_cast<std::uint64_t*>(results.data);
    out.reserve(results.num);
    for (std::int32_t i = 0; i < results.num; ++i) {
        std::uint64_t obj = 0;
        if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(&arr[i]), &obj) || !obj) continue;
        out.push_back(reinterpret_cast<void*>(obj));
    }
    return out;
}

ProcessEventFn pe_from_self(void* self) {
    auto vtable = *reinterpret_cast<void***>(self);
    return reinterpret_cast<ProcessEventFn>(vtable[PROCESS_EVENT_VTABLE_SLOT]);
}

// ----- FString helper -----------------------------------------------------

struct UE_FString {
    wchar_t*     data = nullptr;
    std::int32_t num  = 0;   // includes NUL
    std::int32_t max  = 0;
};

void make_fstring_leaky(UE_FString& out, const wchar_t* text) {
    if (!text) text = L"";
    std::size_t n = std::wcslen(text) + 1;
    auto* buf = static_cast<wchar_t*>(std::malloc(n * sizeof(wchar_t)));
    if (!buf) { out = {}; return; }
    std::memcpy(buf, text, n * sizeof(wchar_t));
    out.data = buf;
    out.num  = static_cast<std::int32_t>(n);
    out.max  = static_cast<std::int32_t>(n);
}

bool fstring_equals_ascii(std::uintptr_t addr, const char* target) {
    std::uint64_t data = 0;
    std::uint32_t num  = 0;
    if (!seh_read_u64(addr, &data) || !data) return false;
    if (!seh_read_u32(addr + 8, &num)) return false;
    if (num <= 0) return false;
    auto len = std::strlen(target);
    if (static_cast<std::size_t>(num) != len + 1) return false;
    for (std::size_t i = 0; i < len; ++i) {
        std::uint16_t ch = 0;
        if (!seh_read_u16(static_cast<std::uintptr_t>(data) + i * 2, &ch)) return false;
        if (ch != static_cast<std::uint16_t>(static_cast<unsigned char>(target[i]))) return false;
    }
    return true;
}

// =============================================================================
// Resolution state.
// =============================================================================

struct Resolved {
    FindUnrealClassFn      find_class            = nullptr;
    FindObjectsOfClassFn   find_objects_of_class = nullptr;

    void* cls_user_widget    = nullptr;   // /Script/UMG.UserWidget
    void* cls_text_block     = nullptr;   // /Script/Polaris.PolarisTextBlock
    void* cls_practice_menu  = nullptr;   // WBP_UI_PracticeMenu_C
    void* cls_button_row     = nullptr;   // WBP_UI_PracticeMenu_Button_1_C
    void* cls_item           = nullptr;   // BP_PracticeMenu_Button_1_Item_C (discovered lazily)

    void* ufn_add_button1    = nullptr;
    void* ufn_update_lv1     = nullptr;
    void* ufn_on_decide      = nullptr;
    void* ufn_set_raw_text   = nullptr;
    void* ufn_set_text_id    = nullptr;   // UPolarisTextBlock::SetTextID

    std::int32_t off_list_item   = -1;
    std::int32_t off_item_text   = -1;
    std::int32_t off_tb_menu_off = -1;
    std::int32_t off_tb_menu_on  = -1;

    bool engine_ok   = false;
    bool bp_full_ok  = false;
};

Resolved       g_r;
std::once_flag g_resolve_once;

void do_resolve() {
    std::uintptr_t ts, sz;
    if (!get_text_range(ts, sz)) {
        OPENDOJO_LOG("practice_menu: .text range unavailable");
        return;
    }
    CompiledPattern p1, p2;
    if (!compile_pattern(PAT_FIND_UNREAL_CLASS, p1)) return;
    if (!compile_pattern(PAT_FIND_OBJECTS_OF_CLASS, p2)) return;
    auto h1 = scan(p1, ts, sz);
    auto h2 = scan(p2, ts, sz);
    if (!h1 || !h2) {
        OPENDOJO_LOG("practice_menu: pattern scan miss (fuc=%d foc=%d)",
                     h1 ? 1 : 0, h2 ? 1 : 0);
        return;
    }
    g_r.find_class            = reinterpret_cast<FindUnrealClassFn>(rip_relative(h1 + 7));
    g_r.find_objects_of_class = reinterpret_cast<FindObjectsOfClassFn>(rip_relative(h2 + 1));

    g_r.cls_user_widget = g_r.find_class(nullptr, L"/Script/UMG.UserWidget", true);
    g_r.cls_text_block  = g_r.find_class(nullptr, L"/Script/Polaris.PolarisTextBlock", true);
    g_r.ufn_set_raw_text = find_ufunction_by_name(g_r.cls_text_block, "SetRawText");
    g_r.ufn_set_text_id  = find_ufunction_by_name(g_r.cls_text_block, "SetTextID");

    g_r.engine_ok = g_r.find_class && g_r.find_objects_of_class
                 && g_r.cls_user_widget && g_r.cls_text_block
                 && g_r.ufn_set_raw_text && g_r.ufn_set_text_id;

    OPENDOJO_LOG("practice_menu: engine resolve %s — find_class=0x%llX "
                 "find_objects=0x%llX uw_cls=0x%llX tb_cls=0x%llX "
                 "set_raw_text=0x%llX set_text_id=0x%llX",
                 g_r.engine_ok ? "OK" : "FAILED",
                 reinterpret_cast<unsigned long long>(g_r.find_class),
                 reinterpret_cast<unsigned long long>(g_r.find_objects_of_class),
                 reinterpret_cast<unsigned long long>(g_r.cls_user_widget),
                 reinterpret_cast<unsigned long long>(g_r.cls_text_block),
                 reinterpret_cast<unsigned long long>(g_r.ufn_set_raw_text),
                 reinterpret_cast<unsigned long long>(g_r.ufn_set_text_id));
}

// BP classes resolve lazily (loaded only after user enters practice mode).
// Rate-limit the heavy class scan to once per second.
int g_bp_scan_countdown = 0;

void try_resolve_bp_classes() {
    if (g_r.bp_full_ok || !g_r.engine_ok) return;
    if (g_bp_scan_countdown > 0) { --g_bp_scan_countdown; return; }
    g_bp_scan_countdown = 60;

    if (!g_r.cls_practice_menu) {
        g_r.cls_practice_menu = find_class_by_name(
            g_r.find_class, g_r.find_objects_of_class,
            g_r.cls_user_widget, "WBP_UI_PracticeMenu_C");
        if (g_r.cls_practice_menu) {
            OPENDOJO_LOG("practice_menu: cls_practice_menu = 0x%llX",
                reinterpret_cast<unsigned long long>(g_r.cls_practice_menu));
        }
    }
    if (!g_r.cls_button_row) {
        g_r.cls_button_row = find_class_by_name(
            g_r.find_class, g_r.find_objects_of_class,
            g_r.cls_user_widget, "WBP_UI_PracticeMenu_Button_1_C");
        if (g_r.cls_button_row) {
            OPENDOJO_LOG("practice_menu: cls_button_row = 0x%llX",
                reinterpret_cast<unsigned long long>(g_r.cls_button_row));
        }
    }
    if (!g_r.cls_practice_menu || !g_r.cls_button_row) return;

    auto try_ufn = [](void*& slot, void* cls, const char* name) {
        if (slot) return;
        slot = find_ufunction_by_name(cls, name);
        if (slot) OPENDOJO_LOG("practice_menu: ufn '%s' = 0x%llX",
            name, reinterpret_cast<unsigned long long>(slot));
    };
    auto try_prop = [](std::int32_t& slot, void* cls, const char* name) {
        if (slot >= 0) return;
        slot = find_fproperty_offset(cls, name);
        if (slot >= 0) OPENDOJO_LOG("practice_menu: prop '%s' offset = %d", name, slot);
    };

    try_ufn (g_r.ufn_add_button1, g_r.cls_practice_menu, "AddButton1Data");
    try_ufn (g_r.ufn_update_lv1,  g_r.cls_practice_menu, "UpdateListView1");
    try_ufn (g_r.ufn_on_decide,   g_r.cls_practice_menu, "OnDecideButton1");
    try_prop(g_r.off_list_item,   g_r.cls_button_row,    "list_item");
    try_prop(g_r.off_tb_menu_off, g_r.cls_button_row,    "TB_Menu_OFF");
    try_prop(g_r.off_tb_menu_on,  g_r.cls_button_row,    "TB_Menu_ON");

    // Item class — discover from any live row's list_item.ClassPrivate.
    // The "ListView_1" FProperty doesn't resolve on the menu class on
    // this build (reason unknown), so we use the item count as a proxy
    // for ListView_1.ListItems.Num to determine our row index.
    if (!g_r.cls_item && g_r.off_list_item >= 0) {
        auto rows = find_all_live_objects_of_class(
            g_r.find_objects_of_class, g_r.cls_button_row);
        for (auto* row : rows) {
            std::uint64_t item = 0;
            if (!seh_read_u64(reinterpret_cast<std::uintptr_t>(row)
                              + g_r.off_list_item, &item) || !item) continue;
            std::uint64_t cls = 0;
            if (!seh_read_u64(static_cast<std::uintptr_t>(item)
                              + UOBJECT_CLASS_OFF, &cls) || !cls) continue;
            g_r.cls_item = reinterpret_cast<void*>(cls);
            OPENDOJO_LOG("practice_menu: cls_item = 0x%llX (from row's list_item)",
                reinterpret_cast<unsigned long long>(g_r.cls_item));
            break;
        }
    }

    // item_text offset — lazily on first item we see.
    if (g_r.off_item_text < 0 && g_r.cls_item) {
        g_r.off_item_text = find_fproperty_offset(g_r.cls_item, "Text");
        if (g_r.off_item_text >= 0)
            OPENDOJO_LOG("practice_menu: prop 'Text' (on item) offset = %d",
                         g_r.off_item_text);
    }

    if (g_r.ufn_add_button1 && g_r.ufn_update_lv1 && g_r.ufn_on_decide
        && g_r.off_list_item >= 0 && g_r.off_item_text >= 0
        && g_r.off_tb_menu_off >= 0 && g_r.off_tb_menu_on >= 0
        && g_r.cls_item) {
        g_r.bp_full_ok = true;
        OPENDOJO_LOG("practice_menu: BP-class resolution COMPLETE");
    }
}

// =============================================================================
// Insertion state machine.
// =============================================================================

enum class State { Idle, NeedInsert, WaitForRow, Ready, Failed };

struct InstanceState {
    State        state          = State::Idle;
    void*        menu_widget    = nullptr;
    void*        row_widget     = nullptr;
    void*        tb_off         = nullptr;
    void*        tb_on          = nullptr;
    std::int32_t our_row_idx    = -1;
    int          poll_frames    = 0;
    bool         add_attempted  = false;   // hard latch — prevents duplicate insert
};

InstanceState g_st;

void call_add_button1_data(void* menu, const wchar_t* label, bool enable) {
    struct {
        UE_FString Label;
        bool       Enable;
        std::uint8_t _pad[7];
    } parms{};
    make_fstring_leaky(parms.Label, label);
    parms.Enable = enable;
    __try {
        pe_from_self(menu)(menu, g_r.ufn_add_button1, &parms);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("practice_menu: SEH in AddButton1Data");
    }
}

void call_update_listview1(void* menu) {
    __try {
        pe_from_self(menu)(menu, g_r.ufn_update_lv1, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("practice_menu: SEH in UpdateListView1");
    }
}

void call_set_raw_text(void* tb, const wchar_t* text) {
    struct {
        UE_FString RawText;
        bool       ReplaceUnsupportedChar;
        std::uint8_t _pad[7];
    } parms{};
    make_fstring_leaky(parms.RawText, text);
    parms.ReplaceUnsupportedChar = false;
    __try {
        pe_from_self(tb)(tb, g_r.ufn_set_raw_text, &parms);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static std::atomic<int> count{0};
        if (count.fetch_add(1) < 3) {
            OPENDOJO_LOG("practice_menu: SEH in SetRawText(tb=0x%p)", tb);
        }
    }
}

// Walk WBP_UI_PracticeMenu_Button_1_C live instances, find the one
// whose list_item.Text == "OpenDojo". Captures row_widget + TBs.
bool find_opendojo_row() {
    auto rows = find_all_live_objects_of_class(
        g_r.find_objects_of_class, g_r.cls_button_row);
    for (auto* row : rows) {
        auto raddr = reinterpret_cast<std::uintptr_t>(row);
        std::uint64_t item = 0;
        if (!seh_read_u64(raddr + g_r.off_list_item, &item) || !item) continue;
        if (!fstring_equals_ascii(static_cast<std::uintptr_t>(item)
                                  + g_r.off_item_text, "OpenDojo")) continue;

        std::uint64_t tb_off = 0, tb_on = 0;
        seh_read_u64(raddr + g_r.off_tb_menu_off, &tb_off);
        seh_read_u64(raddr + g_r.off_tb_menu_on,  &tb_on);
        if (!tb_off || !tb_on) continue;

        g_st.row_widget = row;
        g_st.tb_off     = reinterpret_cast<void*>(tb_off);
        g_st.tb_on      = reinterpret_cast<void*>(tb_on);
        OPENDOJO_LOG("practice_menu: OpenDojo row found @0x%llX "
                     "tb_off=0x%llX tb_on=0x%llX",
                     static_cast<unsigned long long>(raddr),
                     static_cast<unsigned long long>(tb_off),
                     static_cast<unsigned long long>(tb_on));
        return true;
    }
    return false;
}

// =============================================================================
// SetTextID UFunction.Func patch — Lua-parity Gryphon bypass.
// =============================================================================
//
// Lua's approach: hook UPolarisTextBlock::SetTextID post-call; when the
// hooked TB is one of ours, re-apply SetRawText("OpenDojo") so the
// "err(OpenDojo)@@@@" Gryphon wrapper gets overwritten. Event-driven —
// fires only when text actually changes (hover/focus state transitions),
// not 60+ times per second.
//
// DLL equivalent: patch UFunction.Func at offset 0xD8. ProcessEvent
// reads this pointer when dispatching the call. We replace it with our
// shim, which calls the original (preserving game behavior) then —
// for OUR captured TB pointers — calls SetRawText.
//
// Native UFunction.Func signature: void (UObject*, FFrame&, void*)

using NativeFuncFn = void (*)(void* self, void* frame, void* result);

NativeFuncFn      g_set_text_id_orig = nullptr;
std::atomic<bool> g_set_text_id_hooked{false};

void set_text_id_shim(void* self, void* frame, void* result) {
    if (g_set_text_id_orig) g_set_text_id_orig(self, frame, result);
    // After original runs, the TB's text is whatever Gryphon resolved.
    // If this is one of our TBs, slap "OpenDojo" back in via SetRawText.
    if (g_st.state == State::Ready
        && (self == g_st.tb_off || self == g_st.tb_on)) {
        // SetRawText is a separate UFunction — calling via ProcessEvent.
        // Cannot use pe_from_self because that resolves the runtime PE
        // entry (which UE4SS may have hooked at the function-bytes
        // level). Resolving from the TB's own vtable is the canonical
        // path; UE4SS's hook chain handles it.
        struct {
            UE_FString RawText;
            bool       ReplaceUnsupportedChar;
            std::uint8_t _pad[7];
        } parms{};
        make_fstring_leaky(parms.RawText, L"OpenDojo");
        parms.ReplaceUnsupportedChar = false;
        __try {
            auto pe = pe_from_self(self);
            pe(self, g_r.ufn_set_raw_text, &parms);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static std::atomic<int> n{0};
            if (n.fetch_add(1) < 3)
                OPENDOJO_LOG("practice_menu: SEH in SetRawText (shim)");
        }
    }
}

bool install_set_text_id_hook() {
    if (g_set_text_id_hooked.load()) return true;
    if (!g_r.ufn_set_text_id) return false;
    auto func_slot = reinterpret_cast<std::uintptr_t>(g_r.ufn_set_text_id)
                   + UFUNCTION_FUNC_OFF;

    // Read original.
    std::uint64_t orig = 0;
    if (!seh_read_u64(func_slot, &orig) || !orig) {
        OPENDOJO_LOG("practice_menu: SetTextID Func slot unreadable");
        return false;
    }
    g_set_text_id_orig = reinterpret_cast<NativeFuncFn>(orig);

    // Write our shim. UE5 keeps UFunction in heap (R/W) but
    // VirtualProtect to be safe across allocator variations.
    DWORD oldp = 0;
    VirtualProtect(reinterpret_cast<void*>(func_slot), 8,
                   PAGE_READWRITE, &oldp);
    *reinterpret_cast<std::uint64_t*>(func_slot) =
        reinterpret_cast<std::uint64_t>(&set_text_id_shim);
    if (oldp) {
        VirtualProtect(reinterpret_cast<void*>(func_slot), 8,
                       oldp, &oldp);
    }
    g_set_text_id_hooked.store(true);
    OPENDOJO_LOG("practice_menu: SetTextID Func patched "
                 "(slot=0x%llX orig=0x%llX shim=0x%llX)",
                 static_cast<unsigned long long>(func_slot),
                 static_cast<unsigned long long>(orig),
                 reinterpret_cast<unsigned long long>(&set_text_id_shim));
    return true;
}

// =============================================================================
// Click intercept — MinHook on the Decide dispatcher.
// =============================================================================
//
// FUN_142e5ef30 has signature `void(rcx=widget*, edx=id)`. Called once
// per row Decide. We MinHook here because the prior approach (polling
// PracticeMenuImpl manager+0xC8) didn't see writes — likely because
// some flag guard at manager+0x31/0x32 was non-zero, suppressing the
// write. Hooking the dispatcher catches every call unambiguously.

using InvokeDecideFn = void (__fastcall*)(void* self, std::uint32_t id);
InvokeDecideFn      g_invoke_decide_orig = nullptr;
std::atomic<bool>   g_invoke_decide_hooked{false};

void __fastcall invoke_decide_hook(void* self, std::uint32_t id) {
    auto signed_id = static_cast<std::int32_t>(id);
    if (g_st.state == State::Ready
        && self == g_st.menu_widget
        && signed_id == g_st.our_row_idx) {
        // The Decide event fires multiple times for a single physical
        // click — press + each frame held + release can each produce
        // an invocation. Debounce: toggle only if >= 250 ms since the
        // last toggle. Short enough that consecutive intentional
        // clicks still register, long enough to merge a hold.
        using clock = std::chrono::steady_clock;
        static clock::time_point last_toggle =
            clock::now() - std::chrono::seconds(1);
        const auto now = clock::now();
        if (now - last_toggle >= std::chrono::milliseconds(250)) {
            last_toggle = now;
            OPENDOJO_LOG("practice_menu: row clicked (id=%d) -> toggle ImGui",
                         signed_id);
            opendojo::render_hook::toggle_menu();
        }
    }
    g_invoke_decide_orig(self, id);
}

bool install_invoke_decide_hook() {
    if (g_invoke_decide_hooked.load()) return true;
    auto base = memory::polaris_base();
    if (!base) return false;
    auto target = reinterpret_cast<void*>(base + INVOKE_DECIDE_DISPATCH_RVA);
    MH_Initialize();   // no-op if already initialized
    auto s = MH_CreateHook(target,
                           reinterpret_cast<LPVOID>(&invoke_decide_hook),
                           reinterpret_cast<LPVOID*>(&g_invoke_decide_orig));
    if (s != MH_OK) {
        OPENDOJO_LOG("practice_menu: MH_CreateHook(InvokeDecide) failed: %d", s);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        OPENDOJO_LOG("practice_menu: MH_EnableHook(InvokeDecide) failed");
        return false;
    }
    g_invoke_decide_hooked.store(true);
    OPENDOJO_LOG("practice_menu: InvokeDecide dispatcher hooked "
                 "(target=0x%p orig=0x%p shim=0x%p)",
                 target, g_invoke_decide_orig, &invoke_decide_hook);
    return true;
}

}  // anonymous namespace

// =============================================================================
// Public API.
// =============================================================================

bool ensure_resolved() {
    std::call_once(g_resolve_once, do_resolve);
    return g_r.engine_ok;
}

void tick() {
    if (!ensure_resolved()) return;

    if (!g_r.bp_full_ok) {
        try_resolve_bp_classes();
        if (!g_r.bp_full_ok) return;
    }

    // Engine-class hook: install once. UE4SS-safe because we patch the
    // UFunction's Func pointer, not the global ProcessEvent address.
    if (!g_set_text_id_hooked.load()) install_set_text_id_hook();
    // Click intercept: MinHook on the Decide dispatcher
    // (Polaris-internal, no UE4SS conflict).
    if (!g_invoke_decide_hooked.load()) install_invoke_decide_hook();

    auto* menu = find_first_live_object_of_class(
        g_r.find_objects_of_class, g_r.cls_practice_menu);

    if (!menu) {
        if (g_st.state != State::Idle) {
            OPENDOJO_LOG("practice_menu: menu gone — resetting state");
            g_st = InstanceState{};
        }
        return;
    }

    if (menu != g_st.menu_widget) {
        g_st = InstanceState{};
        g_st.menu_widget = menu;
        g_st.state = State::NeedInsert;
        OPENDOJO_LOG("practice_menu: new menu instance 0x%p", menu);
    }

    switch (g_st.state) {
        case State::NeedInsert: {
            // Idempotency option 1: row already exists (e.g., reused
            // menu instance pointer between practice-menu opens).
            if (find_opendojo_row()) {
                auto count = count_live_objects_of_class(
                    g_r.find_objects_of_class, g_r.cls_item);
                g_st.our_row_idx = (count > 0) ? count - 1 : 0;
                call_set_raw_text(g_st.tb_off, L"OpenDojo");
                call_set_raw_text(g_st.tb_on,  L"OpenDojo");
                g_st.add_attempted = true;
                g_st.state = State::Ready;
                OPENDOJO_LOG("practice_menu: existing OpenDojo row reused "
                             "(idx=%d)", g_st.our_row_idx);
                break;
            }

            // HARD LATCH: never call AddButton1Data twice for the same
            // menu instance, even if find_opendojo_row hasn't seen the
            // row yet. The previous build had a duplicate-insert bug
            // because the row widget takes several frames to
            // materialize and we were re-entering NeedInsert.
            if (g_st.add_attempted) {
                // Fall through to WaitForRow-style polling.
                g_st.state = State::WaitForRow;
                break;
            }

            auto count = count_live_objects_of_class(
                g_r.find_objects_of_class, g_r.cls_item);
            if (count < 0) {
                OPENDOJO_LOG("practice_menu: item-count failed; entering Failed");
                g_st.state = State::Failed;
                return;
            }
            g_st.our_row_idx = count;
            g_st.add_attempted = true;
            OPENDOJO_LOG("practice_menu: pre-insert item count=%d "
                         "(our row will be at index %d)",
                         count, g_st.our_row_idx);
            call_add_button1_data(menu, L"OpenDojo", true);
            call_update_listview1(menu);
            g_st.state = State::WaitForRow;
            g_st.poll_frames = 0;
            break;
        }

        case State::WaitForRow: {
            if (find_opendojo_row()) {
                // One-shot SetRawText to set the initial label. The
                // SetTextID Func patch handles re-application on every
                // subsequent BP UpdateData (hover, focus, etc.). No
                // per-frame poll loop — Lua-parity behavior.
                call_set_raw_text(g_st.tb_off, L"OpenDojo");
                call_set_raw_text(g_st.tb_on,  L"OpenDojo");
                g_st.state = State::Ready;
                OPENDOJO_LOG("practice_menu: row Ready (idx=%d)", g_st.our_row_idx);
            } else if (++g_st.poll_frames > 600) {
                OPENDOJO_LOG("practice_menu: gave up waiting for row "
                             "after %d frames; entering Failed",
                             g_st.poll_frames);
                g_st.state = State::Failed;
            }
            break;
        }

        case State::Ready: {
            // No per-frame work — label fix is event-driven via the
            // SetTextID Func patch, click intercept is event-driven
            // via the InvokeDecide MinHook detour.
            break;
        }

        case State::Idle:   break;
        case State::Failed: break;   // terminal no-op for this menu instance
    }
}

}  // namespace opendojo::practice_menu
