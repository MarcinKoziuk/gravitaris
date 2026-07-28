#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Fighter-upgrade research (gravity-well-1997.md's Lab role): a faction's
// Labs pool their effort into one research bar, so more labs finish sooner.
// When it fills, the faction's labs flash "upgrade ready" and a same-team
// ship that lands at one of those labs' planets -- or at that planet's High
// Port -- is offered a draft of three upgrades from the pool
// (data/upgrades.toml). Taking one restarts research.
//
// The draft is rolled once, against the collecting ship's own levels, and
// held until it picks; the player picks with Controls::upgradePick, an AI
// takes the first offer the same tick. Everything lands on the collecting
// ship (UpgradeScope::Ship) -- faction-wide passives are a later scope, not a
// second code path here.
//
// Runs after FactionSystem (whose Update creates the FactionState entities
// this reads) and after LandingStateSystem (whose landed flags gate pickup).
class ResearchSystem {
public:
    // How long one lab needs is data/economy.toml's [research]
    // seconds_per_upgrade. Materials are not spent yet (research is unfunded
    // for now); when it becomes a cost, it comes out of the accompanying
    // Base's finished materials, matching EconomySystem's funder rule.
    ResearchSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue,
                   const UpgradeCatalog& catalog, const EconomyConfig& config);

    void Update(std::uint64_t step);

private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;
    const UpgradeCatalog& m_catalog;
    const EconomyConfig& m_config;
};

} // namespace Gravitaris
