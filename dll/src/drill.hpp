#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Drill format encoders/decoders. Two representations:
//
//   - Text (v1, canonical, human-editable). Matches what the CE Lua emits
//     today, so .drill files round-trip between the Lua and DLL paths.
//   - Binary (legacy "OLAB" container, byte-clone of the slot payload).
//     Kept as a safety net so corrupted text drills can still be played.
//
// All functions operate on the 7202-byte slot payload (the same bytes that
// live in pool1[slot_idx]). Reading a slot is a separate concern handled
// by opendojo::slot::read().

namespace opendojo::drill {

inline constexpr std::size_t SLOT_PITCH = 0x1C22;  // = opendojo::slot::SLOT_PITCH

// Legacy binary container layout (16-byte header + 7202 bytes = 7218 total).
inline constexpr char         BINARY_MAGIC[4] = {'O','L','A','B'};
inline constexpr std::uint32_t BINARY_VERSION = 1;
inline constexpr std::size_t   BINARY_HEADER  = 0x10;
inline constexpr std::size_t   BINARY_SIZE    = BINARY_HEADER + SLOT_PITCH;

// --- Text format ----------------------------------------------------------

// Encode 7202 bytes into the v1 text DSL. slot_idx is 0-based and only used
// for the `slot:` header line; the actual slot location is the caller's
// concern.
std::string encode_text(const std::uint8_t* slot_data, std::size_t slot_idx);

// Decode a text drill. On success, `data` is exactly SLOT_PITCH bytes and
// `error` is empty. On failure, `data` is empty and `error` describes why.
// Accepts both the v1 packed `meta=NNNN` and the legacy individual
// `mark=`/`btn_raw=`/`aux=` annotations.
struct TextResult {
    std::vector<std::uint8_t> data;
    std::string               error;
};
TextResult decode_text(std::string_view text);

// --- Binary format --------------------------------------------------------

// Wrap 7202 bytes in the OLAB binary container.
std::vector<std::uint8_t> encode_binary(const std::uint8_t* slot_data,
                                        std::size_t          slot_idx);

// Strip the binary container. Same result convention as decode_text.
struct BinaryResult {
    std::vector<std::uint8_t> data;
    std::string               error;
};
BinaryResult decode_binary(std::string_view content);

}  // namespace opendojo::drill
