// SPDX-License-Identifier: MIT
#pragma once

// Binary layout contract of the shared GCRA object (see docs/SPEC.md).
// The contract is pinned by the (magic, version) pair: an incompatible layout change
// must rotate both the magic and the shm object name (R20).

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace gcra_ipc::detail {

////////////////////////////////////////////////////////////////////////////////

// Usage-contract upper bound of a single Reserve() charge in units; not checked at runtime.
// The increment numerator units x 1e9 fits uint64 up to the theoretical limit 2^34 (J13);
// units_max keeps a 2^10 margin below it.
inline constexpr std::uint64_t units_max = 1ull << 24;

// Initialization marker of a shm object, fourcc "GCR1".
inline constexpr std::uint32_t magic = 0x31524347u;

// Current layout version; stored for diagnostics only, factories never check it.
inline constexpr std::uint32_t layout_version = 1;

// The single cache line mutated by every Reserve() call in every participant process.
struct alignas(64) gcra_ipc_state
{
    std::atomic<std::uint64_t> position_ns;
    std::atomic<std::uint64_t> rate_per_second;
    std::atomic<std::uint64_t> burst_window_ns;
};

struct gcra_ipc_shm
{
    std::atomic<std::uint32_t> magic;
    std::uint32_t version;
    alignas(64) gcra_ipc_state state;
};

static_assert(sizeof(gcra_ipc_state) == 64);
static_assert(alignof(gcra_ipc_state) == 64);
static_assert(sizeof(gcra_ipc_shm) == 128);
static_assert(std::is_trivially_copyable_v<gcra_ipc_shm>);

////////////////////////////////////////////////////////////////////////////////

} // namespace gcra_ipc::detail
