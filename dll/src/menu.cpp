#include "menu.hpp"

#include "autosave.hpp"
#include "commands.hpp"
#include "config.hpp"
#include "log.hpp"
#include "players.hpp"
#include "render_hook.hpp"
#include "rmlui_backend.hpp"
#include "slot.hpp"
#include "subsystems.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace opendojo::menu {

namespace {

using clock = std::chrono::steady_clock;

struct ToastState {
    std::string text;
    bool        is_error = false;
    clock::time_point until;
};

enum class Section { Drills, Export, Status, Settings, About };

struct State {
    bool wired = false;
    Section section = Section::Drills;

    // Drill list state.
    bool drills_dirty = true;
    bool drill_dom_dirty = true;
    std::vector<opendojo::commands::DrillHeader> drills;
    bool show_all_drills = false;

    ToastState toast;
};

State g;

// Map Section -> (nav element id, content section id).
struct SectionEntry { Section s; const char* nav; const char* sec; };
const SectionEntry kSections[] = {
    { Section::Drills,   "nav-drills",   "section-drills"   },
    { Section::Export,   "nav-export",   "section-export"   },
    { Section::Status,   "nav-status",   "section-status"   },
    { Section::Settings, "nav-settings", "section-settings" },
    { Section::About,    "nav-about",    "section-about"    },
};

Rml::Element* el(const char* id) {
    auto* doc = opendojo::rml_backend::document();
    if (!doc) return nullptr;
    return doc->GetElementById(id);
}

std::string vk_name(std::uint32_t vk) {
    char buf[64] = {};
    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    LONG lparam = (scan & 0xFF) << 16;
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
        case VK_UP:     case VK_DOWN:
            lparam |= (1 << 24); break;
    }
    if (GetKeyNameTextA(lparam, buf, sizeof(buf)) > 0 && buf[0]) return buf;
    std::snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
    return buf;
}

void show_toast(std::string text, bool is_error = false) {
    g.toast.text = std::move(text);
    g.toast.is_error = is_error;
    g.toast.until = clock::now() + std::chrono::seconds(5);
}

void apply_section() {
    for (const auto& s : kSections) {
        auto* nav = el(s.nav);
        if (nav) nav->SetClass("selected", s.s == g.section);
        auto* sec = el(s.sec);
        if (sec) sec->SetClass("hidden",   s.s != g.section);
    }
    // Update help strip text.
    const char* help = "Browse, load and replace drill files saved by OpenDojo.";
    switch (g.section) {
        case Section::Drills:   help = "Browse, load and replace drill files saved by OpenDojo."; break;
        case Section::Export:   help = "Save currently-recorded slots as a shareable drill file."; break;
        case Section::Status:   help = "Diagnostic snapshot of pool1 and per-slot occupancy."; break;
        case Section::Settings: help = "Configure the menu toggle hotkey."; break;
        case Section::About:    help = "About OpenDojo. Drill files live in opendojo/ next to the game exe."; break;
    }
    if (auto* s = el("strip-text")) s->SetInnerRML(help);
}

// Lightweight HTML escaper for drill names / descriptions. RmlUi treats
// the RML we shove in as parsable; we want to keep raw user text safe.
std::string esc(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            default:  out += c;        break;
        }
    }
    return out;
}

void refresh_drills_if_needed() {
    if (!g.drills_dirty) return;
    g.drills = opendojo::commands::list_drills();
    g.drills_dirty = false;
    g.drill_dom_dirty = true;
}

