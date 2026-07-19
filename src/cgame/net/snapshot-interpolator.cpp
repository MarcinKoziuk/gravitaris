#include <algorithm>
#include <cmath>

#include <gravitaris/cgame/net/snapshot-interpolator.hpp>

namespace Gravitaris {

namespace {

constexpr float PI = 3.14159265358979323846f;

float LerpFloat(float a, float b, float t)
{
    return a + (b - a) * t;
}

Magnum::Vector2 LerpVec(const Magnum::Vector2& a, const Magnum::Vector2& b, float t)
{
    return a + (b - a) * t;
}

// Shortest-arc angle interpolation: wraps the raw a->b delta into (-pi, pi]
// before scaling by t, so e.g. lerping from 179deg to -179deg moves 2deg
// through the wrap instead of the long way around through 0.
float LerpAngleShortest(float a, float b, float t)
{
    float delta = std::fmod(b - a + PI, 2.f * PI);
    if (delta < 0.f) delta += 2.f * PI;
    delta -= PI;
    return a + delta * t;
}

// Builds an entity's interpolated/extrapolated state from `from` (used for
// non-interpolatable/discrete fields and as the extrapolation base) with
// pos/rot/scale/vel/angVel taken from LerpVec/LerpAngleShortest against `to`
// at fraction t. When `to` is null, extrapolates `from` forward by
// `extrapolateSeconds` instead (clamped by the caller).
EntityState BuildState(const EntityState& from, const EntityState* to, float t, float extrapolateSeconds)
{
    EntityState out = from;
    if (to) {
        out.pos = LerpVec(from.pos, to->pos, t);
        out.rot = LerpAngleShortest(from.rot, to->rot, t);
        out.scale = LerpVec(from.scale, to->scale, t);
        out.vel = LerpVec(from.vel, to->vel, t);
        out.angVel = LerpFloat(from.angVel, to->angVel, t);
        // Discrete/non-interpolatable fields (type, modelId, teamId,
        // controlsFlags, hp) follow the newer snapshot -- `to` when
        // interpolating, matching the "presence follows the newer
        // straddling snapshot" rule for fields too, not just existence.
        out.controlsFlags = to->controlsFlags;
        out.hp = to->hp;
    }
    else {
        out.pos = from.pos + from.vel * extrapolateSeconds;
        out.rot = from.rot + from.angVel * extrapolateSeconds;
    }
    return out;
}

const EntityState* FindByNetId(const SnapshotData& snapshot, std::uint32_t netId)
{
    const auto it = std::find_if(snapshot.entities.begin(), snapshot.entities.end(),
                                 [&](const EntityState& e) { return e.netId == netId; });
    return it != snapshot.entities.end() ? &*it : nullptr;
}

} // namespace

std::optional<SnapshotData> SnapshotInterpolator::Compute(const std::deque<SnapshotData>& history,
                                                          std::uint64_t renderTick, std::uint32_t exemptNetId,
                                                          float tickRate, const Params& params)
{
    if (history.empty()) return std::nullopt;

    SnapshotData out;
    out.tick = renderTick;

    if (renderTick <= history.front().tick) {
        // Nothing older buffered to interpolate from -- clamp to the
        // earliest known state (also covers "just connected").
        out.entities = history.front().entities;
        out.events = history.front().events;
    }
    else if (renderTick >= history.back().tick) {
        const SnapshotData& latest = history.back();
        const float rawSeconds = static_cast<float>(renderTick - latest.tick) / std::max(tickRate, 1.f);
        const float extrapolateSeconds = std::min(rawSeconds, params.extrapolationCapSeconds);
        out.entities.reserve(latest.entities.size());
        for (const EntityState& e : latest.entities) {
            out.entities.push_back(BuildState(e, nullptr, 0.f, extrapolateSeconds));
        }
        out.events = latest.events;
    }
    else {
        // Binary search for the pair straddling renderTick: history[i].tick
        // <= renderTick < history[i + 1].tick.
        const auto upper = std::upper_bound(
                history.begin(), history.end(), renderTick,
                [](std::uint64_t tick, const SnapshotData& s) { return tick < s.tick; });
        const SnapshotData& newer = *upper;
        const SnapshotData& older = *(upper - 1);

        const float t = static_cast<float>(renderTick - older.tick) / static_cast<float>(newer.tick - older.tick);

        out.entities.reserve(newer.entities.size());
        for (const EntityState& newState : newer.entities) {
            const EntityState* oldState = FindByNetId(older, newState.netId);
            // No interpolation source (freshly spawned between the two
            // snapshots): its exact newer-snapshot state, nothing to lerp.
            out.entities.push_back(oldState ? BuildState(*oldState, &newState, t, 0.f) : newState);
        }
        out.events = newer.events;
    }

    // Own ship: prediction is Phase 5, so it still snaps to the latest known
    // state rather than rendering delayed/interpolated like everything else.
    if (exemptNetId != 0) {
        const EntityState* latestOwn = FindByNetId(history.back(), exemptNetId);
        if (latestOwn) {
            const auto it = std::find_if(out.entities.begin(), out.entities.end(),
                                         [&](const EntityState& e) { return e.netId == exemptNetId; });
            if (it != out.entities.end()) *it = *latestOwn;
            else out.entities.push_back(*latestOwn);
        }
    }

    return out;
}

} // namespace Gravitaris
