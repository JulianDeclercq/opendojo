#include "commands.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "drill.hpp"
#include "log.hpp"
#include "memory.hpp"
#include "slot.hpp"
#include "subsystems.hpp"

namespace opendojo::commands {

namespace {

std::filesystem::path drills_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto root = (n > 0 && n < MAX_PATH)
        ? std::filesystem::path(buf).parent_path()
        : std::filesystem::path(L".");
    return root / L"opendojo_drills";
}

std::filesystem::path drill_path(std::size_t slot_idx, const wchar_t* ext) {
    wchar_t name[64];
    swprintf_s(name, L"slot_%zu.%s", slot_idx + 1, ext);
    return drills_dir() / name;
}

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

bool write_whole_file(const std::filesystem::path& p,
                      const void* data, std::size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return f.good();
}

}  // anonymous

void export_slot(std::size_t slot_idx) {
    if (slot_idx >= opendojo::slot::USER_SLOTS) {
        OPENDOJO_LOG("export: invalid slot index %zu", slot_idx + 1);
        return;
    }

    std::uint8_t bytes[opendojo::slot::SLOT_PITCH];
    if (!opendojo::slot::read(slot_idx, bytes)) {
        OPENDOJO_LOG("export slot %zu: pool1 not allocated yet — record once in practice mode first",
                    slot_idx + 1);
        return;
    }

    std::uint16_t events =
        static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1]) << 8);
    if (events == 0) {
        OPENDOJO_LOG("export slot %zu: empty — nothing to export", slot_idx + 1);
        return;
    }

    if (!ensure_drills_dir()) {
        OPENDOJO_LOG("export slot %zu: failed to create drill directory", slot_idx + 1);
        return;
    }

    auto text = opendojo::drill::encode_text(bytes, slot_idx);
    auto bin  = opendojo::drill::encode_binary(bytes, slot_idx);
    auto text_path = drill_path(slot_idx, L"drill");
    auto bin_path  = drill_path(slot_idx, L"bin");

    if (!write_whole_file(text_path, text.data(), text.size())) {
        OPENDOJO_LOG("export slot %zu: failed to write %ls",
                    slot_idx + 1, text_path.c_str());
        return;
    }
    if (!write_whole_file(bin_path, bin.data(), bin.size())) {
        OPENDOJO_LOG("export slot %zu: failed to write %ls",
                    slot_idx + 1, bin_path.c_str());
        return;
    }
    OPENDOJO_LOG("export slot %zu: %u events -> slot_%zu.drill + slot_%zu.bin",
                slot_idx + 1, static_cast<unsigned>(events),
                slot_idx + 1, slot_idx + 1);
}

void import_slot(std::size_t slot_idx) {
    if (slot_idx >= opendojo::slot::USER_SLOTS) {
        OPENDOJO_LOG("import: invalid slot index %zu", slot_idx + 1);
        return;
    }

    auto text_path = drill_path(slot_idx, L"drill");
    auto bin_path  = drill_path(slot_idx, L"bin");

    std::vector<std::uint8_t> payload;
    const char* source = nullptr;

    auto text_content = read_whole_file(text_path);
    if (!text_content.empty()) {
        // Auto-detect legacy binary stored under the .drill extension.
        if (text_content.size() >= 4 &&
            std::memcmp(text_content.data(), opendojo::drill::BINARY_MAGIC, 4) == 0) {
            auto r = opendojo::drill::decode_binary(text_content);
            if (!r.error.empty()) {
                OPENDOJO_LOG("import slot %zu: legacy binary parse failed: %s",
                            slot_idx + 1, r.error.c_str());
                return;
            }
            payload = std::move(r.data);
            source = "slot_N.drill (legacy binary)";
        } else {
            auto r = opendojo::drill::decode_text(text_content);
            if (!r.error.empty()) {
                OPENDOJO_LOG("import slot %zu: text decode failed: %s",
                            slot_idx + 1, r.error.c_str());
                return;
            }
            payload = std::move(r.data);
            source = "slot_N.drill (text)";
        }
    } else {
        auto bin_content = read_whole_file(bin_path);
        if (bin_content.empty()) {
            OPENDOJO_LOG("import slot %zu: no slot_%zu.drill or slot_%zu.bin found in %ls",
                        slot_idx + 1, slot_idx + 1, slot_idx + 1,
                        drills_dir().c_str());
            return;
        }
        auto r = opendojo::drill::decode_binary(bin_content);
        if (!r.error.empty()) {
            OPENDOJO_LOG("import slot %zu: bin parse failed: %s",
                        slot_idx + 1, r.error.c_str());
            return;
        }
        payload = std::move(r.data);
        source = "slot_N.bin";
    }

    auto status = opendojo::slot::write(slot_idx, payload.data());
    if (status != opendojo::slot::WriteStatus::Ok) {
        OPENDOJO_LOG("import slot %zu (from %s): %s",
                    slot_idx + 1, source, opendojo::slot::describe(status));
        return;
    }

    std::uint16_t events =
        static_cast<std::uint16_t>(payload[0]) |
        (static_cast<std::uint16_t>(payload[1]) << 8);
    OPENDOJO_LOG("import slot %zu (from %s): %u events written. "
                "Close + reopen the practice menu to see the change.",
                slot_idx + 1, source, static_cast<unsigned>(events));
}

void show_status() {
    OPENDOJO_LOG("=== OpenDojo status ===");
    auto base = opendojo::memory::polaris_base();
    OPENDOJO_LOG("  polaris_base = 0x%llX", static_cast<unsigned long long>(base));

    auto p1 = opendojo::subsystems::pool1();
    OPENDOJO_LOG("  pool1        = 0x%llX %s",
                static_cast<unsigned long long>(p1),
                p1 ? "" : "(NULL — record once in practice mode)");

    if (p1) {
        for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
            auto n = opendojo::slot::event_count(i);
            if (n == 0) {
                OPENDOJO_LOG("  slot %zu: empty", i + 1);
            } else {
                OPENDOJO_LOG("  slot %zu: %u events",
                            i + 1, static_cast<unsigned>(n));
            }
        }
    }
    OPENDOJO_LOG("  drill dir    = %ls", drills_dir().c_str());
}

}  // namespace opendojo::commands
