#include "slot.hpp"

#include "memory.hpp"
#include "subsystems.hpp"

namespace opendojo::slot {

const char* describe(WriteStatus s) {
    switch (s) {
        case WriteStatus::Ok:                return "ok";
        case WriteStatus::InvalidSlot:       return "invalid slot index";
        case WriteStatus::PoolNotAllocated:  return "pool1 not allocated — record once in practice mode first";
        case WriteStatus::NotInPracticeMode: return "not in practice mode (subsystem unresolved)";
    }
    return "unknown";
}

std::uintptr_t address(std::size_t slot_idx) {
    if (slot_idx >= USER_SLOTS) return 0;
    auto p1 = subsystems::pool1();
    if (!p1) return 0;
    return p1 + slot_idx * SLOT_PITCH;
}

std::uint16_t event_count(std::size_t slot_idx) {
    auto a = address(slot_idx);
    return a ? memory::read_u16(a) : std::uint16_t{0};
}

bool read(std::size_t slot_idx, std::uint8_t* out) {
    if (!out) return false;
    auto a = address(slot_idx);
    if (!a) return false;
    memory::read_bytes(a, out, SLOT_PITCH);
    return true;
}

WriteStatus set_recorded_flag(std::size_t slot_idx, bool recorded) {
    if (slot_idx >= USER_SLOTS) return WriteStatus::InvalidSlot;

    // Re-resolve every time — subsystem pointers change at scene transitions
    // (see project_opendojo_subsystem_lifecycle memory). Caching breaks
    // silently after the first scene change.
    auto gameplay  = subsystems::lookup(subsystems::KEY_GAMEPLAY);
    auto singleton = subsystems::lookup(subsystems::KEY_SINGLETON);
    auto subB      = subsystems::lookup(subsystems::KEY_SUBB);
    auto subC      = subsystems::lookup(subsystems::KEY_SUBC);
    if (!gameplay || !singleton || !subB || !subC) {
        return WriteStatus::NotInPracticeMode;
    }

    auto flag_addr = gameplay
                   + GAMEPLAY_SLOT_BASE
                   + slot_idx * GAMEPLAY_SLOT_STRIDE
                   + GAMEPLAY_SLOT_FLAG;

    if (recorded) {
        memory::write_u32(flag_addr,         2u);
        // singleton +0x02 = 0x40 is the "recording session active" marker the
        // engine sets during a real practice recording. Empirically observed
        // 0x40 during P2-side AND P1-side recordings; cleared to 0x00 after
        // our import (which is the only state diff between post-record and
        // post-import). Without this, playback ignores the per-event bit 0x20
        // side tag and falls back to current-side interpretation — which
        // mirrors any drill recorded with bit 0x20 set (i.e. P2-side drills).
        memory::write_u8 (singleton + 0x002, 0x40u);
        memory::write_u8 (singleton + 0x008, 0x01u);
        memory::write_u8 (subB      + 0x065, 0x00u);
        memory::write_u32(subC      + 0x25C, 1u);
    } else {
        memory::write_u32(flag_addr,         0u);
        memory::write_u8 (singleton + 0x002, 0x00u);
        memory::write_u8 (singleton + 0x008, 0x00u);
        memory::write_u8 (subB      + 0x065, 0x01u);
        memory::write_u32(subC      + 0x25C, 0xFFFFFFFFu);  // -1 as uint32
    }
    return WriteStatus::Ok;
}

WriteStatus write(std::size_t slot_idx, const std::uint8_t* data) {
    if (slot_idx >= USER_SLOTS) return WriteStatus::InvalidSlot;
    if (!data)                  return WriteStatus::InvalidSlot;

    auto p1 = subsystems::pool1();
    if (!p1) return WriteStatus::PoolNotAllocated;

    // Write the slot bytes first; the in-game tick handler reads the per-slot
    // flag, so we want the data to be in place before the flag flips.
    memory::write_bytes(p1 + slot_idx * SLOT_PITCH, data, SLOT_PITCH);
    return set_recorded_flag(slot_idx, true);
}

}  // namespace opendojo::slot
