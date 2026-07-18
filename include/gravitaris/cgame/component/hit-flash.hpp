#pragma once

namespace Gravitaris {

// Client-side white-out flash on a just-hit entity, read by ModelRenderer2's
// per-instance flash attribute. Set to 1 by CGame when an Impact/LandingCrash
// GameEvent names this entity, decayed client-side every rendered frame.
// A plain always-present field rather than an added/removed-per-hit tag (see
// CLAUDE.md's ECS component design note); attached by CEntitySpawner::
// AddRenderable, so membership is stable for the entity's whole life.
//
// Replication class: client-only presentation state.
struct HitFlash {
    float amount = 0.f; // 1 right after a hit, decaying to 0
};

} // namespace Gravitaris
