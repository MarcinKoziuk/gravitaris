#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Homing for every live Missile: lock the nearest hostile ship or structure,
// then each tick turn the missile's velocity toward it at a bounded rate
// while it accelerates up to its top speed. Runs inside the physics block,
// before Simulate(), so the steer it applies is what that tick integrates.
//
// Gravity is effectively overridden while a missile is under guidance: the
// speed is re-derived every tick rather than accumulated, so a well only
// bends the flight through the heading it inherits.
class MissileSystem {
public:
    // Max heading change per second. ~143 deg/s -- tight enough to chase a
    // turning fighter, loose enough that a missile can be outrun by cutting
    // hard across it.
    static constexpr double TURN_RATE = 2.5;

    // Acceleration up to TOP_SPEED, from ShipControlsSystem's launch speed.
    static constexpr double ACCELERATION = 120.0;
    static constexpr double TOP_SPEED = 300.0;

    MissileSystem(flecs::world& registry, EntitySpawner& entitySpawner, PhysicsSystem& physicsSystem);

    void Update();

private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    PhysicsSystem& m_physicsSystem;
};

} // namespace Gravitaris
