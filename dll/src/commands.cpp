#include "commands.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "drill.hpp"
#include "log.hpp"
#include "memory.hpp"
#include "players.hpp"
#include "slot.hpp"
#include "subsystems.hpp"

namespace opendojo::commands {

std::filesystem::path drills_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto root = (n > 0 && n < MAX_PATH)
        ? std::filesystem::path(buf).parent_path()
        : std::filesystem::path(L".");
    return root / L"opendojo_drills";
}

namespace {

bool ensure_drills_dir() {
    std::error_code ec;
    std::filesystem::create_directories(drills_dir(), ec);
    return !ec;
}

std::string read_whole_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    f.seekg(0);
    std::string out(static_cast<std::size_t>(size), '\0');
    f.read(out.data(), out.size());
    return out;
}

bool write_whole_file(const std::filesystem::path& p, const void* data, std::size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return f.good();
}

// Parse just enough of a .drill file to populate a DrillHeader. Reads up to
// the first `---` line or 4 KiB, whichever comes first.
bool parse_header_only(const std::filesystem::path& path, DrillHeader& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string line;
    std::size_t bytes = 0;
    constexpr std::size_t LIMIT = 4096;
    out.path = path;
    while (std::getline(f, line) && bytes < LIMIT) {
        bytes += line.size() + 1;
        // Strip trailing \r from CRLF.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Stop at the first recording marker.
        if (line.size() >= 3 && line.substr(0, 3) == "---") break;
        if (line.empty() || line[0] == '#') continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto key = line.substr(0, colon);
        auto val = line.substr(colon + 1);
        // Trim ascii whitespace.
        auto trim = [](std::string& s) {
            std::size_t a = 0;
            while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
            std::size_t b = s.size();
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
            s = s.substr(a, b - a);
        };
        trim(key);
        trim(val);
        if      (key == "name")        out.name = val;
        else if (key == "description") out.description = val;
        else if (key == "character")   out.character = val;
        else if (key == "cpu_side")    out.cpu_side = val;
        else if (key == "recordings")  {
            try { out.recording_count = static_cast<std::size_t>(std::stoul(val)); }
            catch (...) { out.recording_count = 0; }
        }
    }
    if (out.character.empty()) out.character = "unknown";
    if (out.name.empty()) out.name = path.stem().string();
    return true;
}

std::filesystem::path resolve_collision(const std::filesystem::path& dir, std::string_view slug) {
    auto base = std::filesystem::path(std::wstring(slug.begin(), slug.end()));
    auto candidate = dir / (base.wstring() + L".drill");
    if (!std::filesystem::exists(candidate)) return candidate;
    for (int i = 2; i < 1000; ++i) {
        wchar_t suffix[16];
        swprintf_s(suffix, L"_%d.drill", i);
        candidate = dir / (base.wstring() + suffix);
        if (!std::filesystem::exists(candidate)) return candidate;
    }
    return {};  // collision storm — caller will treat as failure
}

std::string timestamp_name() {
    using clock = std::chrono::system_clock;
    std::time_t t = clock::to_time_t(clock::now());
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "drill_%04d%02d%02d_%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}  // anonymous

// ---------------------------------------------------------------------------

std::vector<DrillHeader> list_drills() {
    std::vector<DrillHeader> out;
    std::error_code ec;
    auto dir = drills_dir();
    if (!std::filesystem::exists(dir, ec)) return out;

    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto p = entry.path();
        if (p.extension() != L".drill") continue;
        DrillHeader h;
        if (parse_header_only(p, h)) out.push_back(std::move(h));
    }

    std::sort(out.begin(), out.end(),
              [](const DrillHeader& a, const DrillHeader& b) { return a.name < b.name; });
    return out;
}

