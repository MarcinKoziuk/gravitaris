#pragma once

#include <gravitaris/game/resource/body.hpp>

namespace Gravitaris {

// The collision geometry of a replicated entity, carried alongside its
// Renderable so the client can resolve its own cosmetic hits (see
// CosmeticBulletDespawner) against real hull and shield shapes in a world that
// has no physics of its own (ADR 0001). The same Body resource the server built
// its Chipmunk shapes from, reached by the already-replicated modelId.
//
// Replication class: client-only, derived from the replicated modelId.
struct HitOutline {
    ResourcePtr<const Body> body;
};

} // namespace Gravitaris
