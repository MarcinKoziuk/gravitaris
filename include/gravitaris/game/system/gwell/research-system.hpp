#pragma once

#include <cstdint>
#include <vector>

#include <flecs.h>

#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>

namespace Gravitaris {

// The two currencies behind the tech tree, and the purchases that spend them.
//
// **Tech** (gravity-well-1997.md's Lab role): a faction's Labs pool their
// effort into one bar, so more labs pay out sooner. Each fill adds
// economy.toml's tech_per_fill to FactionState::techPoints, which the
// PERMANENT tree spends to raise the faction's unlocked rank of a def. That
// commits from anywhere -- the labs do the learning, not the hull -- and is
// uncapped, because what pulls a pilot home is a stock airframe rather than an
// idle lab.
//
// **Supplies** are the pilot's own (PilotAccount), accrue continuously and on
// kills, and survive death. They buy a rank onto the hull in the SHIP tree,
// and only while it is standing at a friendly lab planet or holding station at
// that planet's High Port -- which is what ResearchAccess::atLab marks.
//
// A purchase names its def, its tree and its rank (Controls::techPick); an AI
// is offered the same choice and scores it (UpgradeCatalog::PreferredFit and
// PreferredUnlock). Nothing is rolled and nothing is drafted: the whole pool
// is visible from the start, and what gates it is prerequisites, ranks and
// price.
//
// Runs after FactionSystem (whose Update creates the FactionState entities
// this reads) and after LandingStateSystem (whose landed flags gate fitting).
class ResearchSystem {
public:
    // How long one lab needs is data/economy.toml's [research]
    // seconds_per_tech. Materials are not spent yet (research is unfunded for
    // now); when it becomes a cost, it comes out of the accompanying Base's
    // finished materials, matching EconomySystem's funder rule.
    ResearchSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue,
                   const UpgradeCatalog& catalog, const EconomyConfig& config);

    void Update(std::uint64_t step);

    // Pays a kill into that pilot's account (DeathReport::killerPilotId).
    // Driven from the death signal rather than polled, since the victim is
    // destructed the same tick and nothing afterwards knows who did it. A zero
    // id, or one with no account, is a no-op.
    void AwardKill(std::uint32_t pilotId);

private:
    // One purchase per ship per tick, from Controls::techPick.
    void ApplyPurchases();

    // Creates an account for any pilot a hull names but nothing has opened one
    // for yet.
    void EnsureAccounts();

    [[nodiscard]] flecs::entity FindAccountEntity(std::uint32_t pilotId) const;
    [[nodiscard]] PilotAccount* FindAccount(std::uint32_t pilotId);
    [[nodiscard]] PilotAccount* AccountFor(flecs::entity ship);
    [[nodiscard]] FactionState* FactionFor(TeamId team);

    // Every account, by pilot id. Rebuilt at the top of each Update and used
    // for the rest of it, because a flecs query run inside another query's
    // callback yields nothing -- so the lookups this system needs mid-walk
    // have to come from a plain list gathered beforehand.
    struct Account {
        std::uint32_t pilotId = 0;
        flecs::entity entity;
    };
    std::vector<Account> m_accounts;

    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;
    const UpgradeCatalog& m_catalog;
    const EconomyConfig& m_config;

    // Supplies accrue at a fractional rate per tick; this carries the part of
    // a point that hasn't been paid out yet, so a slow rate still pays exactly
    // and every account advances on the same tick.
    float m_supplyRemainder = 0.f;
};

} // namespace Gravitaris
