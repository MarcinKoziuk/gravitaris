#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Fighter-upgrade research (gravity-well-1997.md's Lab role): a faction's
// Labs pool their effort into one research bar, so more labs finish sooner.
// Each time it fills, one finished upgrade joins the faction's queue and the
// bar starts over; a same-team ship that lands at one of those labs' planets
// -- or at that planet's High Port -- is offered a draft of three upgrades
// from the pool (data/upgrades.toml), and taking one spends a place in the
// queue. The queue is bounded (economy.toml's stock_capacity, widened by the
// RESEARCH QUEUE upgrade): a side whose pilots never come home eventually
// idles its labs rather than banking the whole match.
//
// The draft is rolled once, against the collecting ship's levels combined
// with its faction's, and held until it picks; the player picks with
// Controls::upgradePick, an AI scores the three. A pick lands on the
// collecting ship, or -- for UpgradeScope::Faction -- on FactionState's own
// levels block, where it outlives that ship.
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
