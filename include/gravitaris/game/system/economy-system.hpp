#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// The materials economy (docs/gravity-well-mode-plan.md Phase 3, mirroring
// gravity-well-1997.md's "Economic model"): Colony produces raw and
// supplies its own planet's Base/High Port directly; Base/High Port
// convert raw into finished and spend it building freighters; freighters
// (built here, driven to completion by FreighterSystem) grow a claimed
// -but-undeveloped friendly planet, Base -> Colony -> High Port, hands off.
//
// Scope simplification: only the construction case is handled -- a Lab/
// Space Dock dispatches a freighter to the nearest friendly planet still
// missing Base/Colony/High Port. Resupplying an already-complete but
// materials-starved planet (one with no Colony of its own) is the
// original's other freighter role and is deferred; nothing here models it.
class EconomySystem {
public:
    // Materials/tick a Colony produces, and the store cap it produces into.
    static constexpr float RAW_PRODUCTION_PER_TICK = 0.5f;
    static constexpr float RAW_CAP = 200.f;

    // Materials/tick a Colony pushes to its own planet's Base and,
    // independently, its High Port -- two separate draws against the same
    // Colony store, one per structure, not a combined cap.
    static constexpr float SUPPLY_RATE = 0.4f;

    // Materials/tick a Base/High Port converts raw -> finished, and the
    // finished-store cap.
    static constexpr float CONVERSION_RATE = 0.3f;
    static constexpr float FINISHED_CAP = 200.f;

    // Finished materials a Lab/Space Dock spends to dispatch one freighter.
    static constexpr float FREIGHTER_COST = 60.f;

    // Finished materials a Base spends on its own Lab then Comm Center, or a
    // High Port on its own Space Dock then Sensor Array (Phase 4's "self
    // -development" -- one at a time, new-unit rule applies, same as
    // freighter construction but same-planet and instant, no freighter
    // trip needed). Placeholder magnitude pending playtesting.
    static constexpr float SELF_DEVELOPMENT_COST = 40.f;

private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;

public:
    EconomySystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue);

    void Update();
};

} // namespace Gravitaris
