#pragma once

#include <flecs.h>

#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Claiming planets by landing on them (docs/gravity-well-mode-plan.md Phase
// 1): a team's ship that stays safely landed on a claimable planet for
// claim_ticks (data/economy.toml) takes ownership, unless another team still
// has a structure standing there. Suns are excluded (no Orbit component -- you can't land on
// them anyway, per IDEAS.md).
class ConquestSystem {
private:
    flecs::world& m_registry;

    EntitySpawner& m_entitySpawner;

    GameEventQueue& m_eventQueue;
    FactionSystem& m_factionSystem;
    const EconomyConfig& m_config;

public:
    // How long a ship must stay safely landed before the claim fires is
    // data/economy.toml's [conquest] claim_ticks.
    ConquestSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue,
                   FactionSystem& factionSystem, const EconomyConfig& config);

    void Update();
};

} // namespace Gravitaris
