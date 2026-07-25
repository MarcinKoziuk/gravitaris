#pragma once

#include <cstdint>
#include <optional>

namespace Gravitaris {

// Own local monotonic tick counter for client-side prediction. Kept
// independent of the caller's `target` between corrections so consecutive
// predicted ticks are always exactly one PHYSICS_DELTA apart, matching what's
// actually being simulated.
//
// A free-running counter like this advances once per *executed* tick, but
// nothing guarantees ticks execute on schedule: rAF throttling on a
// backgrounded browser tab, GC hitches, and a fixed-step accumulator's own
// catch-up cap (which discards backlog rather than replaying it) all lose
// wall-clock time this counter never sees. Every lost tick is permanent
// backward drift vs. the server's wall-clock-paced step, and once it exceeds
// the input-lead window, every input sent is stamped in the server's past
// and dropped as stale -- hence the forward resync.
//
// The two directions are NOT symmetric, which is why they're handled
// differently below. Running behind the target is corrected by jumping the
// counter forward: tick numbers only ever increase, so nothing downstream
// notices. Running *ahead* of it cannot be corrected that way -- moving the
// counter backwards would re-issue tick numbers this client has already
// predicted, stamped input with and buffered for reconciliation, and the
// server (whose own counter only goes up) would discard every one of them as
// stale until real time caught back up. Instead the clock gives a tick back
// by telling the caller to skip a step entirely, at most one per
// MIN_TICKS_BETWEEN_SKIPS, so wall clock closes the gap while the counter
// stands still. That is what makes the input lead safely *reducible* at
// runtime (see NetClient::GetInputLeadTicks): lowering it lowers the target,
// and the clock walks down to meet it a tick at a time.
class PredictedTickClock {
public:
    // ~83ms at 60Hz; above the target's own jitter, below perceptible input
    // lag.
    static constexpr std::uint64_t RESYNC_THRESHOLD_TICKS = 5;

    // 0.5s at 60Hz between voluntary skips. One skipped tick freezes the own
    // ship for a frame; spacing them this far apart keeps a multi-tick
    // correction imperceptible, at the cost of taking that long per tick to
    // converge. Ignored when the clock is ahead by more than
    // RESYNC_THRESHOLD_TICKS -- that's no longer a nudge, and it's worth
    // spending consecutive skips to get back.
    static constexpr std::uint64_t MIN_TICKS_BETWEEN_SKIPS = 30;

    struct AdvanceResult {
        std::uint64_t tick;
        // The caller must NOT step the sim (or send input) this call -- the
        // clock is ahead of its target and is giving one tick back. `tick`
        // is not a tick to use when this is set.
        bool skip = false;
        // Set iff this call resynced forward -- the magnitude of that drift,
        // for the caller's own logging/diagnostics.
        std::optional<std::uint64_t> resyncDrift;
    };

    // Unconditional reset (e.g. on first spawn or a respawn under a new
    // NetId) -- no drift check, no resync reported.
    void Reset(std::uint64_t tick)
    {
        m_nextTick = tick;
        m_ticksSinceSkip = MIN_TICKS_BETWEEN_SKIPS;
    }

    // `target` is fractional (NetClient::EstimateCurrentServerTickF plus the
    // input lead) -- a whole-tick target would make the sub-tick part of the
    // drift read as a permanent ±1 disagreement and skip against it forever.
    AdvanceResult Advance(double target)
    {
        const double drift = target - static_cast<double>(m_nextTick);

        AdvanceResult result;
        if (drift > static_cast<double>(RESYNC_THRESHOLD_TICKS)) {
            result.resyncDrift = static_cast<std::uint64_t>(drift);
            m_nextTick = static_cast<std::uint64_t>(target);
        }
        else if (drift <= -1.0) {
            // Tolerating a full tick of being ahead before correcting keeps
            // an ordinary sub-tick disagreement from skipping every
            // MIN_TICKS_BETWEEN_SKIPS forever.
            const bool aheadFar = drift < -static_cast<double>(RESYNC_THRESHOLD_TICKS);
            if (aheadFar || m_ticksSinceSkip >= MIN_TICKS_BETWEEN_SKIPS) {
                m_ticksSinceSkip = 0;
                result.tick = m_nextTick;
                result.skip = true;
                return result;
            }
        }

        if (m_ticksSinceSkip < MIN_TICKS_BETWEEN_SKIPS) ++m_ticksSinceSkip;
        result.tick = m_nextTick++;
        return result;
    }

    [[nodiscard]] std::uint64_t Current() const { return m_nextTick; }

private:
    std::uint64_t m_nextTick = 0;
    std::uint64_t m_ticksSinceSkip = MIN_TICKS_BETWEEN_SKIPS;
};

} // namespace Gravitaris
