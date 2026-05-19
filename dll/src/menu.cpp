#include "menu.hpp"

#include "imgui.h"

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

    // Export form buffers.
    char export_name[96] = "";
    char export_description[160] = "";

    // Set whenever the window transitions from hidden -> visible. Used
    // to claim window focus + set initial nav focus on the first frame
    // so keyboard nav can start without a mouse click.
    bool needs_focus = true;

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

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("drills", 5, flags, ImVec2(0, 320))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Recordings", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Add", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Replace", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < g_state.drills.size(); ++i) {
            const auto& d = g_state.drills[i];
            if (can_filter && d.character != cpu.character_name) continue;
            ImGui::PushID(static_cast<int>(i));
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

    if (!opendojo::subsystems::pool1()) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
                           "Not ready. Enter practice mode and record once to initialize.");
        return;
    }

    std::size_t populated = 0;
    for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
        if (opendojo::slot::is_populated(i)) ++populated;
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

    ImGui::TextDisabled(
        "Name -> filename slug. Leave blank for timestamp.\n"
        "Character is autodetected from the live CPU player.");

    ImGui::Spacing();

    const bool can_export = populated > 0;
    if (!can_export) ImGui::BeginDisabled();
    if (ImGui::Button("Export", ImVec2(160, 0))) {
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
    const bool ready = opendojo::subsystems::pool1() != 0;

    ImGui::Text("Status: ");
    ImGui::SameLine();
    if (ready) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "ready");
    } else {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "not ready - record once in practice mode");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    {
        bool en = opendojo::autosave::is_enabled();
        if (ImGui::Checkbox("Autosave/autoload per character", &en)) {
            opendojo::autosave::set_enabled(en);
        }
        ImGui::TextDisabled(
            "Saves your recordings per CPU character when you switch chars or\n"
            "leave practice. Restores them when you load that character again.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("recordings", 2, flags)) {
        ImGui::TableSetupColumn("Recording", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Events", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Recording %zu", i + 1);
            ImGui::TableSetColumnIndex(1);
            if (!ready) {
                ImGui::TextDisabled("-");
                continue;
            }
            if (!opendojo::slot::is_populated(i)) {
                ImGui::TextDisabled("empty");
            } else {
                auto n = opendojo::slot::event_count(i);
                ImGui::Text("%u", static_cast<unsigned>(n));
            }
        }
        ImGui::EndTable();
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

void draw_settings_tab() {
    ImGui::TextDisabled("Persisted to opendojo/config.json");
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
        if (ImGui::Button("Rebind...", ImVec2(160, 0))) { opendojo::config::start_capture(); }
        ImGui::SameLine();
        if (ImGui::Button("Reset to F12", ImVec2(160, 0))) {
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
}

void draw_about_tab() {
    ImGui::Text("OpenDojo %s - Tekken 8 practice-mode drill tool", OPENDOJO_VERSION);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Save and share practice-mode recordings as text drill files. "
        "Each drill contains one or more recordings; loading places them "
        "into the in-game recording slots so you can practice scenarios offline.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Toggle this menu: %s  (rebindable in Settings tab)",
                vk_name(opendojo::config::toggle_vk()).c_str());
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

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Drills")) {
            // First-frame focus: anchor the nav cursor inside the tab
            // so keyboard can move around immediately.
            if (g_state.needs_focus) ImGui::SetKeyboardFocusHere();
            draw_drills_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Export")) {
            draw_export_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Status")) {
            draw_status_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            draw_settings_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            draw_about_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    draw_toast();
    ImGui::End();

    if (!open) opendojo::render_hook::toggle_menu();

    g_state.needs_focus = false;
}

}  // namespace opendojo::menu
