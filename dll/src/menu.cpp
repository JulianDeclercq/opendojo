#include "menu.hpp"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "autosave.hpp"
#include "commands.hpp"
#include "config.hpp"
#include "log.hpp"
#include "players.hpp"
#include "render_hook.hpp"
#include "slot.hpp"
#include "subsystems.hpp"
#include "theme.hpp"

#include <windows.h>

namespace opendojo::menu {

namespace {

constexpr const char* OPENDOJO_VERSION = "v0.1";

using clock = std::chrono::steady_clock;

struct ToastState {
    std::string text;
    bool is_error = false;
    clock::time_point until;
};

struct State {
    bool drills_dirty = true;
    std::vector<opendojo::commands::DrillHeader> drills;

    // Drills tab: filter rows whose `character` doesn't match the live CPU
    // character. Disabled automatically when no CPU is detected.
    bool show_all_drills = false;

    // Drills tab sort. Autosaves are always pinned to the top regardless;
    // this only controls how the regular drills below them are ordered.
    enum class Sort {
        Name,
        Newest,
    };
    Sort sort_mode = Sort::Name;

    // Export form buffers.
    char export_name[96] = "";
    char export_description[160] = "";

    // Set whenever the window transitions from hidden -> visible. Used
    // to claim window focus + set initial nav focus on the first frame
    // so keyboard nav can start without a mouse click.
    bool needs_focus = true;

    // Tab state for LB/RB cycling. `active_tab` tracks which tab is
    // currently drawn; `pending_tab` is set to -1 normally and to a
    // target index on the frame we want to force-select via
    // ImGuiTabItemFlags_SetSelected.
    int active_tab = 0;
    int pending_tab = -1;

