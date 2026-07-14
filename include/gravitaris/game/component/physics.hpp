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
    // true: shapes get no physical collision response (still queryable) --
    // bullets use this since hits are resolved by DamageSystem's swept
    // segment query rather than Chipmunk's own collision resolution.
    bool sensor = false;

    RigidBodyDesc() = default;

    RigidBodyDesc(id_t spaceId, const ResourcePtr<Body>& body, bool sensor = false)
            : spaceId(spaceId)
            , body(body)
            , sensor(sensor)
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
