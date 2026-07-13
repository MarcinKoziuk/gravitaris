#pragma once

#include <cstdint>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/resource/body.hpp>

namespace Gravitaris {

// Spawn intent: which space to join and which Body resource defines the
// shapes. Consumed by PhysicsSystem's observer, which allocates the Chipmunk
// state and sets PhysicsRef on the entity.
struct RigidBodyDesc {
    id_t spaceId{};
    ResourcePtr<Body> body;

    RigidBodyDesc() = default;

    RigidBodyDesc(id_t spaceId, const ResourcePtr<Body>& body)
            : spaceId(spaceId)
            , body(body)
    {}
};

// Handle into PhysicsSystem's body storage. Deliberately plain data: the
// Chipmunk handles live in PhysicsSystem, so archetype moves never relocate
// resource-owning state (see docs/adr/0002-physics-ownership.md). The
// generation guards against a recycled slot being freed through a stale ref.
struct PhysicsRef {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
};

} // namespace Gravitaris
