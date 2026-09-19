// SPDX-License-Identifier: MIT
#ifndef GCRA_IPC_INL_H_
#error "Direct inclusion of this file is not allowed, include gcra_ipc/gcra_ipc.h"
// For the sake of sane code completion.
#include <gcra_ipc/gcra_ipc.h>
#endif

#include <gcra_ipc/gcra_ipc_detail.h>

#include <algorithm>

namespace gcra_ipc {

////////////////////////////////////////////////////////////////////////////////

inline std::uint64_t shaper::Reserve(std::uint64_t nowNs, std::uint64_t units)
{
    if (units == 0) {
        return nowNs;
    }

    auto* state = &Shm_->state;
    while (true) {
        auto positionNs = state->position_ns.load(std::memory_order_relaxed);
        auto ratePerSecond = state->rate_per_second.load(std::memory_order_relaxed);
        if (ratePerSecond == 0) {
            return nowNs;
        }
        auto burstWindowNs = state->burst_window_ns.load(std::memory_order_relaxed);

        // Slot: position shifted back by the burst window, guarded against underflow.
        auto slotNs = positionNs >= burstWindowNs
            ? std::max(positionNs - burstWindowNs, nowNs)
            : nowNs;

        // Increment ceil(units x 1e9 / rate), the only rounding of a request; the
        // division/remainder form never overflows, unlike (x + rate - 1) / rate.
        auto scaledNs = units * 1'000'000'000ull;
        auto incrementNs = scaledNs / ratePerSecond + (scaledNs % ratePerSecond != 0);

        // Re-anchor the new position at max(position, now), not at the slot.
        auto newPositionNs = std::max(positionNs, nowNs) + incrementNs;

        if (state->position_ns.compare_exchange_strong(
                positionNs,
                newPositionNs,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return slotNs;
        }
        // Lost race: position has been refreshed by the failed CAS above, the
        // parameters are re-read at the loop top, including the disabled-limit case.
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace gcra_ipc
