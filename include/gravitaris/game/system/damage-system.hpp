#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Resolves bullet hits. Bullets are Chipmunk sensors (see RigidBodyDesc), so
// hits aren't detected by Chipmunk's own collision resolution; instead each
// tick this sweeps a segment query from the bullet's previous to current
// position, which stays correct regardless of bullet speed (no tunneling).
class DamageSystem {
private:
    flecs::world& m_registry;
    PhysicsSystem& m_physicsSystem;

public:
    DamageSystem(flecs::world& registry, PhysicsSystem& physicsSystem);

    void Update();
};

} // namespace Gravitaris
