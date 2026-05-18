#include "drill_browser.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include "commands.hpp"
#include "log.hpp"
#include "memory.hpp"

// Drill browser — UMG widget integration. See drill_browser.hpp and
// dll/OPENDOJO_DRILL_BROWSER_WIDGET.md for the full design.
//
// IMPORTANT: This TU intentionally duplicates a handful of UE5 runtime
// helpers (SEH reads, FName decode, FProperty walker, ProcessEvent
// dispatch). dialog.cpp and native_menu.cpp do the same — the codebase
// convention is "lift to a shared header once a third caller appears."
// We're at three now; the lift is a follow-up task.

namespace opendojo::drill_browser {

namespace {

// =============================================================================
// UE5 runtime helpers — local duplicates.
// =============================================================================

constexpr std::ptrdiff_t USTRUCT_CHILDREN_OFF        = 0x48;  // UField* head
constexpr std::ptrdiff_t USTRUCT_CHILD_PROPERTIES_OFF = 0x50; // FField* head
constexpr std::ptrdiff_t UFIELD_NEXT_OFF             = 0x28;  // UObject NamePrivate
constexpr std::ptrdiff_t UOBJECT_NAME_OFF            = 0x18;
constexpr std::ptrdiff_t FFIELD_NEXT_OFF             = 0x20;
constexpr std::ptrdiff_t FFIELD_NAME_OFF             = 0x28;
constexpr std::ptrdiff_t FPROP_OFFSET_INTERNAL       = 0x4C;

// ProcessEvent vtable slot — pinned via runtime dump (see dialog.cpp).
constexpr int PROCESS_EVENT_VTABLE_SLOT = 77;

// FNamePool — same constants as dialog.cpp / native_menu.cpp.
constexpr std::uintptr_t FNAME_POOL_RVA      = 0x9955480;
constexpr std::ptrdiff_t POOL_BLOCKS_OFFSET  = 0x10;
constexpr std::uint32_t  FNAME_BLOCK_MASK    = 0x1FFF;
constexpr std::uint32_t  FNAME_STRIDE_MASK   = 0xFFFF;
constexpr std::uint32_t  FNAME_BLOCK_SHIFT   = 16;

bool seh_read_u64(std::uintptr_t a, std::uint64_t* o) {
    __try { std::memcpy(o, reinterpret_cast<const void*>(a), 8); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_read_u32(std::uintptr_t a, std::uint32_t* o) {
    __try { std::memcpy(o, reinterpret_cast<const void*>(a), 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_read_u16(std::uintptr_t a, std::uint16_t* o) {
    __try { std::memcpy(o, reinterpret_cast<const void*>(a), 2); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool decode_fname(std::uint32_t idx, char* buf, std::size_t buf_size) {
    if (!buf || !buf_size) return false;
    buf[0] = '\0';
    if (idx == 0) {
        if (buf_size >= 5) std::memcpy(buf, "None", 5);
        return true;
    }
    auto base = memory::polaris_base();
    if (!base) return false;
    std::uintptr_t pool = base + FNAME_POOL_RVA;
    std::uint32_t block_idx = (idx >> FNAME_BLOCK_SHIFT) & FNAME_BLOCK_MASK;
    std::uint32_t stride    = idx & FNAME_STRIDE_MASK;
    std::uint64_t block_ptr = 0;
    if (!seh_read_u64(pool + POOL_BLOCKS_OFFSET + block_idx * 8, &block_ptr)
        || !block_ptr) return false;
    std::uintptr_t entry = static_cast<std::uintptr_t>(block_ptr)
                         + static_cast<std::uintptr_t>(stride) * 2;
    std::uint16_t header = 0;
    if (!seh_read_u16(entry, &header)) return false;
    bool wide = (header & 1) != 0;
    std::uint32_t len = header >> 6;
    if (!len || len > 1023) return false;
    std::uintptr_t data = entry + 2;
    std::size_t i;
    for (i = 0; i < len && i + 1 < buf_size; ++i) {
        if (wide) {
            std::uint16_t w = 0;
            if (!seh_read_u16(data + i * 2, &w)) break;
            buf[i] = (w > 0x7F) ? '?' : static_cast<char>(w);
        } else {
            std::uint8_t b = 0;
            __try { b = *reinterpret_cast<const std::uint8_t*>(data + i); }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            buf[i] = (b > 0x7F) ? '?' : static_cast<char>(b);
        }
    }
    buf[i] = '\0';
    return true;
}

// findUnrealClass sig-scan — same pattern dialog.cpp uses.
using FindUnrealClassFn = void* (*)(void* outer, const wchar_t* name, bool exact);
constexpr const char* PAT_FIND_UNREAL_CLASS =
    "45 33 C0 49 8B CF E8 ?? ?? ?? ?? 48 8B 4C 24 60";

FindUnrealClassFn g_find_class = nullptr;

bool resolve_find_class() {
    if (g_find_class) return true;
    auto base = memory::polaris_base();
    if (!base) return false;
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    std::uintptr_t ts = 0; std::size_t sz = 0;
    auto first = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = first[i];
        if (std::strncmp(reinterpret_cast<const char*>(s.Name), ".text", 5) == 0) {
            ts = base + s.VirtualAddress; sz = s.Misc.VirtualSize; break;
        }
    }
    if (!ts) return false;

    // Compile pattern + scan (inline, no helper).
    std::uint8_t pb[64]; std::uint8_t pm[64]; std::size_t plen = 0;
    auto hex = [](char c, int& v){
        if(c>='0'&&c<='9'){v=c-'0';return true;}
        if(c>='A'&&c<='F'){v=c-'A'+10;return true;}
        if(c>='a'&&c<='f'){v=c-'a'+10;return true;}
        return false;
    };
    for (const char* p = PAT_FIND_UNREAL_CLASS; *p && plen < sizeof(pb);) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') { pb[plen]=0; pm[plen]=0; ++plen; p+=2; }
        else { int hi,lo; if(!hex(p[0],hi)||!hex(p[1],lo))return false;
               pb[plen]=(std::uint8_t)((hi<<4)|lo); pm[plen]=1; ++plen; p+=2; }
    }
    auto bp = reinterpret_cast<const std::uint8_t*>(ts);
    std::uintptr_t hit = 0;
    for (std::size_t i = 0; i + plen <= sz; ++i) {
        bool m = true;
        for (std::size_t j = 0; j < plen; ++j) {
            if (pm[j] && bp[i + j] != pb[j]) { m = false; break; }
        }
        if (m) { hit = ts + i; break; }
    }
    if (!hit) return false;
    auto disp_at = hit + 7;
    auto disp = static_cast<std::int32_t>(memory::read_u32(disp_at));
    auto fn = disp_at + 4 + static_cast<std::uintptr_t>(static_cast<std::int64_t>(disp));
    g_find_class = reinterpret_cast<FindUnrealClassFn>(fn);
    return true;
}

// Walk a UStruct's Children list (UFunctions for a UClass) by FName.
void* find_ufunction_by_name(void* uclass, const char* target) {
    if (!uclass) return nullptr;
    auto base = reinterpret_cast<std::uintptr_t>(uclass);
    std::uint64_t child_q = 0;
    if (!seh_read_u64(base + USTRUCT_CHILDREN_OFF, &child_q)) return nullptr;
    std::uintptr_t child = static_cast<std::uintptr_t>(child_q);
    char buf[256];
    int hops = 0;
    while (child && hops++ < 512) {
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

// Walk a UStruct's ChildProperties list (FProperty chain) by FName and
// return the property's Offset_Internal, or -1 on miss.
std::int32_t find_property_offset(void* uclass, const char* target) {
    if (!uclass) return -1;
    auto base = reinterpret_cast<std::uintptr_t>(uclass);
    std::uint64_t child_q = 0;
    if (!seh_read_u64(base + USTRUCT_CHILD_PROPERTIES_OFF, &child_q)) return -1;
    std::uintptr_t fld = static_cast<std::uintptr_t>(child_q);
    char buf[256];
    int hops = 0;
    while (fld && hops++ < 512) {
        std::uint32_t idx = 0;
        if (!seh_read_u32(fld + FFIELD_NAME_OFF, &idx)) break;
        if (decode_fname(idx, buf, sizeof(buf))
            && std::strcmp(buf, target) == 0) {
            std::uint32_t off = 0;
            if (!seh_read_u32(fld + FPROP_OFFSET_INTERNAL, &off)) return -1;
            return static_cast<std::int32_t>(off);
        }
        std::uint64_t nxt = 0;
        if (!seh_read_u64(fld + FFIELD_NEXT_OFF, &nxt)) break;
        fld = static_cast<std::uintptr_t>(nxt);
    }
    return -1;
}

// ProcessEvent dispatch.
using ProcessEventFn = void (*)(void* self, void* function, void* parms);

void call_pe(void* self, void* function, void* parms) {
    if (!self || !function) return;
    __try {
        auto vt = *reinterpret_cast<void***>(self);
        auto pe = reinterpret_cast<ProcessEventFn>(vt[PROCESS_EVENT_VTABLE_SLOT]);
        pe(self, function, parms);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OPENDOJO_LOG("drill_browser: SEH inside ProcessEvent");
    }
}

// UE5 FString — { wchar_t* Data, int32 Num, int32 Max } (16 bytes).
struct UE_FString {
    wchar_t*     data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};

void make_fstring_leaky(UE_FString& out, const char* utf8) {
    if (!utf8) utf8 = "";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) len = 1;
    auto* buf = static_cast<wchar_t*>(std::malloc(sizeof(wchar_t) * static_cast<size_t>(len)));
    if (!buf) { out = {}; return; }
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, len) == 0) {
        buf[0] = L'\0'; len = 1;
    }
    out.data = buf;
    out.num  = len;
    out.max  = len;
}

// =============================================================================
// findUnrealObjectsOfClass — sig-scan duplicate (mirrors native_menu.cpp).
// =============================================================================

struct UE_TArray {
    void*        data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};

using FindObjectsOfClassFn = void (*)(void* uclass,
                                      UE_TArray* out,
                                      bool include_derived,
                                      std::uint32_t exclude_object_flags,
                                      std::uint32_t exclude_internal_flags);

constexpr const char* PAT_FIND_OBJECTS_OF_CLASS =
    "E8 ?? ?? ?? ?? 90 48 89 6C 24 30";

FindObjectsOfClassFn g_find_objects = nullptr;

bool resolve_find_objects() {
    if (g_find_objects) return true;
    auto base = memory::polaris_base();
    if (!base) return false;
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    std::uintptr_t ts = 0; std::size_t sz = 0;
    auto first = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = first[i];
        if (std::strncmp(reinterpret_cast<const char*>(s.Name), ".text", 5) == 0) {
            ts = base + s.VirtualAddress; sz = s.Misc.VirtualSize; break;
        }
    }
    if (!ts) return false;

    std::uint8_t pb[32], pm[32]; std::size_t plen = 0;
    auto hex = [](char c, int& v){
        if(c>='0'&&c<='9'){v=c-'0';return true;}
        if(c>='A'&&c<='F'){v=c-'A'+10;return true;}
        if(c>='a'&&c<='f'){v=c-'a'+10;return true;}
        return false;
    };
    for (const char* p = PAT_FIND_OBJECTS_OF_CLASS; *p && plen < sizeof(pb);) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0]=='?'&&p[1]=='?') { pb[plen]=0; pm[plen]=0; ++plen; p+=2; }
        else { int hi,lo; if(!hex(p[0],hi)||!hex(p[1],lo)) return false;
               pb[plen]=(std::uint8_t)((hi<<4)|lo); pm[plen]=1; ++plen; p+=2; }
    }
    auto bp = reinterpret_cast<const std::uint8_t*>(ts);
    std::uintptr_t hit = 0;
    for (std::size_t i = 0; i + plen <= sz; ++i) {
        bool m = true;
        for (std::size_t j = 0; j < plen; ++j) {
            if (pm[j] && bp[i + j] != pb[j]) { m = false; break; }
        }
        if (m) { hit = ts + i; break; }
    }
    if (!hit) return false;
    auto disp_at = hit + 1;
    auto disp = static_cast<std::int32_t>(memory::read_u32(disp_at));
    auto fn = disp_at + 4 + static_cast<std::uintptr_t>(static_cast<std::int64_t>(disp));
    g_find_objects = reinterpret_cast<FindObjectsOfClassFn>(fn);
    return true;
}

// =============================================================================
// Resolved state for the browser widget.
// =============================================================================

constexpr const wchar_t* ROOT_CLASS_PATH =
    L"/Game/OpenDojo/UI/WBP_UI_OpenDojo_Root.WBP_UI_OpenDojo_Root_C";

struct Resolved {
    void*        root_class      = nullptr;
    void*        ufn_GetBrowser  = nullptr;
    void*        ufn_ShowBrowser = nullptr;
    void*        ufn_HideAll     = nullptr;
    void*        ufn_AddRow      = nullptr;
    void*        ufn_ClearRows   = nullptr;
    std::int32_t off_ClickAvailable    = -1;
    std::int32_t off_ClickedDrillIndex = -1;
    std::int32_t off_ClickedAction     = -1;
    bool         class_ok        = false;
};

Resolved g_r;
std::atomic<bool> g_open{false};

// Cached drill list — keeps paths alive across the open() → tick() →
// commands::load_drill() flow. Indices match what we pushed into the
// widget (DrillIndex param), so the widget's ClickedDrillIndex is a
// direct index into this vector.
std::vector<std::filesystem::path> g_drills;

// Resolve class + UFunctions on the class CDO. Run once after the .pak
// is loaded. Returns false (and logs why) if the class isn't present.
bool resolve_class_only() {
    auto& r = g_r;
    if (r.class_ok) return true;
    if (!resolve_find_class()) {
        OPENDOJO_LOG("drill_browser: findUnrealClass unavailable");
        return false;
    }
    r.root_class = g_find_class(nullptr, ROOT_CLASS_PATH, /*exact=*/true);
    if (!r.root_class) {
        OPENDOJO_LOG("drill_browser: %ls not found (deploy the .pak first)",
                     ROOT_CLASS_PATH);
        return false;
    }
    OPENDOJO_LOG("drill_browser: Root UClass = 0x%llX",
                 (unsigned long long)reinterpret_cast<std::uintptr_t>(r.root_class));

    r.ufn_GetBrowser  = find_ufunction_by_name(r.root_class, "GetBrowser");
    r.ufn_ShowBrowser = find_ufunction_by_name(r.root_class, "ShowDrillBrowser");
    r.ufn_HideAll     = find_ufunction_by_name(r.root_class, "HideAll");

    if (!r.ufn_GetBrowser || !r.ufn_ShowBrowser || !r.ufn_HideAll) {
        OPENDOJO_LOG("drill_browser: Root UFunctions missing — "
                     "GetBrowser=%p ShowDrillBrowser=%p HideAll=%p",
                     r.ufn_GetBrowser, r.ufn_ShowBrowser, r.ufn_HideAll);
        return false;
    }
    r.class_ok = true;
    return true;
}

// Find the single live root widget instance + cache its Browser handle.
// Re-runs every open() so we re-bind cleanly after a level transition.
struct LiveHandles {
    void* root    = nullptr;
    void* browser = nullptr;
};

bool find_live_widget(LiveHandles& out) {
    if (!resolve_find_objects()) {
        OPENDOJO_LOG("drill_browser: findUnrealObjectsOfClass unavailable");
        return false;
    }
    UE_TArray results;
    g_find_objects(g_r.root_class, &results,
                   /*include_derived=*/true,
                   /*exclude_object_flags=*/0x10,  // skip CDO
                   /*exclude_internal_flags=*/0);
    if (!results.data || results.num <= 0) {
        OPENDOJO_LOG("drill_browser: no live Root widget — BPModLoader "
                     "didn't spawn BP_OpenDojoLoader (yet)");
        return false;
    }
    void* root = static_cast<void**>(results.data)[0];
    if (!root) return false;
    out.root = root;

    // Resolve the Browser sub-widget by calling GetBrowser().
    struct GetBrowser_Args { void* ReturnValue; };
    GetBrowser_Args a{};
    call_pe(root, g_r.ufn_GetBrowser, &a);
    if (!a.ReturnValue) {
        OPENDOJO_LOG("drill_browser: GetBrowser returned null");
        return false;
    }
    out.browser = a.ReturnValue;

    // First time we see the Browser, also resolve its UFunctions +
    // property offsets. The Browser is a different UClass than the
    // Root, so we read its UClass via the standard +0x10 ClassPrivate
    // pointer.
    if (!g_r.ufn_AddRow) {
        std::uint64_t browser_class_q = 0;
        seh_read_u64(reinterpret_cast<std::uintptr_t>(out.browser) + 0x10,
                     &browser_class_q);
        auto* browser_class = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(browser_class_q));
        OPENDOJO_LOG("drill_browser: Browser UClass = 0x%llX",
                     (unsigned long long)browser_class_q);

        g_r.ufn_AddRow    = find_ufunction_by_name(browser_class, "AddDrillRow");
        g_r.ufn_ClearRows = find_ufunction_by_name(browser_class, "ClearDrillRows");
        g_r.off_ClickAvailable    = find_property_offset(browser_class, "ClickAvailable");
        g_r.off_ClickedDrillIndex = find_property_offset(browser_class, "ClickedDrillIndex");
        g_r.off_ClickedAction     = find_property_offset(browser_class, "ClickedAction");

        OPENDOJO_LOG("drill_browser: AddRow=%p ClearRows=%p "
                     "off_avail=%d off_idx=%d off_action=%d",
                     g_r.ufn_AddRow, g_r.ufn_ClearRows,
                     g_r.off_ClickAvailable, g_r.off_ClickedDrillIndex,
                     g_r.off_ClickedAction);

        if (!g_r.ufn_AddRow || !g_r.ufn_ClearRows
            || g_r.off_ClickAvailable < 0
            || g_r.off_ClickedDrillIndex < 0
            || g_r.off_ClickedAction < 0) {
            OPENDOJO_LOG("drill_browser: browser resolution incomplete — "
                         "widget BP missing expected vars/functions");
            return false;
        }
    }
    return true;
}

LiveHandles g_live;

}  // namespace

