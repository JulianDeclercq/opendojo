#include "config.hpp"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "commands.hpp"  // drills_dir() returns the opendojo/ data dir
#include "log.hpp"

namespace opendojo::config {

namespace {

std::atomic<std::uint32_t> g_toggle_vk{VK_F12};
std::atomic<bool> g_capturing{false};
std::atomic<std::uint32_t> g_captured_vk{0};

std::atomic<std::uint16_t> g_toggle_pad_btn{XINPUT_GAMEPAD_Y};
std::atomic<bool> g_pad_capturing{false};
std::atomic<std::uint16_t> g_captured_pad_btn{0};

std::filesystem::path config_path() {
    return opendojo::commands::drills_dir() / L"config.json";
}

// Tiny ad-hoc JSON: we only persist one integer field. A full JSON
// library is overkill. Format:
//
//   { "toggle_vk": 123 }
//
// Whitespace-permissive, single-line.

bool parse_uint32_after(const std::string& src, const char* key, std::uint32_t& out) {
    auto pos = src.find(key);
    if (pos == std::string::npos) return false;
    pos = src.find(':', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    // Skip whitespace.
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t'))
        ++pos;
    if (pos >= src.size()) return false;
    char* end = nullptr;
    auto v = std::strtoul(src.c_str() + pos, &end, 10);
    if (end == src.c_str() + pos) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

}  // namespace

void load() {
    auto path = config_path();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        OPENDOJO_LOG("config: no config file at %ls — using defaults", path.c_str());
        return;
    }
    std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::uint32_t v = 0;
    if (parse_uint32_after(buf, "\"toggle_vk\"", v) && v != 0) {
        g_toggle_vk.store(v);
        OPENDOJO_LOG("config: loaded toggle_vk = 0x%02X", v);
    }
    if (parse_uint32_after(buf, "\"toggle_pad_btn\"", v) && v != 0) {
        g_toggle_pad_btn.store(static_cast<std::uint16_t>(v & 0xFFFF));
        OPENDOJO_LOG("config: loaded toggle_pad_btn = 0x%04X", v);
    }
}

void save() {
    auto path = config_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        OPENDOJO_LOG("config: failed to open %ls for write", path.c_str());
        return;
    }
    f << "{ \"toggle_vk\": " << g_toggle_vk.load()
      << ", \"toggle_pad_btn\": " << g_toggle_pad_btn.load() << " }\n";
    OPENDOJO_LOG("config: saved toggle_vk=0x%02X toggle_pad_btn=0x%04X to %ls", g_toggle_vk.load(),
                 g_toggle_pad_btn.load(), path.c_str());
}

std::uint32_t toggle_vk() {
    return g_toggle_vk.load();
}

void set_toggle_vk(std::uint32_t vk) {
    if (vk == 0) return;
    g_toggle_vk.store(vk);
    save();
}

void start_capture() {
    g_captured_vk.store(0);
    g_capturing.store(true);
}

void cancel_capture() {
    g_capturing.store(false);
    g_captured_vk.store(0);
}

bool is_capturing() {
    return g_capturing.load();
}

std::uint32_t consume_captured_vk() {
    auto v = g_captured_vk.exchange(0);
    if (v != 0) g_capturing.store(false);
    return v;
}

void notify_captured_vk(std::uint32_t vk) {
    if (g_capturing.load()) g_captured_vk.store(vk);
}

// ---- Pad bind -------------------------------------------------------------

std::uint16_t toggle_pad_btn() {
    return g_toggle_pad_btn.load();
}

void set_toggle_pad_btn(std::uint16_t mask) {
    if (mask == 0) return;
    g_toggle_pad_btn.store(mask);
    save();
}

void start_pad_capture() {
    g_captured_pad_btn.store(0);
    g_pad_capturing.store(true);
}

void cancel_pad_capture() {
    g_pad_capturing.store(false);
    g_captured_pad_btn.store(0);
}

bool is_pad_capturing() {
    return g_pad_capturing.load();
}

std::uint16_t consume_captured_pad_btn() {
    auto v = g_captured_pad_btn.exchange(0);
    if (v != 0) g_pad_capturing.store(false);
    return v;
}

void notify_captured_pad_btn(std::uint16_t mask) {
    if (g_pad_capturing.load()) g_captured_pad_btn.store(mask);
}

}  // namespace opendojo::config
