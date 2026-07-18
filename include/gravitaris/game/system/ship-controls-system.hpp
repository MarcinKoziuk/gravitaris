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

    // Ticks between shots while firePrimary is held (weapon cadence).
    // 7 (was 10) is ~50% more bullets/sec (8.6 vs 6).
    static constexpr std::uint32_t FIRE_COOLDOWN_TICKS = 7;

private:
    flecs::world& m_registry;

    EntitySpawner& m_entitySpawner;

    PhysicsSystem& m_physicsSystem;

    GameEventQueue& m_eventQueue;

public:
    explicit ShipControlsSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                                PhysicsSystem& physicsSystem, GameEventQueue& eventQueue);

    ~ShipControlsSystem() = default;

    void Update(std::uint64_t step);
};

} // namespace Gravitaris
