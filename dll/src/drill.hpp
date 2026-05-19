#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Drill file format. One file = one drill = N recordings (N >= 1). A
// "single recording" is the N=1 case — same format, same parser.
// Multi-recording drills enable shareable scenarios like "Jin string
// defense" with several related recordings grouped together.
//
// Encoder produces text. Decoder reads text. Each recording carries a
// 7202-byte slot payload alongside text-decoded event lines (dir /
// buttons / frames / meta=NNNN).

namespace opendojo::drill {

inline constexpr std::size_t SLOT_PITCH = 0x1C22;  // mirrors opendojo::slot::SLOT_PITCH

struct Recording {
    std::string name;  // human-readable; "" => unnamed
    std::uint16_t event_count = 0;
    std::uint32_t total_frames = 0;
    std::vector<std::uint8_t> slot_bytes;  // exactly SLOT_PITCH bytes
};

struct Drill {
    std::string name;         // human-readable drill name
    std::string description;  // single-line free-form
    std::string character;    // lowercase id; "unknown" if unset
    std::string cpu_side;     // "p1" / "p2" / "" (unset)
    std::vector<Recording> recordings;
};

// Encode a drill to text. Always emits the full structure even if N==1.
std::string encode_text(const Drill& d);

struct TextResult {
    Drill drill;
    std::string error;  // empty on success
};

// Decode drill text. On error, `drill.recordings` is empty and `error`
// describes why.
TextResult decode_text(std::string_view text);

// Build a Recording from raw 7202-byte slot payload (the same bytes returned
// by opendojo::slot::read). Populates event_count and total_frames from the
// payload's leading uint16 + per-event frame field. `name` is stored as-is.
Recording make_recording(std::string name, const std::uint8_t* slot_bytes);

// Convert a drill / recording name to a filesystem-safe lowercase slug.
// Non-alphanumeric runs collapse to a single underscore; leading/trailing
// underscores are trimmed; empty input returns "drill".
//   "Jin String Defense"  -> "jin_string_defense"
//   "ff+3   sweep!"       -> "ff_3_sweep"
//   ""                    -> "drill"
std::string slugify(std::string_view name);

}  // namespace opendojo::drill