void rebuild_drill_list() {
    auto* host = el("drill-list");
    if (!host) return;

    auto cpu = opendojo::players::detect_cpu();
    const bool can_filter = cpu.detected && !g.show_all_drills;

    std::size_t visible = 0;
    std::string rml;
    rml.reserve(g.drills.size() * 256 + 256);

    for (std::size_t i = 0; i < g.drills.size(); ++i) {
        const auto& d = g.drills[i];
        if (can_filter && d.character != cpu.character_name) continue;
        ++visible;
        char idxbuf[32];
        std::snprintf(idxbuf, sizeof(idxbuf), "%zu", i);

        std::string meta = esc(d.character);
        if (!d.cpu_side.empty()) {
            meta += " (";
            meta += esc(d.cpu_side);
            meta += ")";
        }
        char rec_buf[32];
        std::snprintf(rec_buf, sizeof(rec_buf), " · %zu rec", d.recording_count);
        meta += rec_buf;

        rml += "<div class='row drill-row' data-drill-index='";
        rml += idxbuf;
        rml += "'>";
        rml += "<span class='drill-name'>";
        rml += esc(d.name);
        rml += "</span>";
        rml += "<span class='drill-meta'>";
        rml += meta;
        rml += "</span>";
        rml += "<span class='drill-actions'>";
        rml += "<div class='drill-btn add' data-action='add' data-drill-index='";
        rml += idxbuf;
        rml += "'>Add</div>";
        rml += "<div class='drill-btn replace' data-action='replace' data-drill-index='";
        rml += idxbuf;
        rml += "'>Replace</div>";
        rml += "</span></div>";
    }

    if (visible == 0) {
        rml += "<div class='row drill-empty'>";
        rml += "<span class='label'>";
        if (g.drills.empty()) {
            rml += "No drills found in opendojo/. Use Export to create one.";
        } else if (can_filter) {
            rml += "No drills match ";
            rml += esc(cpu.character_name);
            rml += ". Toggle filter Off to see all.";
        } else {
            rml += "No drills visible.";
        }
        rml += "</span><span class='value unset'>Empty</span></div>";
    }

    host->SetInnerRML(rml);

    // Meta line above the list.
    if (auto* mt = el("drill-meta-text")) {
        char buf[160];
        if (cpu.detected) {
            std::snprintf(buf, sizeof(buf), "CPU: %s · %zu drills total",
                          cpu.character_name.c_str(), g.drills.size());
        } else {
            std::snprintf(buf, sizeof(buf), "%zu drills total · no CPU detected",
                          g.drills.size());
        }
        mt->SetInnerRML(buf);
    }
    if (auto* mf = el("drill-meta-filter")) {
        mf->SetInnerRML(can_filter ? "Filter: ON" : "Filter: OFF");
    }
    if (auto* btn = el("btn-filter")) {
        btn->SetInnerRML(g.show_all_drills ? "Off" : "On");
    }
}

void refresh_export_section() {
    if (auto* p = el("export-populated")) {
        std::size_t populated = 0;
        if (opendojo::subsystems::pool1()) {
            for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
                if (opendojo::slot::is_populated(i)) ++populated;
            }
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%zu / %zu",
                      populated, opendojo::slot::USER_SLOTS);
        p->SetInnerRML(buf);
    }
    auto cpu = opendojo::players::detect_cpu();
    if (auto* c = el("export-character")) {
        c->SetInnerRML(cpu.detected ? esc(cpu.character_name) : "Unknown");
        c->SetClass("unset", !cpu.detected);
    }
    if (auto* s = el("export-side")) {
        s->SetInnerRML(cpu.detected
            ? opendojo::players::side_to_string(cpu.cpu_side)
            : "—");
    }
}

void refresh_status_section() {
    auto p1 = opendojo::subsystems::pool1();
    if (auto* p = el("status-pool")) {
        if (p1) {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "0x%llX",
                          (unsigned long long)p1);
            p->SetInnerRML(buf);
            p->SetClass("set",   true);
            p->SetClass("unset", false);
        } else {
            p->SetInnerRML("Not allocated");
            p->SetClass("set",   false);
            p->SetClass("unset", true);
        }
    }
    if (auto* b = el("btn-autosave")) {
        b->SetInnerRML(opendojo::autosave::is_enabled() ? "On" : "Off");
    }
    if (auto* host = el("slot-list")) {
        std::string rml;
        for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
            char idx[16]; std::snprintf(idx, sizeof(idx), "%zu", i + 1);
            rml += "<div class='row'><span class='label'>Slot ";
            rml += idx;
            rml += "</span><span class='value ";
            if (!p1 || !opendojo::slot::is_populated(i)) {
                rml += "unset'>empty";
            } else {
                rml += "set'>";
                char cbuf[16];
                std::snprintf(cbuf, sizeof(cbuf), "%u",
                              (unsigned)opendojo::slot::event_count(i));
                rml += cbuf;
                rml += " events";
            }
            rml += "</span></div>";
        }
        host->SetInnerRML(rml);
    }
}

