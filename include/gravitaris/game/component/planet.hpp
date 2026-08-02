#pragma once

namespace Gravitaris {

// Marks a celestial body: the massive, gravity-well-forming things ships fly
// around, as opposed to ships, bullets or debris. Stable membership -- a planet
// is a planet for its whole life -- so a real component rather than a field on
// something else (see CLAUDE.md's ECS component design note).
//
// Carries the body's true collision radius (world units, *before*
// Transform::scale -- callers multiply by t.scale.x() themselves, same as the
// PhysicsBody-based lookup this replaced). A real data component (not an
// empty tag) so camera framing and the minimap can read it directly as a
// query term, without a live PhysicsSystem: a net client reconstructs it once
// at creation from the same Body resource by modelId (see
// SnapshotApplier::Apply), so this never needs to travel on the wire itself.
//
// Replication class: replicated (server -> clients), but derived client-side
// from already-replicated data (modelId) rather than sent directly.
struct Planet {
    float radius = 0.f;
    // A star rather than a rock: nothing survives touching one (DamageSystem),
    // so it is never a landing site for anybody, player or AI. A field rather
    // than a tag for the same reason `radius` is one -- it is fixed for the
    // body's whole life and read as part of ordinary Planet queries.
    bool star = false;
};

} // namespace Gravitaris
