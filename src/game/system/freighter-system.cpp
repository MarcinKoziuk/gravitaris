#include <cmath>

#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/freighter-system.hpp>

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

    m_registry.each([&](flecs::entity freighter, Transform& transf, PhysicsRef& ref, Freighter& state) {
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
            return;
        }

        const Vector2d vel = (toPlanet / distance) * TRANSIT_SPEED;
        const Vector2d pos = transf.pos + vel * Game::PHYSICS_DELTA;
        m_physicsSystem.SetKinematicMotion(ref, pos, vel);
        transf.pos = pos;
        transf.vel = vel;
    });

    for (flecs::entity freighter : toDestruct) freighter.destruct();
    for (auto& [freighter, attach] : arrivals) freighter.set<PlanetOrbitAttachment>(attach);

    // Build: an arrived freighter resolves its order against the planet's
    // CURRENT structures (not what dispatch saw), then is consumed either
    // way -- if the order's already been fulfilled by someone else, the
    // materials it was carrying are simply lost, an accepted edge case
    // rather than redirecting it to a different missing structure.
    std::vector<flecs::entity> consumed;
    m_registry.each([&](flecs::entity freighter, const Team& team, const Freighter& state) {
        if (!state.arrived) return;

        const flecs::entity planet = m_entitySpawner.EntityForNetId(state.targetPlanetNetId);
        if (planet.is_alive() && !HasStructure(m_registry, state.targetPlanetNetId,
                                               state.buildOrder == BuildOrder::Base   ? StructureType::Base
                                               : state.buildOrder == BuildOrder::Colony ? StructureType::Colony
                                                                                        : StructureType::HighPort)) {
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
                              static_cast<std::uint32_t>(state.buildOrder));
        }

        consumed.push_back(freighter);
    });
    for (flecs::entity freighter : consumed) freighter.destruct();
}

} // namespace Gravitaris
