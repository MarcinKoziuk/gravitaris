#pragma once

#include <cstdint>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/resource/body.hpp>

namespace Gravitaris {

// Which collision rule a body's shapes play by (networking-plan Phase 9).
// Declared at spawn rather than inferred from components: PhysicsSystem's
// OnSet observer runs the instant RigidBodyDesc is emplaced, before the
// spawner has finished adding the rest of the entity's components, so there
// is nothing to infer from yet.
enum class CollisionClass : std::uint8_t {
    // Ordinary hard contact for every pair: planets, structures (High Ports,
    // docks), freighters. Ram/landing damage via PhysicsSystem's wildcard
    // postSolve handler.
    Default = 0,
    // Player- and AI-piloted ships. Identical to Default against everything
    // else -- landing on a planet is unchanged -- but ship-against-ship
    // resolves as gameplay instead of physics: they pass through each other,
    // and a fast enough contact destroys rather than bounces. See
    // PhysicsSystem::PreSolveShipPair.
    Ship = 1,
};

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
    CollisionClass collisionClass = CollisionClass::Default;

    RigidBodyDesc() = default;

    RigidBodyDesc(id_t spaceId, const ResourcePtr<Body>& body, bool sensor = false,
                  CollisionClass collisionClass = CollisionClass::Default)
            : spaceId(spaceId)
            , body(body)
            , sensor(sensor)
            , collisionClass(collisionClass)
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
