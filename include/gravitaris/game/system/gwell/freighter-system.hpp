#pragma once

#include <cstdint>

#include <flecs.h>

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
public:
    // World units/second -- slow, matching gravity-well-1997.md's
    // "Freighter... moves slowly" annotation.
    static constexpr double TRANSIT_SPEED = 40.0;

    // World units/second^2 -- ramps up to TRANSIT_SPEED over ~2s from a
    // standing start rather than snapping to it instantly, so the _thrust
    // visual (gated on "still below cruise speed", not "still in transit")
    // only shows while actually accelerating, coasting silent/thrustless
    // the rest of the trip -- there's no engine noise needed to hold a
    // constant velocity in vacuum.
    static constexpr double TRANSIT_ACCELERATION = 20.0;

    // "Close enough" to a planet to stop transiting and start orbiting it,
    // and the radius that orbit rides at. Past High Port/Space Dock/Sensor
    // Array's own orbitRadius (starting-complex.cpp, 180) so a freighter's
    // parking orbit doesn't sit on top of them.
    static constexpr double ARRIVAL_RADIUS = 220.0;

    // Ticks between cargo unloads (300 == 5s at Game::PHYSICS_DELTA) -- makes
    // the two-cargo unload read as sequential events rather than an instant
    // double-drop the moment the freighter arrives.
    static constexpr std::uint32_t CARGO_UNLOAD_INTERVAL_TICKS = 300;

    // Raw materials the first cargo pod hands to the target planet's
    // existing Base, if it has one (gravity-well-1997.md's "freighters...
    // deliver [raw materials] to needy planets" resupply role). Placeholder
    // magnitude pending playtesting -- roughly a minute of Colony->Base
    // supply (EconomySystem::SUPPLY_RATE) in one lump.
    static constexpr float CARGO_ONE_RAW_MATERIALS = 25.f;

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