LoadResult load_drill(const std::filesystem::path& path, LoadMode mode) {
    LoadResult r;
    auto text = read_whole_file(path);
    if (text.empty()) {
        r.message = "couldn't read drill file";
        return r;
    }
    auto decoded = opendojo::drill::decode_text(text);
    if (!decoded.error.empty()) {
        r.message = "decode failed: " + decoded.error;
        return r;
    }
    auto& d = decoded.drill;

    if (!opendojo::subsystems::pool1()) {
        r.message = "pool1 not allocated — record once in practice mode first";
        return r;
    }

    if (d.recordings.size() > opendojo::slot::USER_SLOTS) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "drill has %zu recordings (max %zu)",
                      d.recordings.size(), opendojo::slot::USER_SLOTS);
        r.message = buf;
        return r;
    }

    // Bisect markers for narrowing where the round-intro input freeze
    // comes from. Each marker file (empty file inside opendojo_drills/)
    // disables one component of the load. Defaults (no markers) match
    // pre-bisect behavior.
    auto dbg_skip = [](const char* name) {
        std::error_code ec;
        return std::filesystem::exists(drills_dir() / name, ec);
    };
    const bool skip_pre_clear  = dbg_skip("_dbg_skip_pre_clear");
    const bool skip_slot_bytes = dbg_skip("_dbg_skip_slot_bytes");
    const bool skip_set_flag   = dbg_skip("_dbg_skip_set_flag");
    OPENDOJO_LOG("load_drill: bisect — pre_clear=%s slot_bytes=%s set_flag=%s",
                 skip_pre_clear  ? "SKIP" : "run",
                 skip_slot_bytes ? "SKIP" : "run",
                 skip_set_flag   ? "SKIP" : "run");

    if (mode == LoadMode::ReplaceAll) {
        if (!skip_pre_clear) {
            for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
                opendojo::slot::set_recorded_flag(i, false);
            }
        }
        for (std::size_t i = 0; i < d.recordings.size(); ++i) {
            if (!skip_slot_bytes) {
                auto addr = opendojo::slot::address(i);
                if (!addr) {
                    r.message = "pool not allocated";
                    return r;
                }
                opendojo::memory::write_bytes(
                    addr, d.recordings[i].slot_bytes.data(), opendojo::slot::SLOT_PITCH);
            }
            if (!skip_set_flag) {
                auto s = opendojo::slot::set_recorded_flag(i, true);
                if (s != opendojo::slot::WriteStatus::Ok) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "slot %zu: %s",
                                  i + 1, opendojo::slot::describe(s));
                    r.message = buf;
                    return r;
                }
            }
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "replaced all slots with %zu recordings", d.recordings.size());
        r.ok = true;
        r.message = buf;
        OPENDOJO_LOG("load_drill: %s (%ls)", r.message.c_str(), path.c_str());
        return r;
    }

    // AppendToFree.
    std::vector<std::size_t> free_slots;
    for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
        if (!opendojo::slot::is_populated(i)) free_slots.push_back(i);
    }
    if (free_slots.size() < d.recordings.size()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "drill needs %zu slots, %zu free — Shift+Load to replace",
                      d.recordings.size(), free_slots.size());
        r.message = buf;
        return r;
    }
    for (std::size_t i = 0; i < d.recordings.size(); ++i) {
        if (!skip_slot_bytes) {
            auto addr = opendojo::slot::address(free_slots[i]);
            if (!addr) {
                r.message = "pool not allocated";
                return r;
            }
            opendojo::memory::write_bytes(
                addr, d.recordings[i].slot_bytes.data(), opendojo::slot::SLOT_PITCH);
        }
        if (!skip_set_flag) {
            auto s = opendojo::slot::set_recorded_flag(free_slots[i], true);
            if (s != opendojo::slot::WriteStatus::Ok) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "slot %zu: %s",
                              free_slots[i] + 1, opendojo::slot::describe(s));
                r.message = buf;
                return r;
            }
        }
    }
    // Caller is responsible for calling subsystems::mark_session_loaded —
    // see the same note in the ReplaceAll branch above.
    char buf[160];
    std::snprintf(buf, sizeof(buf), "loaded %zu recordings into free slots",
                  d.recordings.size());
    r.ok = true;
    r.message = buf;
    OPENDOJO_LOG("load_drill: %s (%ls)", r.message.c_str(), path.c_str());
    return r;
}

