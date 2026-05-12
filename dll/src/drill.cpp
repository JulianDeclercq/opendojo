#include "drill.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace openlab::drill {

namespace {

// Defaults — match the Lua. Annotations are omitted when at default.
constexpr std::uint8_t DEFAULT_MARK = 0x2;
constexpr std::uint8_t DEFAULT_AUX  = 0xA0;

// Direction nibble <-> Tekken notation. byte 0 low nibble is a 4-bit mask:
// bit 0 = up, bit 1 = down, bit 2 = forward, bit 3 = back.
struct DirRow { std::uint8_t nibble; const char* text; };
constexpr std::array<DirRow, 9> DIR_TABLE = {{
    { 0,  "n"  },
    { 1,  "u"  },
    { 2,  "d"  },
    { 4,  "f"  },
    { 5,  "uf" },
    { 6,  "df" },
    { 8,  "b"  },
    { 9,  "ub" },
    { 10, "db" },
}};

const char* dir_to_text(std::uint8_t nibble) {
    for (const auto& row : DIR_TABLE) {
        if (row.nibble == nibble) return row.text;
    }
    return nullptr;
}

int text_to_dir(std::string_view tok) {
    for (const auto& row : DIR_TABLE) {
        if (tok == row.text) return row.nibble;
    }
    return -1;
}

// byte 1 high nibble: 0x40=LP (1), 0x80=RP (2), 0x10=LK (3), 0x20=RK (4).
struct ButtonEncode {
    std::string  text;
    std::uint8_t unknown;
};

ButtonEncode encode_buttons(std::uint8_t byte1) {
    ButtonEncode out;
    auto add = [&](char c, std::uint8_t bit) {
        if (byte1 & bit) {
            if (!out.text.empty()) out.text += '+';
            out.text += c;
        }
    };
    add('1', 0x40);
    add('2', 0x80);
    add('3', 0x10);
    add('4', 0x20);
    if (out.text.empty()) out.text = ".";
    out.unknown = byte1 & 0x0F;
    return out;
}

int parse_buttons(std::string_view tok) {
    if (tok == ".") return 0;
    int mask = 0;
    std::size_t pos = 0;
    while (pos < tok.size()) {
        auto end = tok.find('+', pos);
        if (end == std::string_view::npos) end = tok.size();
        auto sub = tok.substr(pos, end - pos);
        if      (sub == "1") mask |= 0x40;
        else if (sub == "2") mask |= 0x80;
        else if (sub == "3") mask |= 0x10;
        else if (sub == "4") mask |= 0x20;
        else return -1;
        pos = (end < tok.size()) ? end + 1 : end;
    }
    return mask;
}

std::string_view trim(std::string_view s) {
    auto is_ws = [](char c) { return c == ' ' || c == '\t'; };
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back()))  s.remove_suffix(1);
    return s;
}

std::string_view strip_comment(std::string_view s) {
    auto pos = s.find('#');
    return (pos == std::string_view::npos) ? s : s.substr(0, pos);
}

// Hex parser. Returns -1 on any non-hex digit.
long long parse_hex(std::string_view s) {
    if (s.empty()) return -1;
    long long n = 0;
    for (char c : s) {
        int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return -1;
        n = (n << 4) | d;
    }
    return n;
}

// Decimal parser. Returns -1 on non-digit.
long long parse_dec(std::string_view s) {
    if (s.empty()) return -1;
    long long n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n;
}

std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        std::size_t start = i;
        while (i < s.size() && !(s[i] == ' ' || s[i] == '\t')) ++i;
        if (start < i) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// True if `s` looks like `<identifier>: <value>` — i.e. a metadata header