// -----------------------------------------------------------------------------
// Public API.
// -----------------------------------------------------------------------------

bool ensure_resolved() {
    return resolve_class_only();
}

bool open() {
    if (!resolve_class_only()) return false;
    if (g_open.exchange(true)) {
        OPENDOJO_LOG("drill_browser: already open — ignoring");
        return false;
    }
    if (!find_live_widget(g_live)) {
        g_open = false;
        return false;
    }

    // Snapshot the drill list and push into the widget.
    g_drills.clear();
    auto drills = opendojo::commands::list_drills();
    g_drills.reserve(drills.size());

    // Clear any prior rows.
    call_pe(g_live.browser, g_r.ufn_ClearRows, nullptr);

    // Reset the click signal so we don't pick up a stale click from a
    // previous session.
    if (g_r.off_ClickAvailable >= 0) {
        auto* avail = reinterpret_cast<std::uint8_t*>(
            reinterpret_cast<std::uintptr_t>(g_live.browser) + g_r.off_ClickAvailable);
        *avail = 0;
    }

    struct AddRow_Args {
        UE_FString    Name;
        UE_FString    Character;
        std::int32_t  Recordings;
        std::int32_t  DrillIndex;
    };
    static_assert(sizeof(AddRow_Args) == 0x28,
                  "AddRow_Args expected 0x28 (FString*2 + 2x int32)");

    int idx = 0;
    for (const auto& d : drills) {
        AddRow_Args a{};
        make_fstring_leaky(a.Name,      d.name.c_str());
        make_fstring_leaky(a.Character, d.character.c_str());
        a.Recordings = static_cast<std::int32_t>(d.recording_count);
        a.DrillIndex = idx;
        call_pe(g_live.browser, g_r.ufn_AddRow, &a);
        g_drills.push_back(d.path);
        ++idx;
    }
    OPENDOJO_LOG("drill_browser: pushed %d rows", idx);

    // Show.
    call_pe(g_live.root, g_r.ufn_ShowBrowser, nullptr);
    return true;
}

