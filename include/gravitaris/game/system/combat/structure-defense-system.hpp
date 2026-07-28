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
private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;
    const UpgradeCatalog& m_catalog;

public:
    // Range, cadence and the round itself all come from data/upgrades.toml's
    // [turret] table: a static defense fires a ship's round far more slowly,
    // being a deterrent rather than something expected to out-DPS a fighter
    // head-on.
    StructureDefenseSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue,
                           const UpgradeCatalog& catalog);

    void Update();
};

} // namespace Gravitaris
