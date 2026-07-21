#pragma once

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

using Magnum::Vector2d;

// A decaying visual-only position offset for a mirror-world (net-client)
// entity, folding in any discontinuity between where SnapshotInterpolator's
// output was heading and where it actually lands this frame -- see
// SnapshotApplier::Apply. The mirror world is presentation-only (no physics,
// nothing reads it back into simulation), so unlike the own ship's
// reconciliation offset this is baked directly into Transform::pos rather
// than kept separate and applied only at render time.
//
// lastAuthoritativePos/Vel track the entity's continuity independently of
// Transform::pos. This is deliberate: Transform::pos is `authoritative +
// offset`, so if jump-detection extrapolated from it directly, a still-large
// (not yet decayed) offset would make next frame's prediction land on
// "authoritative + stale offset + vel*dt" instead of on the real trajectory,
// re-detecting roughly the same discontinuity as brand new every frame and
// adding to the offset again instead of letting it decay -- a compounding/
// runaway bug found in practice (freighters and orbiting structures appeared
// to "disappear" after a client-side lag spike). Extrapolating from these
// separate fields instead means each frame's jump check is always measured
// against the true last known state, so it only fires once per real
// discontinuity.
//
// Replication class: client-only presentation state.
struct RemoteSmoothing {
    Vector2d offset;
    Vector2d lastAuthoritativePos;
    Vector2d lastAuthoritativeVel;
    bool hasLast = false;
};

} // namespace Gravitaris