// line, not an event line. Used to skip the `slot:`, `events:` etc. lines.
bool is_header_line(std::string_view s) {
    auto colon = s.find(':');
    if (colon == std::string_view::npos) return false;
    auto key = trim(s.substr(0, colon));
    auto val = trim(s.substr(colon + 1));
    if (key.empty() || val.empty()) return false;
    for (char c : key) {
        bool ident = (c == '_') ||
                     (c >= 'a' && c <= 'z') ||
                     (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9');
        if (!ident) return false;
    }
    return true;
}

std::string encode_event_line(std::uint8_t b0, std::uint8_t b1,
                              std::uint8_t b2, std::uint8_t b3) {
    std::uint8_t mark = (b0 >> 4) & 0x0F;
    std::uint8_t dir  = b0 & 0x0F;

    const char* dir_text = dir_to_text(dir);
    std::string dir_raw_annot;
    if (!dir_text) {
        dir_text = "n";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "dir_raw=%X", dir);
        dir_raw_annot = buf;
    }

    auto btn = encode_buttons(b1);

    char line[128];
    std::snprintf(line, sizeof(line), "  %-2s   %-5s  %4u",
                  dir_text, btn.text.c_str(), static_cast<unsigned>(b3));
    std::string result = line;

    const bool need_meta = (mark != DEFAULT_MARK) ||
                           (btn.unknown != 0)    ||
                           (b2 != DEFAULT_AUX);
    if (!dir_raw_annot.empty() || need_meta) {
        result += "   ";
        if (!dir_raw_annot.empty()) {
            result += dir_raw_annot;
            if (need_meta) result += ' ';
        }
        if (need_meta) {
            char meta[16];
            std::snprintf(meta, sizeof(meta), "meta=%X%X%02X",
                          mark, btn.unknown, b2);
            result += meta;
        }
    }
    return result;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------

std::string encode_text(const std::uint8_t* slot_data, std::size_t slot_idx) {
    if (!slot_data) return {};

    const std::uint16_t event_count =
        static_cast<std::uint16_t>(slot_data[0]) |
        (static_cast<std::uint16_t>(slot_data[1]) << 8);

    std::uint32_t total_frames = 0;
    for (std::uint16_t i = 0; i < event_count; ++i) {
        std::size_t off = 2 + std::size_t{i} * 4;
        if (off + 3 >= SLOT_PITCH) break;
        total_frames += slot_data[off + 3];
    }

    std::string out;
    out.reserve(512 + std::size_t{event_count} * 32);

    char buf[128];
    out += "# OpenLab drill v1\n";
    std::snprintf(buf, sizeof(buf), "slot:         %zu\n", slot_idx + 1);
    out += buf;
    std::snprintf(buf, sizeof(buf), "events:       %u\n",
                  static_cast<unsigned>(event_count));
    out += buf;
    std::snprintf(buf, sizeof(buf), "total_frames: %u\n",
                  static_cast<unsigned>(total_frames));
    out += buf;
    out +=
        "#\n"
        "# === Editing this drill ===\n"
        "# Each event below is one input transition in the recording.\n"
        "# Format:   <dir>  <buttons>  <frames>  [meta=NNNN]\n"
        "#\n"
        "# Editable fields - change these to dial in the drill:\n"
        "#   dir       Tekken notation: n f b u d, uf df ub db\n"
        "#   buttons   1 2 3 4 (LP RP LK RK), combined with + (e.g. 1+2), or . for none\n"
        "#   frames    duration in frames (60 frames = 1 second @ 60fps)\n"
        "#\n"
        "# meta=NNNN  recorder metadata - DO NOT EDIT. It encodes engine state\n"
        "#            (input gate, raw input flags, per-frame sync hash). The game\n"
        "#            checks the sync hash against live state during playback;\n"
        "#            mismatched meta causes silent playback desync. The default\n"
        "#            value (meta=20A0) is omitted from this file.\n"
        "#\n"
        "# === Events ===\n";

    for (std::uint16_t i = 0; i < event_count; ++i) {
        std::size_t off = 2 + std::size_t{i} * 4;
        if (off + 3 >= SLOT_PITCH) break;
        out += encode_event_line(slot_data[off],     slot_data[off + 1],
                                 slot_data[off + 2], slot_data[off + 3]);
        out += '\n';
    }
    return out;
}

TextResult decode_text(std::string_view text) {
    TextResult result;
    std::vector<std::array<std::uint8_t, 4>> events;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto eol = text.find_first_of("\r\n", pos);
        if (eol == std::string_view::npos) eol = text.size();
        auto raw_line = text.substr(pos, eol - pos);
        pos = eol;
        if (pos < text.size() && text[pos] == '\r') ++pos;
        if (pos < text.size() && text[pos] == '\n') ++pos;
        if (raw_line.empty() && pos == text.size()) break;

        auto line = trim(strip_comment(raw_line));
        if (line.empty()) continue;
        if (is_header_line(line)) continue;

        auto toks = split_ws(line);
        if (toks.size() < 3) {
            result.error = "expected at least 3 tokens (dir buttons frames), got: ";
            result.error.append(line.data(), line.size());
            return result;
        }

        const int dir = text_to_dir(toks[0]);
        if (dir < 0) {
            result.error = "unknown direction: ";
            result.error.append(toks[0].data(), toks[0].size());
            return result;
        }

        const int btn_mask = parse_buttons(toks[1]);
        if (btn_mask < 0) {
            result.error = "bad buttons: ";
            result.error.append(toks[1].data(), toks[1].size());
            return result;
        }

        const long long frames = parse_dec(toks[2]);
        if (frames < 0 || frames > 255) {
            result.error = "bad frame count: ";
            result.error.append(toks[2].data(), toks[2].size());
            return result;
        }

        int mark    = DEFAULT_MARK;
        int btn_raw = 0;
        int aux     = DEFAULT_AUX;
        int dir_raw = -1;

        for (std::size_t i = 3; i < toks.size(); ++i) {
            auto eq = toks[i].find('=');
            if (eq == std::string_view::npos) {
                result.error = "expected key=value, got: ";
                result.error.append(toks[i].data(), toks[i].size());
                return result;
            }
            auto key = toks[i].substr(0, eq);
            auto val = toks[i].substr(eq + 1);
            long long n = parse_hex(val);
            if (n < 0) {
                result.error = "bad hex value in annotation: ";
                result.error.append(toks[i].data(), toks[i].size());
                return result;
            }

            if (key == "meta") {
                if (n > 0xFFFF) {
                    result.error = "meta out of range: ";
                    result.error.append(val.data(), val.size());
                    return result;
                }
                const auto v = static_cast<std::uint16_t>(n);
                mark    = (v >> 12) & 0xF;
                btn_raw = (v >>  8) & 0xF;
                aux     =  v        & 0xFF;
            } else if (key == "mark")    { mark    = static_cast<int>(n); }
              else if (key == "btn_raw") { btn_raw = static_cast<int>(n); }
              else if (key == "aux")     { aux     = static_cast<int>(n); }
              else if (key == "dir_raw") { dir_raw = static_cast<int>(n); }
              else {
                result.error = "unknown annotation key: ";
                result.error.append(key.data(), key.size());
                return result;
            }
        }

        const std::uint8_t b0_dir = (dir_raw >= 0)
            ? static_cast<std::uint8_t>(dir_raw & 0xF)
            : static_cast<std::uint8_t>(dir & 0xF);
        std::array<std::uint8_t, 4> ev = {
            static_cast<std::uint8_t>(((mark & 0xF) << 4) | b0_dir),
            static_cast<std::uint8_t>((btn_mask & 0xF0) | (btn_raw & 0x0F)),
            static_cast<std::uint8_t>(aux),
            static_cast<std::uint8_t>(frames),
        };
        events.push_back(ev);
    }

    constexpr std::size_t MAX_EVENTS = (SLOT_PITCH - 2) / 4;
    if (events.size() > MAX_EVENTS) {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "too many events: %zu (max %zu)", events.size(), MAX_EVENTS);
        result.error = buf;
        return result;
    }

    result.data.assign(SLOT_PITCH, 0);
    const std::uint16_t cnt = static_cast<std::uint16_t>(events.size());
    result.data[0] = static_cast<std::uint8_t>(cnt & 0xFF);
    result.data[1] = static_cast<std::uint8_t>((cnt >> 8) & 0xFF);
    for (std::size_t i = 0; i < events.size(); ++i) {
        const std::size_t off = 2 + i * 4;
        result.data[off    ] = events[i][0];
        result.data[off + 1] = events[i][1];
        result.data[off + 2] = events[i][2];
        result.data[off + 3] = events[i][3];
    }
    return result;
}

