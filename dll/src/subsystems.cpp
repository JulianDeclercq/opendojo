#include "subsystems.hpp"

#include <cstdint>

#include "log.hpp"
#include "memory.hpp"

namespace {

// Polaris-side recording-subsystem setup, mirrored from the natural
// caller FUN_141913f00. All three take the recording subsystem (resolved
// via KEY_RECORDING) as their first arg.
//
//   pool_init   (FUN_1418e8e00)  — writes [recording+0x24]=0, alloc pool1+pool2 if null, memset
//   post_init   (FUN_1418eb3e0)  — writes recording[0x64]=side, recording[0x5c or 0x60]=idx
//
// The natural caller also runs FUN_1418ec330 (pre_clear) before pool_init,
// but that function is gated on `pool1 != 0` — on a first-ever record
// the gate is false and the call is a no-op, so we skip it.
constexpr std::uintptr_t POOL_INIT_RVA = 0x18E8E00;
constexpr std::uintptr_t POST_INIT_RVA = 0x18EB3E0;

using PoolInitFn = void(*)(void* this_ptr);
using PostInitFn = void(*)(void* this_ptr, char side, std::uint32_t idx);

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
    // pool_init's `[this+0x24] = 0` clear lands on the right object, and
    // call post_init afterward to write the recording-side state that the
    // in-game UI checks. Without these, pool1 has correct bytes but the
    // in-game playback UI never reflects them on a fresh process — the
    // game's first-record flow is what normally sets up this state, and
    // we have to mirror it.
    auto recording = lookup(KEY_RECORDING);
    if (!recording) {
        OPENDOJO_LOG("subsystems: KEY_RECORDING unresolved — skipping forced alloc");
        return;
    }

    auto pool_init = reinterpret_cast<PoolInitFn>(base + POOL_INIT_RVA);
    auto post_init = reinterpret_cast<PostInitFn>(base + POST_INIT_RVA);

    pool_init(reinterpret_cast<void*>(recording));
    // side=0 (P1), idx=0 — safe defaults. The natural caller computes idx
    // from the active gameplay slot, but for pool allocation alone, any
    // valid pair is fine.
    post_init(reinterpret_cast<void*>(recording), 0, 0);

    auto p1 = memory::read_u64(memory::polaris(POOL1_PTR_OFFSET));
    auto p2 = memory::read_u64(memory::polaris(POOL2_PTR_OFFSET));
    OPENDOJO_LOG("subsystems: force-allocated pool1=0x%llX pool2=0x%llX (recording=0x%llX)",
                 static_cast<unsigned long long>(p1),
                 static_cast<unsigned long long>(p2),
                 static_cast<unsigned long long>(recording));
}
