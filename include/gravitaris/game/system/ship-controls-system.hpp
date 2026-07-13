#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

class ShipControlsSystem {
public:
    // Forward thrust force (local -Y). Public so guidance can derive the
    // ship's available acceleration (force / mass).
    static constexpr double THRUST_FORCE = 140.0;

private:
    flecs::world& m_registry;

    EntitySpawner& m_entitySpawner;

    PhysicsSystem& m_physicsSystem;

public:
    explicit ShipControlsSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                                PhysicsSystem& physicsSystem);

    ~ShipControlsSystem() = default;

    void Update(std::uint64_t step);
};

} // namespace Gravitaris
