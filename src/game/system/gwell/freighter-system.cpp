#include <algorithm>
#include <cmath>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/gwell/economy-system.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/gwell/freighter-system.hpp>

namespace Gravitaris {

namespace {

// Planetside offsets match starting-complex.cpp's own layout, so a
// freighter-built complex looks identical to a hand-assembled one.
const Vector2d BASE_OFFSET{-15., -10.};
const Vector2d COLONY_OFFSET{15., -10.};
constexpr double HIGH_PORT_ORBIT_RADIUS = 90.0;

// True if `planetNetId` already has a live structure of `type` attached to
// it (planetside or orbital) -- the "new-unit rule", re-checked at build
// time rather than trusting what dispatch decided, in case someone else
// already built it since.
bool HasStructure(flecs::world& registry, std::uint32_t planetNetId, StructureType type)
{
    bool found = false;
    registry.each([&](const Structure& s, const PlanetSurfaceAttachment& attach) {
        if (attach.planetNetId == planetNetId && s.type == type) found = true;
    });
    registry.each([&](const Structure& s, const PlanetOrbitAttachment& attach) {
        if (attach.planetNetId == planetNetId && s.type == type) found = true;
    });
    return found;
}

// Base is always planetside (see starting-complex.cpp/FreighterSystem's own
// BASE_OFFSET), so only the surface attachment needs checking here.
flecs::entity FindBase(flecs::world& registry, std::uint32_t planetNetId)
{
    flecs::entity found;
    registry.each([&](flecs::entity e, const Structure& s, const PlanetSurfaceAttachment& attach) {
        if (attach.planetNetId == planetNetId && s.type == StructureType::Base) found = e;
    });
    return found;
}

} // namespace

FreighterSystem::FreighterSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                                 PhysicsSystem& physicsSystem, GameEventQueue& eventQueue)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_physicsSystem(physicsSystem)
        , m_eventQueue(eventQueue)
{}

