#include <algorithm>
#include <cmath>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/gwell/economy-system.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/scenario/structure-layout.hpp>
#include <gravitaris/game/system/gwell/freighter-system.hpp>

namespace Gravitaris {

namespace {

// How close to the aim point counts as being on it. Under a tick of cruise
// travel, so the approach settles into the hold rather than straddling it.
constexpr double AIM_DEADBAND = 1.0;

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

// Base is always planetside (StructureLayout gives it a surface slot), so
// only the surface attachment needs checking here.
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
                                 PhysicsSystem& physicsSystem, GameEventQueue& eventQueue,
                                 const EconomyConfig& config)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_physicsSystem(physicsSystem)
        , m_eventQueue(eventQueue)
        , m_config(config)
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

        if (distance <= m_config.freighter.arrivalRadius) {
            double centerMass = 0.0;
            if (const GravitySource* source = planet.try_get<GravitySource>()) {
                centerMass = source->mass * static_cast<double>(source->multiplier);
            }
            const double theta = std::atan2(-toPlanet.y(), -toPlanet.x()); // angle from planet to freighter, now
            arrivals.emplace_back(freighter,
                                  PlanetOrbitAttachment{state.targetPlanetNetId, centerMass, m_config.freighter.arrivalRadius, theta, 1.0});
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
            double eta = distance / m_config.freighter.transitSpeed;
            for (int i = 0; i < 4; ++i) {
                const double theta = orbit->theta + orbit->angularSpeed * eta;
                aimPos = orbit->center + Vector2d{std::cos(theta), std::sin(theta)} * orbit->radius;
                eta = (aimPos - transf.pos).length() / m_config.freighter.transitSpeed;
            }
        }
        else {
            const double eta = distance / m_config.freighter.transitSpeed;
            aimPos = planetTransf.pos + planetTransf.vel * eta;
        }
        const Vector2d toAim = aimPos - transf.pos;
        const double aimDistance = toAim.length();

        // Station-keeping: the freighter can reach the rendezvous point well
        // before the planet does, and this close the residual toward it is
        // numerical noise rather than a direction. Flying that at cruise
        // crosses the point every tick, so the ship bounces back and forth
        // with its nose flipping end over end for as long as it waits. Hold
        // still instead, nose on the planet it's waiting for.
        if (aimDistance < AIM_DEADBAND) {
            m_physicsSystem.SetKinematicMotion(ref, transf.pos, Vector2d{},
                                               std::atan2(toPlanet.x(), -toPlanet.y()));
            transf.vel = Vector2d{};
            controls.actionFlags.thrustForward = false;
            return;
        }

        // Ramps toward cruise rather than snapping to it, so there's
        // an actual accelerating phase for the _thrust visual/audio below to
        // key off of; once at cruise speed it coasts thrustless (currentSpeed
        // reads back last tick's transf.vel, which this same block sets, so
        // it persists across ticks without a separate stored field). Capped so
        // a tick never carries it past the aim point either -- the deadband
        // above is narrower than a tick of cruise travel, so without this the
        // ship would jump straight over it.
        const double currentSpeed = transf.vel.length();
        const double speed = std::min({currentSpeed + m_config.freighter.transitAcceleration * Game::PHYSICS_DELTA,
                                       m_config.freighter.transitSpeed,
                                       aimDistance / Game::PHYSICS_DELTA});
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
        controls.actionFlags.thrustForward = speed < m_config.freighter.transitSpeed;
    });

    for (flecs::entity freighter : toDestruct) freighter.destruct();
    for (auto& [freighter, attach] : arrivals) freighter.set<PlanetOrbitAttachment>(attach);

    // Cargo: an arrived freighter unloads its two pods one at a time, gated
    // by the configured unload interval so the two events read as sequential.
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
        if (state.ticksSinceUnload < m_config.freighter.cargoUnloadIntervalTicks) return;
        state.ticksSinceUnload = 0;

        const flecs::entity planet = m_entitySpawner.EntityForNetId(state.targetPlanetNetId);

        // Somebody else's rock now. A transit is long enough that a world can
        // change hands during one, and unloading anyway put this side's
        // buildings on a planet the other side holds. The run is written off:
        // the freighter is consumed where it stands rather than turned around,
        // since dispatch has already been paid for and re-targeting it would
        // be a second decision this system has no business making.
        const Team* planetTeam = planet.is_alive() ? planet.try_get<Team>() : nullptr;
        if (planetTeam && planetTeam->id != team.id) {
            state.cargoRemaining = 0;
            consumed.push_back(freighter);
            return;
        }

        if (state.cargoRemaining == 2) {
            if (planet.is_alive()) {
                if (flecs::entity base = FindBase(m_registry, state.targetPlanetNetId); base.is_alive()) {
                    Structure& baseStructure = base.get_mut<Structure>();
                    baseStructure.rawMaterials = std::min(baseStructure.rawMaterials + m_config.freighter.cargoOneRawMaterials,
                                                          m_config.colony.rawCap);
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
                                                           team.id);
                    break;
                case BuildOrder::Colony:
                    built = m_entitySpawner.SpawnStructure(StructureType::Colony, "models/structures/colony"_id,
                                                           planet, team.id);
                    break;
                case BuildOrder::HighPort:
                    built = m_entitySpawner.SpawnOrbitingStructure(
                            StructureType::HighPort, "models/structures/high-port-0"_id, planet, team.id,
                            StructureLayout::OrbitRadius(planet.get<Planet>().radius), 1.0, 0.0);
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
