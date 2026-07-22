#pragma once

#include <cstdint>
#include <optional>

namespace Gravitaris {

// Own local monotonic tick counter for client-side prediction. Kept
// independent of NetClient::EstimateCurrentServerTick() (a wall-clock,
// jittery estimate) between resyncs so consecutive predicted ticks are
// always exactly one PHYSICS_DELTA apart, matching what's actually being
// simulated -- only resynced when drift grows past RESYNC_THRESHOLD_TICKS.
//
// A free-running counter like this advances once per *executed* tick, but
// nothing guarantees ticks execute on schedule: rAF throttling on a
// backgrounded browser tab, GC hitches, and a fixed-step accumulator's own
// catch-up cap (which discards backlog rather than replaying it) all lose
// wall-clock time this counter never sees. Every lost tick is permanent
// backward drift vs. the server's wall-clock-paced step, and once it exceeds
// the input-lead window, every input sent is stamped in the server's past
// and dropped as stale -- hence the resync.
class PredictedTickClock {
public:
    // ~83ms at 60Hz; above the wall-clock estimate's own jitter, below
    // perceptible input lag.
    static constexpr std::uint64_t RESYNC_THRESHOLD_TICKS = 5;

    struct AdvanceResult {
        std::uint64_t tick;
        // Set iff this call resynced (drift exceeded the threshold) -- the
        // magnitude of that drift, for the caller's own logging/diagnostics.
        std::optional<std::uint64_t> resyncDrift;
    };

    // Unconditional reset (e.g. on first spawn or a respawn under a new
    // NetId) -- no drift check, no resync reported.
    void Reset(std::uint64_t tick) { m_nextTick = tick; }

    // Resyncs to `target` first if drifted more than RESYNC_THRESHOLD_TICKS,
    // then returns the tick to use this call and advances by one.
    AdvanceResult Advance(std::uint64_t target)
    {
        const std::uint64_t drift = target > m_nextTick ? target - m_nextTick : m_nextTick - target;

        AdvanceResult result;
        if (drift > RESYNC_THRESHOLD_TICKS) {
            result.resyncDrift = drift;
            m_nextTick = target;
        }
        result.tick = m_nextTick++;
        return result;
    }

    [[nodiscard]] std::uint64_t Current() const { return m_nextTick; }

private:
    std::uint64_t m_nextTick = 0;
};

} // namespace Gravitaris
