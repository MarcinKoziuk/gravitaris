#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Drives every Freighter's lifecycle (docs/gravity-well-mode-plan.md Phase
// 3): transit toward its target planet, then unload its two cargo pods one
// at a time -- first topping up the target's existing Base with raw
// materials, then resolving its build order -- before being consumed.
//
// Scope simplification vs. the plan's "GNC GotoPoint/InterceptEntity"
// wording: transit is a plain constant-speed kinematic seek toward the
// target planet's live position, not full inertial thrust-and-rotate GNC
// flight. Freighters are background economy actors the player never pilots
// or fights head-on (only shoots at, as a target) -- a simple, always
// -correct homing motion is far more tractable to keep deterministic than
// wiring up FlightController/GuidanceParams per freighter, and still gives
// the "freighter visibly flies to the target planet" behavior the mode
// needs. On arrival, it stops seeking and gets a real PlanetOrbitAttachment
// (the same mechanism High Port uses), so StructureAttachmentSystem takes
// over its motion from then on -- FreighterSystem must therefore run before
// StructureAttachmentSystem so a freighter arriving this tick is already
// under that system's control by the time it runs.
class FreighterSystem {
private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    PhysicsSystem& m_physicsSystem;
    GameEventQueue& m_eventQueue;
    const EconomyConfig& m_config;

public:
    // Cruise speed, the ramp up to it, the arrival radius and the unload
    // cadence all come from data/economy.toml's [freighter] table. The ramp
    // exists so the _thrust visual (gated on "still below cruise speed", not
    // "still in transit") only shows while actually accelerating -- holding a
    // constant velocity in vacuum needs no engine.
    FreighterSystem(flecs::world& registry, EntitySpawner& entitySpawner, PhysicsSystem& physicsSystem,
                    GameEventQueue& eventQueue, const EconomyConfig& config);

    void Update();
};

} // namespace Gravitaris
