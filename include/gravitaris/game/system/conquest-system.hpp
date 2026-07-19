#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Claiming planets by landing on them (docs/gravity-well-mode-plan.md Phase
// 1): a team's ship that stays safely landed on a claimable planet for
// CLAIM_TICKS takes ownership. Suns are excluded (no Orbit component -- you
// can't land on them anyway, per IDEAS.md).
class ConquestSystem {
public:
    // Consecutive landed ticks before the claim fires: long enough that a
    // slow bounce or graze never claims, short enough to feel immediate
    // after a real touchdown (1s at the fixed tick).
    static constexpr std::uint32_t CLAIM_TICKS = 60;

private:
    flecs::world& m_registry;

    EntitySpawner& m_entitySpawner;

    GameEventQueue& m_eventQueue;

public:
    ConquestSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue);

    void Update();
};

} // namespace Gravitaris
