#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Drives every PlanetSurfaceAttachment/PlanetOrbitAttachment entity's
// kinematic body from its parent planet's CURRENT-tick position/velocity
// (resolved by NetId via EntitySpawner), so planetside/orbital structures
// track their planet as it travels its own orbit instead of drifting away
// in fixed world space. Must run after OrbitSystem (the parent planet's
// position for this tick needs to already be fresh) and before
// PhysicsSystem::Simulate (gravity/collision should see today's position,
// not last tick's).
class StructureAttachmentSystem {
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    PhysicsSystem& m_physicsSystem;

public:
    StructureAttachmentSystem(flecs::world& registry, EntitySpawner& entitySpawner, PhysicsSystem& physicsSystem);

    void Update();
};

} // namespace Gravitaris