    ToastState toast;
};

State g_state;

void show_toast(std::string text, bool is_error = false) {
    g_state.toast.text = std::move(text);
    g_state.toast.is_error = is_error;
    g_state.toast.until = clock::now() + std::chrono::seconds(5);
}

void refresh_drills_if_needed() {
    if (!g_state.drills_dirty) return;
    g_state.drills = opendojo::commands::list_drills();
    // Autosaves first, then the regular drills in the user-chosen order.
    std::sort(g_state.drills.begin(), g_state.drills.end(),
              [](const opendojo::commands::DrillHeader& a,
                 const opendojo::commands::DrillHeader& b) {
                  if (a.is_autosave != b.is_autosave) return a.is_autosave;
                  if (g_state.sort_mode == State::Sort::Newest) {
                      // Filesystem mtime; descending (newer first).
                      return a.mtime > b.mtime;
                  }
                  return a.name < b.name;
              });
    g_state.drills_dirty = false;
}

// ---- Tabs ------------------------------------------------------------------

void draw_drills_tab() {
    ImGui::TextDisabled("Drills found in opendojo/");
    ImGui::Spacing();

    auto cpu = opendojo::players::detect_cpu();
    const bool can_filter = cpu.detected && !g_state.show_all_drills;

    // Count visible vs total under the current filter.
    std::size_t visible = 0;
    if (can_filter) {
        for (const auto& d : g_state.drills) {
            if (d.character == cpu.character_name) ++visible;
        }
    } else {
        visible = g_state.drills.size();
    }

    if (ImGui::Button("Refresh")) g_state.drills_dirty = true;
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("Sort:");
    ImGui::SameLine();
    {
        int sort_idx = static_cast<int>(g_state.sort_mode);
        if (ImGui::RadioButton("Name", &sort_idx, static_cast<int>(State::Sort::Name))) {
            g_state.sort_mode = State::Sort::Name;
            g_state.drills_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Newest", &sort_idx, static_cast<int>(State::Sort::Newest))) {
            g_state.sort_mode = State::Sort::Newest;
            g_state.drills_dirty = true;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (cpu.detected) {
        ImGui::Checkbox("Show all", &g_state.show_all_drills);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (can_filter) {
            ImGui::TextDisabled("%zu of %zu drills (filtered to %s)", visible,
                                g_state.drills.size(), cpu.character_name.c_str());
        } else {
            ImGui::TextDisabled("%zu drills (CPU: %s)", g_state.drills.size(),
                                cpu.character_name.c_str());
        }
    } else {
        ImGui::TextDisabled("%zu drills (no CPU detected - filter disabled)",
                            g_state.drills.size());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_state.drills.empty()) {
        ImGui::TextDisabled("No drills saved yet. Use the Export tab to create one.");
        return;
    }

    // Split into autosaves (rendered first with distinct styling) and
    // regular drills (in a standard table).
    std::vector<std::size_t> autosave_idxs;
    std::vector<std::size_t> regular_idxs;
    for (std::size_t i = 0; i < g_state.drills.size(); ++i) {
        const auto& d = g_state.drills[i];
        if (can_filter && d.character != cpu.character_name) continue;
        if (d.is_autosave) {
            autosave_idxs.push_back(i);
        } else {
            regular_idxs.push_back(i);
        }
    }

    // ---- Autosaves block --------------------------------------------------
    if (!autosave_idxs.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "Auto-saves");
        ImGui::TextDisabled(
            "Overwritten every time you switch characters or leave practice. "
            "Click \"Save as drill\" to keep a copy.");
        ImGui::Spacing();
        for (std::size_t idx : autosave_idxs) {
            const auto& d = g_state.drills[idx];
            ImGui::PushID(static_cast<int>(idx));
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "%s", d.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu rec)", d.recording_count);
            ImGui::SameLine();
            if (ImGui::SmallButton("Add##autosave_add")) {
                auto r = opendojo::commands::load_drill(d.path,
                                                        opendojo::commands::LoadMode::AppendToFree);
                if (r.ok) opendojo::subsystems::mark_session_loaded(true);
                show_toast(r.message, !r.ok);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Replace##autosave_replace")) {
                auto r = opendojo::commands::load_drill(d.path,
                                                        opendojo::commands::LoadMode::ReplaceAll);
                if (r.ok) opendojo::subsystems::mark_session_loaded(true);
                show_toast(r.message, !r.ok);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Save as drill##autosave_save")) {
                std::string new_name = d.character.empty() ? "saved" : d.character + "_saved";
                auto r = opendojo::commands::copy_drill(d.path, new_name);
                show_toast(r.message, !r.ok);
                if (r.ok) g_state.drills_dirty = true;
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ---- Regular drills table --------------------------------------------
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (regular_idxs.empty()) {
        if (!autosave_idxs.empty()) {
            ImGui::TextDisabled("No saved drills yet — use Export or \"Save as drill\" above.");
        }
    } else if (ImGui::BeginTable("drills", 5, flags, ImVec2(0, 320))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Recordings", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        // Scale-aware column widths: compute the pixel width of the button
        // label at the current font size + frame padding, since ImGui's
        // WidthFixed takes raw pixels (not scaled).
        const auto pad = ImGui::GetStyle().FramePadding.x;
        const float add_w = ImGui::CalcTextSize("Add").x + pad * 4;
        const float repl_w = ImGui::CalcTextSize("Replace").x + pad * 4;
        ImGui::TableSetupColumn("Add", ImGuiTableColumnFlags_WidthFixed, add_w);
        ImGui::TableSetupColumn("Replace", ImGuiTableColumnFlags_WidthFixed, repl_w);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t idx : regular_idxs) {
            const auto& d = g_state.drills[idx];
            ImGui::PushID(static_cast<int>(idx));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(d.name.c_str());
            if (!d.description.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", d.description.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            if (!d.cpu_side.empty()) {
                ImGui::Text("%s (%s)", d.character.c_str(), d.cpu_side.c_str());
            } else {
                ImGui::TextUnformatted(d.character.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%zu", d.recording_count);

            ImGui::TableSetColumnIndex(3);
            if (ImGui::Button("Add##add", ImVec2(-1, 0))) {
                auto r = opendojo::commands::load_drill(d.path,
                                                        opendojo::commands::LoadMode::AppendToFree);
                if (r.ok) opendojo::subsystems::mark_session_loaded(true);
                show_toast(r.message, !r.ok);
            }

            ImGui::TableSetColumnIndex(4);
            if (ImGui::Button("Replace##replace", ImVec2(-1, 0))) {
                auto r = opendojo::commands::load_drill(d.path,
                                                        opendojo::commands::LoadMode::ReplaceAll);
                if (r.ok) opendojo::subsystems::mark_session_loaded(true);
                show_toast(r.message, !r.ok);
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (can_filter && visible == 0 && !g_state.drills.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
                           "No drills match %s. Toggle \"Show all\" to see every drill.",
                           cpu.character_name.c_str());
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Add: load into empty recording slots (refuses if too few are free).");
    ImGui::TextDisabled("Replace: clear all recordings, then load from the drill.");
}

void draw_export_tab() {
    ImGui::TextDisabled("Save current recordings as a shareable drill file.");
    ImGui::Spacing();

    // We can export whenever the gameplay subsystem is reachable AND
    // at least one slot is populated. Live recordings require pool1
    // to be allocated, but move-list slots live in the recordpool (a
    // different subsystem) and don't depend on pool1.
    std::size_t populated = 0;
    for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
        if (opendojo::slot::is_populated(i)) ++populated;
    }
    if (populated == 0) {
        ImGui::TextColored(
            ImVec4(1, 0.7f, 0.3f, 1),
            "No recordings to export. Record a move or select one from the move list first.");
        return;
    }
    ImGui::Text("Recordings ready to export: %zu / %zu", populated, opendojo::slot::USER_SLOTS);

    auto cpu = opendojo::players::detect_cpu();
    if (cpu.detected) {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.65f, 1), "Detected: %s (CPU on %s)",
                           cpu.character_name.c_str(),
                           opendojo::players::side_to_string(cpu.cpu_side));
    } else {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
                           "Detected: not in a match (character/side will be \"unknown\")");
    }
    ImGui::Spacing();

    ImGui::PushItemWidth(420);
    ImGui::InputText("Name", g_state.export_name, sizeof(g_state.export_name));
    ImGui::InputText("Description", g_state.export_description, sizeof(g_state.export_description));
    ImGui::PopItemWidth();

    ImGui::TextDisabled("Leave name blank for timestamp.");

    ImGui::Spacing();

    const bool can_export = populated > 0;
    if (!can_export) ImGui::BeginDisabled();
    if (ImGui::Button("Export", ImVec2(0, 0))) {
        auto r = opendojo::commands::export_current_slots(g_state.export_name,
                                                          g_state.export_description,
                                                          "" /* character: always autodetected */,
                                                          "" /* cpu_side: always use detection */);
        show_toast(r.message, !r.ok);
        if (r.ok) {
            g_state.export_name[0] = 0;
            g_state.export_description[0] = 0;
            g_state.drills_dirty = true;
        }
    }
    if (!can_export) ImGui::EndDisabled();
}

void draw_status_tab() {
    // Practice-mode state is determined by the gameplay subsystem being
    // reachable. pool1 only allocates after the first live recording —
    // gating on it would hide existing movelist slots (which live in
    // the recordpool subsystem and are independently allocated).
    const bool in_practice = opendojo::subsystems::lookup(opendojo::subsystems::KEY_GAMEPLAY) != 0;

    ImGui::Text("Status: ");
    ImGui::SameLine();
    if (in_practice) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "ready");
    } else {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "not in practice mode");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("recordings", 3, flags)) {
        ImGui::TableSetupColumn("Recording", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Recording %zu", i + 1);
            ImGui::TableSetColumnIndex(1);
            if (!in_practice) {
                ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("-");
                continue;
            }
            auto k = opendojo::slot::kind(i);
            if (k == opendojo::slot::Kind::Empty) {
                ImGui::TextDisabled("empty");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("-");
            } else {
                ImGui::TextUnformatted(opendojo::slot::kind_name(k));
                ImGui::TableSetColumnIndex(2);
                if (k == opendojo::slot::Kind::MoveList) {
                    ImGui::TextUnformatted("saved");
                } else {
                    ImGui::Text("%u events", static_cast<unsigned>(opendojo::slot::event_count(i)));
                }
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Dump slot flags to log")) {
        opendojo::slot::dump_flag_state();
        show_toast("Dumped slot flag state to opendojo.log");
    }
}

// Render a user-readable label for a Win32 virtual-key code. Uses
// MapVirtualKey + GetKeyNameText for the OS-localized names so
// keyboards in any layout produce sensible labels. Falls back to
// "VK 0x??" for unmappable codes.
std::string vk_name(std::uint32_t vk) {
    char buf[64] = {};
    // Function keys + arrows have an "extended" bit Windows wants set
    // for proper naming.
    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    LONG lparam = (scan & 0xFF) << 16;
    switch (vk) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN: lparam |= (1 << 24); break;
    }
    if (GetKeyNameTextA(lparam, buf, sizeof(buf)) > 0 && buf[0]) { return buf; }
    std::snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
    return buf;
}

// Human label for an XInput button mask. Matches XINPUT_GAMEPAD_*
// values; the menu only ever shows one button at a time so we don't
// need to handle combined masks.
const char* pad_btn_name(std::uint16_t mask) {
    switch (mask) {
        case 0x1000: return "A";
        case 0x2000: return "B";
        case 0x4000: return "X";
        case 0x8000: return "Y";
        case 0x0010: return "Start";
        case 0x0020: return "Back";
        case 0x0001: return "DPad Up";
        case 0x0002: return "DPad Down";
        case 0x0004: return "DPad Left";
        case 0x0008: return "DPad Right";
        case 0x0100: return "LB";
        case 0x0200: return "RB";
        case 0x0040: return "LS";
        case 0x0080: return "RS";
        default: return "(unbound)";
    }
}

void draw_settings_tab() {
    ImGui::TextDisabled("Persisted to opendojo/config.json");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    {
        bool en = opendojo::autosave::is_enabled();
        if (ImGui::Checkbox("Autosave / autoload per character", &en)) {
            opendojo::autosave::set_enabled(en);
        }
        ImGui::TextDisabled(
            "Snapshots your current recordings per CPU character on every\n"
            "character switch or practice exit. Reloads on return. The\n"
            "autosave file always overwrites the previous one.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Menu toggle key");
    ImGui::Spacing();

    const auto vk = opendojo::config::toggle_vk();
    ImGui::Text("Current binding: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.65f, 1), "%s", vk_name(vk).c_str());

    ImGui::Spacing();

    // Capture state owned by config.* / WndProc - we can't poll
    // GetAsyncKeyState here because the keyboard-suppression hook
    // returns 0 for everything while the menu is up.
    const bool capturing = opendojo::config::is_capturing();
    if (!capturing) {
        if (ImGui::Button("Rebind...", ImVec2(0, 0))) { opendojo::config::start_capture(); }
        ImGui::SameLine();
        if (ImGui::Button("Reset to F12", ImVec2(0, 0))) {
            opendojo::config::set_toggle_vk(VK_F12);
            show_toast("Toggle key reset to F12");
        }
    } else {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "Press any key... (Esc cancels)");
        // The WndProc subclass captures the next WM_KEYDOWN VK into
        // a shared atomic; we just check / consume it here.
        auto pressed = opendojo::config::consume_captured_vk();
        if (pressed != 0) {
            opendojo::config::set_toggle_vk(pressed);
            show_toast(std::string("Toggle key bound to ") + vk_name(pressed));
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Controller toggle chord");
    ImGui::Spacing();

    const auto pad_btn = opendojo::config::toggle_pad_btn();
    ImGui::Text("Current binding: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.65f, 1), "Back + %s", pad_btn_name(pad_btn));

    ImGui::Spacing();

    const bool pad_capturing = opendojo::config::is_pad_capturing();
    if (!pad_capturing) {
        if (ImGui::Button("Rebind controller...", ImVec2(0, 0))) {
            opendojo::config::start_pad_capture();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to Y", ImVec2(0, 0))) {
            opendojo::config::set_toggle_pad_btn(0x8000);  // XINPUT_GAMEPAD_Y
            show_toast("Controller chord reset to Back + Y");
        }
    } else {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1),
                           "Press any controller button... (Back cancels)");
        auto pressed_btn = opendojo::config::consume_captured_pad_btn();
        if (pressed_btn != 0) {
            opendojo::config::set_toggle_pad_btn(pressed_btn);
            show_toast(std::string("Controller chord bound to Back + ") +
                       pad_btn_name(pressed_btn));
        }
    }
}

void draw_about_tab() {
    ImGui::Text("OpenDojo %s - Tekken 8 practice-mode drill tool", OPENDOJO_VERSION);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Save and share practice-mode recordings as text drill files. "
        "Each drill contains one or more recordings; loading places them "
        "into the in-game recording slots.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Toggle this menu: %s  or  Back + %s  (both rebindable)",
                vk_name(opendojo::config::toggle_vk()).c_str(),
                pad_btn_name(opendojo::config::toggle_pad_btn()));
    ImGui::Spacing();
    ImGui::TextDisabled("Drill files + config live in opendojo/ next to the game executable.");
}

// ---- Toast (transient bottom-of-window status line) ------------------------

void draw_toast() {
    if (g_state.toast.text.empty()) return;
    if (clock::now() > g_state.toast.until) {
        g_state.toast.text.clear();
        return;
    }
    ImGui::Spacing();
    const ImVec4 col = g_state.toast.is_error ? ImVec4(1.0f, 0.55f, 0.40f, 1.0f)
                                              : ImVec4(0.55f, 0.95f, 0.65f, 1.0f);
    ImGui::TextColored(col, "%s", g_state.toast.text.c_str());
}

}  // anonymous namespace

void invalidate() {
    g_state.drills_dirty = true;
    // Window just became visible: reclaim focus + initial nav target.
    g_state.needs_focus = true;
}

void draw() {
    refresh_drills_if_needed();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    // Size as a fraction of the display so the menu remains readable
    // at every resolution. Clamped so it doesn't get absurdly small
    // on tiny windows or absurdly large on ultrawide.
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    const ImVec2 size(clampf(vp->WorkSize.x * 0.50f, 900.0f, 1400.0f),
                      clampf(vp->WorkSize.y * 0.60f, 520.0f, 1000.0f));
    const ImVec2 pos(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f,
                     vp->WorkPos.y + (vp->WorkSize.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);

    if (g_state.needs_focus) {
        // SetNextWindowFocus only takes effect once Begin runs. Pair it
        // with SetItemDefaultFocus below on the first interactive widget
        // so the keyboard nav cursor starts somewhere visible.
        ImGui::SetNextWindowFocus();
    }

    const ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;
    char title[64];
    std::snprintf(title, sizeof(title), "OpenDojo %s###opendojo", OPENDOJO_VERSION);

    // Pass an `open` bool so ImGui draws the X close button. We treat
    // a click on X identically to the toggle hotkey.
    bool open = true;
    if (!ImGui::Begin(title, &open, wflags)) {
        ImGui::End();
        if (!open) opendojo::render_hook::toggle_menu();
        return;
    }

    // Gamepad L1/R1 cycle through tabs (rising-edge). The events were
    // fed earlier this frame by render_hook::poll_gamepad, so checking
    // here picks them up in time for the tab bar to honor SetSelected.
    struct TabDef {
        const char* name;
        void (*draw)();
    };
    const TabDef tabs[] = {
        {"Drills", &draw_drills_tab}, {"Export", &draw_export_tab},
        {"Status", &draw_status_tab}, {"Settings", &draw_settings_tab},
        {"About", &draw_about_tab},
    };
    constexpr int kTabCount = static_cast<int>(sizeof(tabs) / sizeof(tabs[0]));

    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
        g_state.pending_tab = (g_state.active_tab - 1 + kTabCount) % kTabCount;
    } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
        g_state.pending_tab = (g_state.active_tab + 1) % kTabCount;
    }

    if (ImGui::BeginTabBar("##tabs")) {
        for (int i = 0; i < kTabCount; ++i) {
            ImGuiTabItemFlags flags = 0;
            if (i == g_state.pending_tab) flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem(tabs[i].name, nullptr, flags)) {
                g_state.active_tab = i;
                // First-frame focus on the initial tab: anchor the nav
                // cursor so keyboard/gamepad can move around immediately.
                if (g_state.needs_focus && i == 0) ImGui::SetKeyboardFocusHere();
                tabs[i].draw();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    g_state.pending_tab = -1;

    draw_toast();
    ImGui::End();

    if (!open) opendojo::render_hook::toggle_menu();

    g_state.needs_focus = false;
}

}  // namespace opendojo::menu
