#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Drives every Freighter's lifecycle (docs/gravity-well-mode-plan.md Phase
// 3): transit toward its target planet, then build.
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
public:
    // World units/second -- slow, matching gravity-well-1997.md's
    // "Freighter... moves slowly" annotation.
    static constexpr double TRANSIT_SPEED = 40.0;

    // Matches the orbit radius newly built/hand-placed High Ports use
    // (see starting-complex.cpp) -- "close enough" to a planet to stop
    // transiting and start orbiting it.
    static constexpr double ARRIVAL_RADIUS = 90.0;

private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    PhysicsSystem& m_physicsSystem;
    GameEventQueue& m_eventQueue;

public:
    FreighterSystem(flecs::world& registry, EntitySpawner& entitySpawner, PhysicsSystem& physicsSystem,
                    GameEventQueue& eventQueue);

    void Update();
};

} // namespace Gravitaris
