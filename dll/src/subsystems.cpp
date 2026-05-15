#include "subsystems.hpp"

#include <cstdint>

#include "log.hpp"
#include "memory.hpp"

namespace {

// Polaris-side pool init function (FUN_1418e8e00). Takes the recording
// subsystem (resolved via KEY_RECORDING) as `this`. Writes [recording+0x24]=0,
// allocates pool1+pool2 if null, and memsets them. Idempotent.
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

    // Pass the real recording subsystem as `this` (not a stack dummy) so
    // pool_init's `[this+0x24] = 0` clear lands on the right object. We
    // skip the post_init follow-up the natural caller does — it was a
    // speculative fix that ended up touching recording[0x64], [0x5c] in
    // ways the in-game UI didn't like.
    auto recording = lookup(KEY_RECORDING);
    if (!recording) {
        OPENDOJO_LOG("subsystems: KEY_RECORDING unresolved — skipping forced alloc");
        return;
    }

    auto pool_init = reinterpret_cast<PoolInitFn>(base + POOL_INIT_RVA);
    pool_init(reinterpret_cast<void*>(recording));

    auto p1 = memory::read_u64(memory::polaris(POOL1_PTR_OFFSET));
    auto p2 = memory::read_u64(memory::polaris(POOL2_PTR_OFFSET));
    OPENDOJO_LOG("subsystems: force-allocated pool1=0x%llX pool2=0x%llX (recording=0x%llX)",
                 static_cast<unsigned long long>(p1),
                 static_cast<unsigned long long>(p2),
                 static_cast<unsigned long long>(recording));
}