ExportResult export_current_slots(std::string_view drill_name,
                                  std::string_view description,
                                  std::string_view character,
                                  std::string_view cpu_side) {
    ExportResult r;

    if (!opendojo::subsystems::pool1()) {
        r.message = "pool1 not allocated — record once in practice mode first";
        return r;
    }
    if (!ensure_drills_dir()) {
        r.message = "couldn't create drills directory";
        return r;
    }

    // Auto-detect from the live game state and let any explicit caller
    // override. detect_cpu() returns detected=false outside a match.
    auto cpu = opendojo::players::detect_cpu();

    opendojo::drill::Drill d;
    d.name        = drill_name.empty() ? timestamp_name() : std::string(drill_name);
    d.description = std::string(description);
    if (!character.empty()) {
        d.character = std::string(character);
    } else if (cpu.detected) {
        d.character = cpu.character_name;
    } else {
        d.character = "unknown";
    }
    if (!cpu_side.empty()) {
        d.cpu_side = std::string(cpu_side);
    } else if (cpu.detected) {
        d.cpu_side = opendojo::players::side_to_string(cpu.cpu_side);
    }

    for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
        if (!opendojo::slot::is_populated(i)) continue;
        std::uint8_t bytes[opendojo::slot::SLOT_PITCH];
        if (!opendojo::slot::read(i, bytes)) continue;
        char rec_name[32];
        std::snprintf(rec_name, sizeof(rec_name), "slot %zu", i + 1);
        d.recordings.push_back(opendojo::drill::make_recording(rec_name, bytes));
    }
    if (d.recordings.empty()) {
        r.message = "no slots contain recordings to export";
        return r;
    }

    auto text = opendojo::drill::encode_text(d);
    auto slug = opendojo::drill::slugify(d.name);
    auto path = resolve_collision(drills_dir(), slug);
    if (path.empty()) {
        r.message = "filename collision storm — pick a different name";
        return r;
    }
    if (!write_whole_file(path, text.data(), text.size())) {
        r.message = "failed to write drill file";
        return r;
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf), "exported %zu recordings", d.recordings.size());
    r.ok = true;
    r.path = path;
    r.message = buf;
    OPENDOJO_LOG("export_current_slots: %s -> %ls", r.message.c_str(), path.c_str());
    return r;
}

void show_status() {
    OPENDOJO_LOG("=== OpenDojo status ===");
    auto base = opendojo::memory::polaris_base();
    OPENDOJO_LOG("  polaris_base = 0x%llX", static_cast<unsigned long long>(base));

    auto p1 = opendojo::subsystems::pool1();
    OPENDOJO_LOG("  pool1        = 0x%llX %s",
                static_cast<unsigned long long>(p1),
                p1 ? "" : "(NULL — record once in practice mode)");

    // Log all resolved subsystem addresses — used to find character ID offsets.
    struct { const char* name; std::uintptr_t key; } subsys[] = {
        { "gameplay",  opendojo::subsystems::KEY_GAMEPLAY  },
        { "singleton", opendojo::subsystems::KEY_SINGLETON },
        { "subB",      opendojo::subsystems::KEY_SUBB      },
        { "subC",      opendojo::subsystems::KEY_SUBC      },
        { "subD",      opendojo::subsystems::KEY_SUBD      },
    };
    for (auto& s : subsys) {
        auto addr = opendojo::subsystems::lookup(s.key);
        OPENDOJO_LOG("  %-9s = 0x%llX", s.name, static_cast<unsigned long long>(addr));
    }

    if (p1) {
        for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
            if (!opendojo::slot::is_populated(i)) {
                OPENDOJO_LOG("  slot %zu: empty", i + 1);
            } else {
                auto n = opendojo::slot::event_count(i);
                OPENDOJO_LOG("  slot %zu: %u events", i + 1, static_cast<unsigned>(n));
            }
        }
    }
    OPENDOJO_LOG("  drill dir    = %ls", drills_dir().c_str());
}

}  // namespace opendojo::commands
