#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Fighter-upgrade research (gravity-well-1997.md's Lab role): a faction's
// Labs pool their effort into one research bar, so more labs finish sooner.
// When it fills, the faction's labs flash "upgrade ready" and a same-team
// ship that lands at one of those labs' planets -- or at that planet's High
// Port -- collects it, which restarts research.
//
// Runs after FactionSystem (whose Update creates the FactionState entities
// this reads) and after LandingStateSystem (whose landed flags gate pickup).
class ResearchSystem {
public:
    // Seconds one lab needs to finish an upgrade alone; two labs halve it.
    static constexpr double RESEARCH_SECONDS = 30.0;

    // What collecting one upgrade grants, and the rack's size -- the sidebar
    // draws one tick per stored missile, so the cap is also how wide that row
    // has to be. Placeholder magnitudes pending playtesting.
    static constexpr int MISSILES_PER_UPGRADE = 3;
    static constexpr int MISSILE_CAPACITY = 8;

    // Materials are not spent yet (research is unfunded for now); when it
    // becomes a cost, it comes out of the accompanying Base's finished
    // materials, matching EconomySystem's funder rule.
    ResearchSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue);

    void Update();

private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;
};

} // namespace Gravitaris