void tick() {
    if (!g_open.load(std::memory_order_relaxed)) return;
    if (!g_r.class_ok || !g_live.browser
        || g_r.off_ClickAvailable < 0) {
        g_open = false;
        return;
    }
    auto* avail = reinterpret_cast<std::uint8_t*>(
        reinterpret_cast<std::uintptr_t>(g_live.browser) + g_r.off_ClickAvailable);
    if (!*avail) return;

    // A click is available. Read index + action, dispatch.
    auto bw = reinterpret_cast<std::uintptr_t>(g_live.browser);
    auto* drill_idx_p = reinterpret_cast<std::int32_t*>(bw + g_r.off_ClickedDrillIndex);
    auto* action_p    = reinterpret_cast<std::int32_t*>(bw + g_r.off_ClickedAction);
    std::int32_t drill_idx = *drill_idx_p;
    std::int32_t action    = *action_p;

    OPENDOJO_LOG("drill_browser: click drill=%d action=%d", drill_idx, action);

    if (action == 0 || action == 1) {
        if (drill_idx >= 0 && static_cast<std::size_t>(drill_idx) < g_drills.size()) {
            auto mode = (action == 0)
                ? opendojo::commands::LoadMode::AppendToFree
                : opendojo::commands::LoadMode::ReplaceAll;
            auto res = opendojo::commands::load_drill(g_drills[drill_idx], mode);
            OPENDOJO_LOG("drill_browser: load_drill -> ok=%d msg='%s'",
                         res.ok ? 1 : 0, res.message.c_str());
        } else {
            OPENDOJO_LOG("drill_browser: drill_idx %d out of range (have %zu)",
                         drill_idx, g_drills.size());
        }
    }
    // action == 2 (close) or after dispatch: hide + reset.
    *avail = 0;
    call_pe(g_live.root, g_r.ufn_HideAll, nullptr);
    g_open = false;
}

}  // namespace opendojo::drill_browser