// ---------------------------------------------------------------------------

std::vector<std::uint8_t> encode_binary(const std::uint8_t* slot_data,
                                        std::size_t          slot_idx) {
    std::vector<std::uint8_t> out(BINARY_SIZE, 0);
    std::memcpy(out.data(), BINARY_MAGIC, 4);
    const auto version = BINARY_VERSION;
    std::memcpy(out.data() + 4, &version, sizeof(version));
    const auto idx = static_cast<std::uint32_t>(slot_idx);
    std::memcpy(out.data() + 8, &idx, sizeof(idx));
    // reserved 4 bytes at offset 12 already zero
    if (slot_data) {
        std::memcpy(out.data() + BINARY_HEADER, slot_data, SLOT_PITCH);
    }
    return out;
}

BinaryResult decode_binary(std::string_view content) {
    BinaryResult result;
    if (content.size() < BINARY_SIZE) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "too small: %zu bytes (need %zu)",
                      content.size(), BINARY_SIZE);
        result.error = buf;
        return result;
    }
    if (std::memcmp(content.data(), BINARY_MAGIC, 4) != 0) {
        result.error = "bad magic (expected 'OLAB')";
        return result;
    }
    std::uint32_t version;
    std::memcpy(&version, content.data() + 4, sizeof(version));
    if (version != BINARY_VERSION) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "unsupported version %u", version);
        result.error = buf;
        return result;
    }
    const auto* payload =
        reinterpret_cast<const std::uint8_t*>(content.data()) + BINARY_HEADER;
    result.data.assign(payload, payload + SLOT_PITCH);
    return result;
}

}  // namespace openlab::drill
