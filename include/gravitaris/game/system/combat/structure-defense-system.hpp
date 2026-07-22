#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Base and High Port structures auto-fire at enemy ships in range
// (gravity-well-1997.md: "planetary defenses will automatically respond
// when enemy vessels are in range"). A turret doesn't rotate/aim like a
// ship -- it leads the target (same intercept math AIPilotSystem's guns
// use) and fires directly once in range and off cooldown.
class StructureDefenseSystem {
public:
    // World units; comfortably past a ship's usual combat range so flying
    // near a defended planet is genuinely risky, not just grazing distance.
    static constexpr double FIRE_RANGE = 400.0;

    // Ticks between shots -- slower than a ship's own FIRE_COOLDOWN_TICKS
    // (7): a static defense is a deterrent, not expected to out-DPS a
    // fighter head-on.
    static constexpr std::uint32_t FIRE_COOLDOWN_TICKS = 90;

private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;

public:
    StructureDefenseSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue);

    void Update();
};

} // namespace Gravitaris
