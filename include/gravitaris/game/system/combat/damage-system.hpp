#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Resolves bullet hits. Bullets are Chipmunk sensors (see RigidBodyDesc), so
// hits aren't detected by Chipmunk's own collision resolution; instead each
// tick this sweeps a segment query from the bullet's previous to current
// position, which stays correct regardless of bullet speed (no tunneling).
//
// Also owns the two non-bullet damage sources, both drained from
// PhysicsSystem: ordinary hard contacts (landing/crash) and ship-against-ship
// rams, which are a gameplay rule rather than a physical one (see
// ResolveShipRams and networking-plan Phase 9).
class DamageSystem {
private:
    flecs::world& m_registry;
    PhysicsSystem& m_physicsSystem;
    GameEventQueue& m_eventQueue;

    void ResolveShipRams();

public:
    DamageSystem(flecs::world& registry, PhysicsSystem& physicsSystem, GameEventQueue& eventQueue);

    void Update();
};

} // namespace Gravitaris
