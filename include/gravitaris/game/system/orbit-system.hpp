#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Drives every Orbit entity's kinematic body to its position and tangential
// velocity for the current tick, run before the physics step reads those
// positions for gravity and collision. The angular speed is derived from
// PhysicsSystem's gravity constant/multiplier each tick (see Orbit), so the
// result is deterministic and replay-safe while still tracking live gravity
// tuning.
class OrbitSystem {
    flecs::world& m_registry;
    PhysicsSystem& m_physicsSystem;

public:
    OrbitSystem(flecs::world& registry, PhysicsSystem& physicsSystem);

    void Update();
};

} // namespace Gravitaris