void FreighterSystem::Update()
{
    // Transit: seek the target planet's current position. Arrivals are
    // collected here and given a real PlanetOrbitAttachment afterward,
    // outside the iteration -- adding a component moves the entity to a
    // different archetype/table, which the same collect-then-mutate
    // discipline used elsewhere in this codebase (DeathSystem's destructs,
    // DamageSystem's spent bullets) applies to just as much as destruction
    // does.
    std::vector<flecs::entity> toDestruct;
    std::vector<std::pair<flecs::entity, PlanetOrbitAttachment>> arrivals;

    m_registry.each([&](flecs::entity freighter, Transform& transf, PhysicsRef& ref, Freighter& state,
                        Controls& controls) {
        if (state.arrived) return;

        const flecs::entity planet = m_entitySpawner.EntityForNetId(state.targetPlanetNetId);
        if (!planet.is_alive()) {
            toDestruct.push_back(freighter); // target gone -- shouldn't normally happen
            return;
        }

        const Transform& planetTransf = planet.get<Transform>();
        const Vector2d toPlanet = planetTransf.pos - transf.pos;
        const double distance = toPlanet.length();

        if (distance <= ARRIVAL_RADIUS) {
            double centerMass = 0.0;
            if (const GravitySource* source = planet.try_get<GravitySource>()) {
                centerMass = source->mass * static_cast<double>(source->multiplier);
            }
            const double theta = std::atan2(-toPlanet.y(), -toPlanet.x()); // angle from planet to freighter, now
            arrivals.emplace_back(freighter,
                                  PlanetOrbitAttachment{state.targetPlanetNetId, centerMass, ARRIVAL_RADIUS, theta, 1.0});
            state.arrived = true;
            controls.actionFlags.thrustForward = false;
            return;
        }

        // Lead the planet's orbital motion: aim at where it'll be at ETA, not
        // where it is now, otherwise the freighter chases a moving target and
        // flies a curved pursuit path instead of a straight line. A planet's
        // future position is a known closed form (Orbit's theta/angularSpeed,
        // same as EvaluateOrbit), so solve for the intercept exactly via
        // fixed-point iteration on the ETA rather than a one-shot linear
        // extrapolation of the planet's current velocity -- linear
        // extrapolation runs tangent to the orbit's curve, so it badly
        // overshoots for a distant/slow-closing freighter and only drags
        // itself back in line as ETA shrinks near arrival. A handful of
        // iterations converges well within a tick's positional tolerance.
        Vector2d aimPos = planetTransf.pos;
        if (const Orbit* orbit = planet.try_get<Orbit>(); orbit && orbit->radius > 0.0) {
            double eta = distance / TRANSIT_SPEED;
            for (int i = 0; i < 4; ++i) {
                const double theta = orbit->theta + orbit->angularSpeed * eta;
                aimPos = orbit->center + Vector2d{std::cos(theta), std::sin(theta)} * orbit->radius;
                eta = (aimPos - transf.pos).length() / TRANSIT_SPEED;
            }
        }
        else {
            const double eta = distance / TRANSIT_SPEED;
            aimPos = planetTransf.pos + planetTransf.vel * eta;
        }
        const Vector2d toAim = aimPos - transf.pos;
        const double aimDistance = std::max(toAim.length(), 1e-6);

        // Ramps toward TRANSIT_SPEED rather than snapping to it, so there's
        // an actual accelerating phase for the _thrust visual/audio below to
        // key off of; once at cruise speed it coasts thrustless (currentSpeed
        // reads back last tick's transf.vel, which this same block sets, so
        // it persists across ticks without a separate stored field).
        const double currentSpeed = transf.vel.length();
        const double speed = std::min(currentSpeed + TRANSIT_ACCELERATION * Game::PHYSICS_DELTA, TRANSIT_SPEED);
        const Vector2d vel = (toAim / aimDistance) * speed;
        const Vector2d pos = transf.pos + vel * Game::PHYSICS_DELTA;
        // Nose is local -Y (see ShipControlsSystem::ApplyMovement's thrust
        // direction), so facing along vel means rot = atan2(vel.x, -vel.y).
        const double rot = std::atan2(vel.x(), -vel.y());
        m_physicsSystem.SetKinematicMotion(ref, pos, vel, rot);
        transf.pos = pos;
        transf.vel = vel;

        // Drives the _thrust visual/audio (and their replication) only --
        // motion stays SetKinematicMotion's above; a kinematic body ignores
        // ShipControlsSystem's forces. Only lit while still ramping up to
        // cruise speed -- coasting in vacuum needs no visible thrust.
        controls.actionFlags.thrustForward = speed < TRANSIT_SPEED;
    });

    for (flecs::entity freighter : toDestruct) freighter.destruct();
    for (auto& [freighter, attach] : arrivals) freighter.set<PlanetOrbitAttachment>(attach);

    // Cargo: an arrived freighter unloads its two pods one at a time, gated
    // by CARGO_UNLOAD_INTERVAL_TICKS so the two events read as sequential.
    // Cargo 1 tops up the target's existing Base with raw materials (a
    // no-op if it doesn't have one yet -- e.g. this freighter's own build
    // order IS to build that Base). Cargo 2 resolves the freighter's build
    // order against the planet's CURRENT structures (not what dispatch
    // saw), then the freighter is consumed either way -- if the order's
    // already been fulfilled by someone else, cargo 2 is simply lost, an
    // accepted edge case rather than redirecting it to a different missing
    // structure.
    std::vector<flecs::entity> consumed;
    m_registry.each([&](flecs::entity freighter, const Team& team, Freighter& state) {
        if (!state.arrived || state.cargoRemaining == 0) return;

        ++state.ticksSinceUnload;
        if (state.ticksSinceUnload < CARGO_UNLOAD_INTERVAL_TICKS) return;
        state.ticksSinceUnload = 0;

        const flecs::entity planet = m_entitySpawner.EntityForNetId(state.targetPlanetNetId);

        if (state.cargoRemaining == 2) {
            if (planet.is_alive()) {
                if (flecs::entity base = FindBase(m_registry, state.targetPlanetNetId); base.is_alive()) {
                    Structure& baseStructure = base.get_mut<Structure>();
                    baseStructure.rawMaterials = std::min(baseStructure.rawMaterials + CARGO_ONE_RAW_MATERIALS,
                                                          EconomySystem::RAW_CAP);
                }
            }
            state.cargoRemaining = 1;
            return;
        }

        const StructureType orderedType = state.buildOrder == BuildOrder::Base   ? StructureType::Base
                                         : state.buildOrder == BuildOrder::Colony ? StructureType::Colony
                                                                                  : StructureType::HighPort;
        if (planet.is_alive() && !HasStructure(m_registry, state.targetPlanetNetId, orderedType)) {
            flecs::entity built;
            switch (state.buildOrder) {
                case BuildOrder::Base:
                    built = m_entitySpawner.SpawnStructure(StructureType::Base, "models/structures/base"_id, planet,
                                                           team.id, BASE_OFFSET);
                    break;
                case BuildOrder::Colony:
                    built = m_entitySpawner.SpawnStructure(StructureType::Colony, "models/structures/colony"_id,
                                                           planet, team.id, COLONY_OFFSET);
                    break;
                case BuildOrder::HighPort:
                    built = m_entitySpawner.SpawnOrbitingStructure(StructureType::HighPort,
                                                                   "models/structures/high-port"_id, planet, team.id,
                                                                   HIGH_PORT_ORBIT_RADIUS, 1.0, 0.0);
                    break;
            }
            const Transform& builtTransf = built.get<Transform>();
            m_eventQueue.Emit(GameEventType::StructureBuilt, built,
                              Magnum::Vector2{static_cast<float>(builtTransf.pos.x()),
                                              static_cast<float>(builtTransf.pos.y())},
                              static_cast<std::uint32_t>(orderedType));
        }

        state.cargoRemaining = 0;
        consumed.push_back(freighter);
    });
    for (flecs::entity freighter : consumed) freighter.destruct();
}

} // namespace Gravitaris
