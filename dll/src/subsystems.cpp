#include "subsystems.hpp"

#include "memory.hpp"

std::uintptr_t opendojo::subsystems::lookup(std::uintptr_t key_offset) {
    auto base = memory::polaris_base();
    if (!base) return 0;
    auto ctx = memory::read_u64(base + CTX_PTR_OFFSET);
    if (!ctx) return 0;
    auto map = memory::read_u64(ctx + 0x10);
    if (!map) return 0;

    auto sentinel = memory::read_u64(map + 0x100);
    auto mask     = memory::read_u64(map + 0x128);
    auto buckets  = memory::read_u64(map + 0x110);
    if (!buckets) return 0;

    auto key    = memory::read_u32(base + key_offset);
    auto bucket = buckets + (mask & key) * 0x10;
    auto first  = memory::read_u64(bucket);
    auto entry  = memory::read_u64(bucket + 8);

    // Walk the bucket's collision chain. Cap at 64 steps as a sanity bound
    // — real chains are short, anything deeper means the data is corrupt
    // or we've snapshotted mid-resize.
    for (int steps = 0; entry && entry != sentinel && steps < 64; ++steps) {
        if (memory::read_u32(entry + 0x10) == key) {
            return memory::read_u64(entry + 0x18);
        }
        if (entry == first) break;
        entry = memory::read_u64(entry + 8);
    }
    return 0;
}

std::uintptr_t opendojo::subsystems::pool1() {
    return memory::read_u64(memory::polaris(POOL1_PTR_OFFSET));
}

std::uintptr_t opendojo::subsystems::pool2() {
    return memory::read_u64(memory::polaris(POOL2_PTR_OFFSET));
}