void refresh_settings_section() {
    if (auto* h = el("settings-hotkey")) {
        const auto vk = opendojo::config::toggle_vk();
        if (opendojo::config::is_capturing()) {
            h->SetInnerRML("Press any key… (Esc cancels)");
            h->SetClass("unset", true);
        } else {
            h->SetInnerRML(esc(vk_name(vk)).c_str());
            h->SetClass("unset", false);
        }
    }
    if (auto* r = el("btn-rebind")) {
        r->SetInnerRML(opendojo::config::is_capturing() ? "Cancel" : "Rebind…");
    }

    // Drain pending captured VK so the user sees their new key reflected.
    auto pressed = opendojo::config::consume_captured_vk();
    if (pressed != 0) {
        opendojo::config::set_toggle_vk(pressed);
        show_toast(std::string("Toggle bound to ") + vk_name(pressed));
    }
}

void refresh_toast() {
    auto* t = el("toast");
    if (!t) return;
    if (g.toast.text.empty() || clock::now() > g.toast.until) {
        if (!g.toast.text.empty()) g.toast.text.clear();
        t->SetInnerRML("");
        t->SetClass("error", false);
        return;
    }
    t->SetInnerRML(esc(g.toast.text));
    t->SetClass("error", g.toast.is_error);
}

// ---- Event listener ---------------------------------------------------------

class ClickListener : public Rml::EventListener {
public:
    explicit ClickListener(std::string action) : action_(std::move(action)) {}

    void ProcessEvent(Rml::Event& ev) override {
        if (action_ == "select_drills")   { g.section = Section::Drills;   apply_section(); g.drill_dom_dirty = true; }
        else if (action_ == "select_export")   { g.section = Section::Export;   apply_section(); refresh_export_section(); }
        else if (action_ == "select_status")   { g.section = Section::Status;   apply_section(); refresh_status_section(); }
        else if (action_ == "select_settings") { g.section = Section::Settings; apply_section(); refresh_settings_section(); }
        else if (action_ == "select_about")    { g.section = Section::About;    apply_section(); }
        else if (action_ == "close")           { opendojo::render_hook::toggle_menu(); }
        else if (action_ == "refresh") {
            g.drills_dirty    = true;
            g.drill_dom_dirty = true;
            show_toast("Drill list refreshed");
        }
        else if (action_ == "toggle_filter") {
            g.show_all_drills = !g.show_all_drills;
            g.drill_dom_dirty = true;
        }
        else if (action_ == "export") {
            std::string name, desc;
            if (auto* e = el("export-name")) {
                if (auto* in = dynamic_cast<Rml::ElementFormControlInput*>(e)) {
                    name = in->GetValue();
                }
            }
            if (auto* e = el("export-desc")) {
                if (auto* in = dynamic_cast<Rml::ElementFormControlInput*>(e)) {
                    desc = in->GetValue();
                }
            }
            auto r = opendojo::commands::export_current_slots(name, desc, "", "");
            show_toast(r.message, !r.ok);
            if (r.ok) {
                if (auto* e = el("export-name"))
                    if (auto* in = dynamic_cast<Rml::ElementFormControlInput*>(e))
                        in->SetValue("");
                if (auto* e = el("export-desc"))
                    if (auto* in = dynamic_cast<Rml::ElementFormControlInput*>(e))
                        in->SetValue("");
                g.drills_dirty = true;
                g.drill_dom_dirty = true;
            }
        }
        else if (action_ == "autosave") {
            const bool en = !opendojo::autosave::is_enabled();
            opendojo::autosave::set_enabled(en);
            refresh_status_section();
            show_toast(en ? "Autosave: On" : "Autosave: Off");
        }
        else if (action_ == "rebind") {
            if (opendojo::config::is_capturing()) {
                opendojo::config::cancel_capture();
            } else {
                opendojo::config::start_capture();
            }
            refresh_settings_section();
        }
        else if (action_ == "reset_hotkey") {
            opendojo::config::set_toggle_vk(VK_F12);
            refresh_settings_section();
            show_toast("Toggle reset to F12");
        }
        else if (action_ == "drill_add" || action_ == "drill_replace") {
            // Index encoded in data-drill-index on the clicked element.
            auto* el_target = ev.GetTargetElement();
            if (!el_target) return;
            Rml::String idx_str;
            if (!el_target->GetAttribute<Rml::String>("data-drill-index", "").empty()) {
                idx_str = el_target->GetAttribute<Rml::String>("data-drill-index", "");
            }
            if (idx_str.empty()) return;
            std::size_t idx = (std::size_t)std::strtoul(idx_str.c_str(), nullptr, 10);
            if (idx >= g.drills.size()) return;
            const auto& d = g.drills[idx];
            using LM = opendojo::commands::LoadMode;
            auto mode = action_ == "drill_add" ? LM::AppendToFree : LM::ReplaceAll;
            auto r = opendojo::commands::load_drill(d.path, mode);
            if (r.ok) opendojo::subsystems::mark_session_loaded(true);
            show_toast(r.message, !r.ok);
        }
    }

private:
    std::string action_;
};

