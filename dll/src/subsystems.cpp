#include "subsystems.hpp"

#include <cstdint>

#include "log.hpp"
#include "memory.hpp"

namespace {

// Polaris-side function that allocates pool1 and pool2 if missing, then
// zero-fills them. Calling convention: x64 fastcall, single `this`
// argument in RCX. The function dereferences `this+0x24` and writes
// zero there — we satisfy that by passing a stack-local sacrificial
// buffer. Found by AOB-scanning .text for the unique MOV [pool1_slot],
// rax instruction and walking back to the function start.
constexpr std::uintptr_t POOL_INIT_RVA = 0x18E8E00;

using PoolInitFn = void(*)(void* this_ptr);

}  // anonymous namespace

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

void opendojo::subsystems::ensure_pool_allocated() {
    if (memory::read_u64(memory::polaris(POOL1_PTR_OFFSET)) != 0) return;

    auto base = memory::polaris_base();
    if (!base) return;

    // The init function writes 0 to *(this_ptr + 0x24). 64 bytes is more
    // than enough and naturally aligned to 8.
    alignas(8) std::uint8_t dummy[64] = {};

    auto fn = reinterpret_cast<PoolInitFn>(base + POOL_INIT_RVA);
    fn(dummy);

    auto p1 = memory::read_u64(memory::polaris(POOL1_PTR_OFFSET));
    auto p2 = memory::read_u64(memory::polaris(POOL2_PTR_OFFSET));
    OPENDOJO_LOG("subsystems: force-allocated pool1=0x%llX pool2=0x%llX",
                 static_cast<unsigned long long>(p1),
                 static_cast<unsigned long long>(p2));
}
