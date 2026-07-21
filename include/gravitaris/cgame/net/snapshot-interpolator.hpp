#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include <gravitaris/game/net/snapshot.hpp>

namespace Gravitaris {

// Turns NetClient's buffered snapshot history into one synthetic snapshot
// for a target render tick (docs/networking-plan.md Phase 4 -- "render
// ~100ms behind, lerp between straddling snapshots"). Entities other than
// `exemptNetId` (the local player's own ship, rendered instead via Phase
// 5's ClientPrediction -- a real, locally-simulated entity, so it's omitted
// here entirely rather than given some snapshot-derived position) are
// interpolated between the two snapshots straddling `renderTick`, or
// extrapolated (capped) past the newest one if the client is running ahead
// of what it's received.
//
// Presence (entity created/destroyed) follows the newer of the two
// straddling snapshots: an entity destroyed between them is already absent
// in the output at the render tick, one freshly spawned appears at its
// exact (non-interpolated -- there's nothing to interpolate from) state.
class SnapshotInterpolator {
public:
    struct Params {
        // ADR/plan: ~50ms cap on extrapolating past the newest snapshot
        // (a client running further ahead than this snaps to the newest
        // known state instead of guessing further into the unknown).
        float extrapolationCapSeconds = 0.05f;
    };

    // `history` must be sorted ascending by tick with no duplicates
    // (NetClient::GetSnapshotHistory already guarantees this). `tickRate` is
    // the server's tick rate (NetClient::GetTickRate()), needed to convert
    // tick deltas to seconds for the extrapolation cap. Returns nullopt only
    // if history is empty (nothing received yet).
    //
    // `planetTick` (defaults to `renderTick` if omitted -- fine for a caller
    // that doesn't also run ClientPrediction, e.g. a test): the tick planets
    // are analytically evaluated at (EvaluateOrbit), separate from
    // `renderTick` used for every other entity. This MUST match whatever
    // tick ClientPrediction::SyncPlanetProxies most recently positioned its
    // gravity/collision proxies at (the last ticked prediction, typically
    // `estimatedServerTick + NetClient::INPUT_LEAD_TICKS`) -- not the
    // delayed `renderTick`, or a landed ship visibly desyncs from the
    // rendered planet surface by however far apart the two ticks are (worse
    // the higher the interpolation delay is set). See this class's own doc
    // comment on why planets are otherwise exempt from `renderTick`-based
    // interpolation/extrapolation.
    static std::optional<SnapshotData> Compute(const std::deque<SnapshotData>& history, std::uint64_t renderTick,
                                                std::uint32_t exemptNetId, float tickRate, const Params& params,
                                                std::optional<std::uint64_t> planetTick = std::nullopt);
};

} // namespace Gravitaris
