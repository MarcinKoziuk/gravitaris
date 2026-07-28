#pragma once

#include <flecs.h>

#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// The materials economy (docs/gravity-well-mode-plan.md Phase 3, mirroring
// gravity-well-1997.md's "Economic model"): Colony produces raw and
// supplies its own planet's Base/High Port directly; Base/High Port
// convert raw into finished and spend it building freighters; freighters
// (built here, driven to completion by FreighterSystem) grow a claimed
// -but-undeveloped friendly planet, Base -> Colony -> High Port, hands off.
//
// Scope simplification: only the construction case is handled -- a Lab (or
// a High Port, which is its own producer) dispatches a freighter to the
// nearest friendly planet still
// missing Base/Colony/High Port. Resupplying an already-complete but
// materials-starved planet (one with no Colony of its own) is the
// original's other freighter role and is deferred; nothing here models it.
class EconomySystem {
private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;
    const EconomyConfig& m_config;

public:
    // Every rate -- production, supply, conversion, and what a freighter or a
    // Base's own Lab costs -- comes from data/economy.toml.
    EconomySystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue,
                  const EconomyConfig& config);

    void Update();
};

} // namespace Gravitaris
