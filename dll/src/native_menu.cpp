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

// -----------------------------------------------------------------------------
// Resolution state. All addresses are absolute; resolved once per process.
// -----------------------------------------------------------------------------

struct Resolved {
    FindUnrealClassFn find_class    = nullptr;
    void*             text_menu_cls = nullptr;  // UClass* for /Script/Polaris.PolarisUMGTextMenu
    bool              attempted     = false;
    bool              ok            = false;
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

// Empirical findings from prior introspection runs:
//   - Children at +0x48 (confirmed)
//   - All UFunctions in the chain are 0xE0 bytes apart (confirmed)
//   - Func pointer is NOT at +0xA0 — that read returned 0 for every child.
//     The UStruct portion is larger than expected; Func is somewhere
//     in the +0xB0..+0xD8 range. This pass dumps the full tail for the
//     first few children so we can pin Func's exact offset by eye.
constexpr std::ptrdiff_t UCLASS_CHILDREN_OFF   = 0x48;
constexpr std::ptrdiff_t UFIELD_NEXT_OFF       = 0x28;
constexpr std::ptrdiff_t UOBJECT_NAME_OFF      = 0x18;

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

    constexpr int MAX_WALK         = 80;
    constexpr int FULL_DUMP_COUNT  = 3;     // dump 0xE0 bytes for first N children
    constexpr int FULL_DUMP_QWORDS = 28;    // 28 * 8 = 0xE0 (full UFunction)

    int n = 0;
    while (child && n < MAX_WALK) {
        std::uint64_t vt   = 0;
        std::uint32_t cidx = 0;
        std::uint32_t cnum = 0;
        std::uint64_t next = 0;

        if (!seh_read_u64(child + 0x00,                 &vt))   { OPENDOJO_LOG("    [%2d] 0x%llX  SEH @ vt — abort", n + 1, child); break; }
        if (!seh_read_u32(child + UOBJECT_NAME_OFF,     &cidx)) { OPENDOJO_LOG("    [%2d] 0x%llX  SEH @ FName.idx — abort", n + 1, child); break; }
        if (!seh_read_u32(child + UOBJECT_NAME_OFF + 4, &cnum)) { OPENDOJO_LOG("    [%2d] 0x%llX  SEH @ FName.num — abort", n + 1, child); break; }
        if (!seh_read_u64(child + UFIELD_NEXT_OFF,      &next)) next = 0;

        OPENDOJO_LOG("    [%2d] 0x%llX  vt=0x%llX  FName(%u,%u)",
                     n + 1,
                     static_cast<unsigned long long>(child),
                     static_cast<unsigned long long>(vt),
                     cidx, cnum);

        // For the first FULL_DUMP_COUNT children, also dump the full
        // 0xE0 of UFunction memory so we can identify where Func lives.
        // We're particularly interested in the +0x80..+0xD8 range where
        // FunctionFlags + RPC IDs + EventGraph stuff + Func sit.
        if (n < FULL_DUMP_COUNT) {
            OPENDOJO_LOG("         full dump (offset : qword):");
            for (int q = 0; q < FULL_DUMP_QWORDS; ++q) {
                std::uint64_t v = 0;
                if (!seh_read_u64(child + q * 8, &v)) {
                    OPENDOJO_LOG("           +0x%02X: SEH", q * 8);
                    break;
                }
                OPENDOJO_LOG("           +0x%02X: 0x%016llX",
                             q * 8, static_cast<unsigned long long>(v));
            }
        }

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
        introspect_uclass("UPolarisUMGDialog",
                          reinterpret_cast<std::uintptr_t>(dialog_cls));
    } else {
        OPENDOJO_LOG("native_menu: FindObject(/Script/Polaris.PolarisUMGDialog) returned null");
    }
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

void show() {
    if (g_visible.exchange(true, std::memory_order_relaxed)) return;
    if (!ensure_resolved()) {
        OPENDOJO_LOG("native_menu: show() — patterns not resolved; cannot construct widget");
        return;
    }
    log_phase1_warning_once();

    // [PHASE 1 TODO] Construct the widget:
    //   1. Get a UWorld* / APlayerController*. Cheapest path: walk
    //      the GlobalPlayerHolder we already resolve in players.cpp.
    //      Player struct's outer eventually reaches a UWorld. Pin the
    //      offset chain via runtime inspection.
    //   2. Allocate the widget: StaticConstructObject_Internal(
    //         g_resolved.text_menu_cls, owner, name, flags, ...);
    //      Pattern for that function: TBD (Phase 1).
    //   3. Call UUserWidget::Initialize via vtable slot N. Dump the
    //      UClass's FuncMap at runtime to identify N.
    //   4. Call UUserWidget::AddToViewport(zOrder=100) via vtable slot M.
    //   5. Hold the widget pointer in module-scope state so hide() can
    //      RemoveFromParent it.
    //   6. Acquire UI focus via UWidgetBlueprintLibrary::SetInputMode_UIOnlyEx.
    //
    // For Phase 0 we simply mark visible=true so toggle() works
    // symmetrically; nothing actually appears on screen yet.
}

void hide() {
    if (!g_visible.exchange(false, std::memory_order_relaxed)) return;

    // [PHASE 1 TODO] RemoveFromParent on the cached widget pointer.
    // Restore input mode to game-and-UI or game-only as appropriate.
    // Do not destroy the widget — keep it for the next show() to reuse.
}

void toggle() {
    if (is_visible()) hide(); else show();
}

void tick() {
    // Resolve on first tick (cheap once cached).
    ensure_resolved();

    // F11 edge-trigger. GetAsyncKeyState's high bit is the down state;
    // we want the leading edge so the toggle fires once per press.
    bool now = (GetAsyncKeyState(HOTKEY_VK) & 0x8000) != 0;
    if (now && !g_last_hotkey_state) {
        toggle();
    }
    g_last_hotkey_state = now;

    // [PHASE 1 TODO] When visible, drive per-frame widget state:
    //   - Refresh drill list if dirty (call commands::list_drills()).
    //   - Update item labels if selection-dependent.
    //   - Maintain focus if some other UE widget steals it.
}

}  // namespace opendojo::native_menu
