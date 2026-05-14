#include "menu.hpp"

#include "imgui.h"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "autosave.hpp"
#include "commands.hpp"
#include "log.hpp"
#include "players.hpp"
#include "slot.hpp"
#include "subsystems.hpp"
#include "theme.hpp"

namespace opendojo::menu {

namespace {

using clock = std::chrono::steady_clock;

struct ToastState {
    std::string text;
    bool        is_error = false;
    clock::time_point until;
};

struct State {
    bool                                       drills_dirty = true;
    std::vector<opendojo::commands::DrillHeader> drills;

    // Drills tab: filter rows whose `character` doesn't match the live CPU
    // character. Disabled automatically when no CPU is detected.
    bool show_all_drills = false;

    // Export form buffers.
    char export_name[96]        = "";
    char export_description[160] = "";
    char export_character[32]   = "";

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
    ImGui::TextDisabled("Drills found in opendojo_drills/");
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
            ImGui::TextDisabled("%zu of %zu drills (filtered to %s)",
                                visible, g_state.drills.size(), cpu.character_name.c_str());
        } else {
            ImGui::TextDisabled("%zu drills (CPU: %s)",
                                g_state.drills.size(), cpu.character_name.c_str());
        }
    } else {
        ImGui::TextDisabled("%zu drills (no CPU detected — filter disabled)",
                            g_state.drills.size());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_state.drills.empty()) {
        ImGui::TextDisabled("No drills saved yet. Use the Export tab to create one.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders
                                | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_SizingStretchProp
                                | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("drills", 5, flags, ImVec2(0, 320))) {
        ImGui::TableSetupColumn("Name",        ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Character",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Recordings",  ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Add",         ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Replace",     ImGuiTableColumnFlags_WidthFixed, 80.0f);
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
            if (ImGui::Button("Add", ImVec2(-1, 0))) {
                auto r = opendojo::commands::load_drill(d.path,
                            opendojo::commands::LoadMode::AppendToFree);
                show_toast(r.message, !r.ok);
            }

            ImGui::TableSetColumnIndex(4);
            if (ImGui::Button("Replace", ImVec2(-1, 0))) {
                auto r = opendojo::commands::load_drill(d.path,
                            opendojo::commands::LoadMode::ReplaceAll);
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
    ImGui::TextDisabled(
        "Add: fills empty slots in order (refuses if too few free).  "
        "Replace: clears all slots first.");
}

void draw_export_tab() {
    ImGui::TextDisabled("Save currently-recorded slots as a shareable drill file.");
    ImGui::Spacing();

    if (!opendojo::subsystems::pool1()) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
            "Pool1 not allocated yet. Enter practice mode and record once to initialize.");
        return;
    }

    std::size_t populated = 0;
    for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
        if (opendojo::slot::event_count(i) > 0) ++populated;
    }
    ImGui::Text("Slots with recordings: %zu / %zu", populated, opendojo::slot::USER_SLOTS);

    auto cpu = opendojo::players::detect_cpu();
    if (cpu.detected) {
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.65f, 1),
            "Detected: %s (CPU on %s, id=%u)",
            cpu.character_name.c_str(),
            opendojo::players::side_to_string(cpu.cpu_side),
            static_cast<unsigned>(cpu.character_id));
    } else {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
            "Detected: not in a match (character/side will be \"unknown\")");
    }
    ImGui::Spacing();

    ImGui::PushItemWidth(420);
    ImGui::InputText("Name",        g_state.export_name,        sizeof(g_state.export_name));
    ImGui::InputText("Description", g_state.export_description, sizeof(g_state.export_description));
    ImGui::InputText("Character",   g_state.export_character,   sizeof(g_state.export_character));
    ImGui::PopItemWidth();

    ImGui::TextDisabled(
        "Name -> filename slug. Leave blank for timestamp.\n"
        "Character: blank = use detected value above. Free-form lowercase tag overrides.");

    ImGui::Spacing();

    const bool can_export = populated > 0;
    if (!can_export) ImGui::BeginDisabled();
    if (ImGui::Button("Export", ImVec2(160, 0))) {
        auto r = opendojo::commands::export_current_slots(
            g_state.export_name,
            g_state.export_description,
            g_state.export_character,
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
    auto p1 = opendojo::subsystems::pool1();

    ImGui::Text("Pool1: ");
    ImGui::SameLine();
    if (p1) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "ready (0x%llX)",
                               static_cast<unsigned long long>(p1));
    else    ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
                               "not allocated — record once in practice mode");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    {
        bool en = opendojo::autosave::is_enabled();
        if (ImGui::Checkbox("Autosave/autoload per character", &en)) {
            opendojo::autosave::set_enabled(en);
        }
        ImGui::TextDisabled(
            "Saves your slot contents per CPU character when you switch chars or\n"
            "leave practice. Restores them when you load that character again.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders
                                | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("slots", 2, flags)) {
        ImGui::TableSetupColumn("Slot",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Events", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu", i + 1);
            ImGui::TableSetColumnIndex(1);
            if (!p1) {
                ImGui::TextDisabled("—");
                continue;
            }
            auto n = opendojo::slot::event_count(i);
            if (n == 0) ImGui::TextDisabled("empty");
            else        ImGui::Text("%u", static_cast<unsigned>(n));
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Dump to log")) opendojo::commands::show_status();
}

void draw_about_tab() {
    ImGui::TextUnformatted("OpenDojo — Tekken 8 practice-mode drill tool");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Save and share practice-mode recordings as text drill files. "
        "Each drill contains one or more recordings; loading places them "
        "into the in-game slots so you can practice scenarios offline.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Toggle this menu: F12");
    ImGui::Spacing();
    ImGui::TextDisabled(
        "v2 format. Hotkeys are disabled in this build while the menu UX "
        "is being designed. Drill files live in opendojo_drills/ next to "
        "the game executable.");
}

// ---- Toast (transient bottom-of-window status line) ------------------------

void draw_toast() {
    if (g_state.toast.text.empty()) return;
    if (clock::now() > g_state.toast.until) {
        g_state.toast.text.clear();
        return;
    }
    ImGui::Spacing();
    const ImVec4 col = g_state.toast.is_error
        ? ImVec4(1.0f, 0.55f, 0.40f, 1.0f)
        : ImVec4(0.55f, 0.95f, 0.65f, 1.0f);
    ImGui::TextColored(col, "%s", g_state.toast.text.c_str());
}

}  // anonymous namespace

void invalidate() {
    g_state.drills_dirty = true;
}

void draw() {
    refresh_drills_if_needed();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(720, 520);
    const ImVec2 pos(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f,
                     vp->WorkPos.y + (vp->WorkSize.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(pos,   ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);

    const ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("OPENDOJO", nullptr, wflags)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Drills"))  { draw_drills_tab();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Export"))  { draw_export_tab();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Status"))  { draw_status_tab();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("About"))   { draw_about_tab();   ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    draw_toast();
    ImGui::End();
}

}  // namespace opendojo::menu