// Listeners are owned by this static array; we add them once and never
// remove. Lifetime = process lifetime.
std::vector<ClickListener*> g_listeners;

void attach(const char* id, const char* action) {
    auto* e = el(id);
    if (!e) return;
    auto* l = new ClickListener(action);
    g_listeners.push_back(l);
    e->AddEventListener("click", l);
}

// Drill rows are recreated on every list rebuild, so we attach a single
// listener on the host element and use event bubbling: read the
// data-action attribute on the clicked target.
class DelegatingListener : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& ev) override {
        auto* tgt = ev.GetTargetElement();
        if (!tgt) return;
        Rml::String action = tgt->GetAttribute<Rml::String>("data-action", "");
        if (action.empty()) return;
        ClickListener tmp(std::string("drill_") + action.c_str());
        tmp.ProcessEvent(ev);
    }
};
DelegatingListener g_drill_listener;

void wire_once() {
    if (g.wired) return;
    auto* doc = opendojo::rml_backend::document();
    if (!doc) return;
    g.wired = true;

    attach("nav-drills",   "select_drills");
    attach("nav-export",   "select_export");
    attach("nav-status",   "select_status");
    attach("nav-settings", "select_settings");
    attach("nav-about",    "select_about");
    attach("nav-close",    "close");

    attach("btn-refresh",  "refresh");
    attach("btn-filter",   "toggle_filter");
    attach("btn-export",   "export");
    attach("btn-autosave", "autosave");
    attach("btn-rebind",   "rebind");
    attach("btn-reset-hotkey", "reset_hotkey");

    if (auto* host = el("drill-list")) {
        host->AddEventListener("click", &g_drill_listener);
    }

    apply_section();
}

}  // anonymous namespace

void invalidate() {
    g.drills_dirty    = true;
    g.drill_dom_dirty = true;
}

void draw() {
    if (!opendojo::rml_backend::document()) return;
    wire_once();
    refresh_drills_if_needed();

    if (g.drill_dom_dirty) {
        rebuild_drill_list();
        g.drill_dom_dirty = false;
    }
    if (g.section == Section::Export)   refresh_export_section();
    if (g.section == Section::Status)   refresh_status_section();
    if (g.section == Section::Settings) refresh_settings_section();

    refresh_toast();
}

}  // namespace opendojo::menu
