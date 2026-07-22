#include <algorithm>
#include <unordered_set>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/economy-system.hpp>

namespace Gravitaris {

namespace {

struct PlanetEconomy {
    flecs::entity colony, base, highPort, lab, spaceDock;
};

// Matches BuildStartingComplex's own layout (starting-complex.cpp) so a
// self-developed complex looks identical to a hand-assembled one.
const Vector2d LAB_OFFSET{-15., 15.};
const Vector2d COMM_CENTER_OFFSET{15., 15.};
constexpr double SELF_DEV_ORBIT_PHASE_OFFSET = 0.4;

// True if `planetNetId` already has a live structure of `type` -- same
// check FreighterSystem uses at build time, duplicated rather than shared
// (matches this codebase's established pattern of small per-system
// duplicates, e.g. ai-pilot-system.cpp/structure-defense-system.cpp's own
// copies of WrapToPi/SolveInterceptTime).
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

EconomySystem::EconomySystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
{}

void EconomySystem::Update()
{
    ankerl::unordered_dense::map<std::uint32_t, PlanetEconomy> byPlanet;

    m_registry.each([&](flecs::entity e, const Structure& s, const PlanetSurfaceAttachment& attach) {
        PlanetEconomy& pe = byPlanet[attach.planetNetId];
        switch (s.type) {
            case StructureType::Colony: pe.colony = e; break;
            case StructureType::Base:   pe.base = e; break;
            case StructureType::Lab:    pe.lab = e; break;
            default: break;
        }
    });
    m_registry.each([&](flecs::entity e, const Structure& s, const PlanetOrbitAttachment& attach) {
        PlanetEconomy& pe = byPlanet[attach.planetNetId];
        switch (s.type) {
            case StructureType::HighPort:  pe.highPort = e; break;
            case StructureType::SpaceDock: pe.spaceDock = e; break;
            default: break;
        }
    });

    // Production, local supply, conversion -- pure value mutation on
    // existing components, safe inline.
    for (auto& [netId, pe] : byPlanet) {
        if (pe.colony.is_alive()) {
            Structure& colony = pe.colony.get_mut<Structure>();
            colony.rawMaterials = std::min(colony.rawMaterials + RAW_PRODUCTION_PER_TICK, RAW_CAP);

            if (pe.base.is_alive()) {
                const float supplied = std::min(colony.rawMaterials, SUPPLY_RATE);
                colony.rawMaterials -= supplied;
                Structure& base = pe.base.get_mut<Structure>();
                base.rawMaterials = std::min(base.rawMaterials + supplied, RAW_CAP);
            }
            if (pe.highPort.is_alive()) {
                const float supplied = std::min(colony.rawMaterials, SUPPLY_RATE);
                colony.rawMaterials -= supplied;
                Structure& highPort = pe.highPort.get_mut<Structure>();
                highPort.rawMaterials = std::min(highPort.rawMaterials + supplied, RAW_CAP);
            }
        }

        for (flecs::entity converter : {pe.base, pe.highPort}) {
            if (!converter.is_alive()) continue;
            Structure& s = converter.get_mut<Structure>();
            const float converted = std::min(s.rawMaterials, CONVERSION_RATE);
            s.rawMaterials -= converted;
            s.finishedMaterials = std::min(s.finishedMaterials + converted, FINISHED_CAP);
        }
    }

    // Self-development (docs/gravity-well-mode-plan.md Phase 4): a Base
    // grows its own Lab then Comm Center, a High Port its own Space Dock
    // then Sensor Array, spending ITS OWN finished materials -- same-planet
    // and instant, unlike freighter-built Base/Colony/High Port which need
    // a trip to a (possibly different) planet. One at a time, new-unit
    // rule applies, same as everywhere else structures get built.
    for (auto& [netId, pe] : byPlanet) {
        if (pe.base.is_alive()) {
            Structure& base = pe.base.get_mut<Structure>();
            if (base.finishedMaterials >= SELF_DEVELOPMENT_COST) {
                if (!pe.lab.is_alive()) {
                    base.finishedMaterials -= SELF_DEVELOPMENT_COST;
                    const Team& team = pe.base.get<Team>();
                    flecs::entity planet = m_entitySpawner.EntityForNetId(netId);
                    flecs::entity built = m_entitySpawner.SpawnStructure(StructureType::Lab, "models/structures/lab"_id,
                                                                         planet, team.id, LAB_OFFSET);
                    const Transform& builtTransf = built.get<Transform>();
                    m_eventQueue.Emit(GameEventType::StructureBuilt, built,
                                      Magnum::Vector2{static_cast<float>(builtTransf.pos.x()),
                                                      static_cast<float>(builtTransf.pos.y())},
                                      static_cast<std::uint32_t>(StructureType::Lab));
                }
                else if (!HasStructure(m_registry, netId, StructureType::CommCenter)) {
                    base.finishedMaterials -= SELF_DEVELOPMENT_COST;
                    const Team& team = pe.base.get<Team>();
                    flecs::entity planet = m_entitySpawner.EntityForNetId(netId);
                    flecs::entity built = m_entitySpawner.SpawnStructure(
                            StructureType::CommCenter, "models/structures/comm-center"_id, planet, team.id,
                            COMM_CENTER_OFFSET);
                    const Transform& builtTransf = built.get<Transform>();
                    m_eventQueue.Emit(GameEventType::StructureBuilt, built,
                                      Magnum::Vector2{static_cast<float>(builtTransf.pos.x()),
                                                      static_cast<float>(builtTransf.pos.y())},
                                      static_cast<std::uint32_t>(StructureType::CommCenter));
                }
            }
        }

        if (pe.highPort.is_alive()) {
            Structure& highPort = pe.highPort.get_mut<Structure>();
            if (highPort.finishedMaterials >= SELF_DEVELOPMENT_COST) {
                const PlanetOrbitAttachment& hpAttach = pe.highPort.get<PlanetOrbitAttachment>();
                if (!pe.spaceDock.is_alive()) {
                    highPort.finishedMaterials -= SELF_DEVELOPMENT_COST;
                    const Team& team = pe.highPort.get<Team>();
                    flecs::entity planet = m_entitySpawner.EntityForNetId(netId);
                    flecs::entity built = m_entitySpawner.SpawnOrbitingStructure(
                            StructureType::SpaceDock, "models/structures/space-dock"_id, planet, team.id,
                            hpAttach.radius, hpAttach.direction, hpAttach.theta + SELF_DEV_ORBIT_PHASE_OFFSET);
                    const Transform& builtTransf = built.get<Transform>();
                    m_eventQueue.Emit(GameEventType::StructureBuilt, built,
                                      Magnum::Vector2{static_cast<float>(builtTransf.pos.x()),
                                                      static_cast<float>(builtTransf.pos.y())},
                                      static_cast<std::uint32_t>(StructureType::SpaceDock));
                }
                else if (!HasStructure(m_registry, netId, StructureType::SensorArray)) {
                    highPort.finishedMaterials -= SELF_DEVELOPMENT_COST;
                    const Team& team = pe.highPort.get<Team>();
                    flecs::entity planet = m_entitySpawner.EntityForNetId(netId);
                    flecs::entity built = m_entitySpawner.SpawnOrbitingStructure(
                            StructureType::SensorArray, "models/structures/sensor-array"_id, planet, team.id,
                            hpAttach.radius, hpAttach.direction, hpAttach.theta - SELF_DEV_ORBIT_PHASE_OFFSET);
                    const Transform& builtTransf = built.get<Transform>();
                    m_eventQueue.Emit(GameEventType::StructureBuilt, built,
                                      Magnum::Vector2{static_cast<float>(builtTransf.pos.x()),
                                                      static_cast<float>(builtTransf.pos.y())},
                                      static_cast<std::uint32_t>(StructureType::SensorArray));
                }
            }
        }
    }

    // Freighters already tasked to a planet: don't dispatch a second one.
    std::unordered_set<std::uint32_t> alreadyTasked;
    m_registry.each([&](const Freighter& f) { alreadyTasked.insert(f.targetPlanetNetId); });

    // Candidate friendly planets still missing Base/Colony/High Port, in
    // that priority order, sorted by NetId for deterministic tie-breaking
    // when a producer picks the nearest one.
    struct Candidate {
        flecs::entity planet;
        std::uint32_t netId;
        Vector2d pos;
        BuildOrder order;
    };
    std::vector<Candidate> candidates;
    m_registry.each([&](flecs::entity planet, const Transform& transf, const Team& team, const Orbit&) {
        if (team.id == TeamId::None) return;
        const std::uint32_t netId = planet.get<NetId>().value;
        if (alreadyTasked.count(netId)) return;

        BuildOrder order;
        if (!HasStructure(m_registry, netId, StructureType::Base)) {
            order = BuildOrder::Base;
        }
        else if (!HasStructure(m_registry, netId, StructureType::Colony)) {
            order = BuildOrder::Colony;
        }
        else if (!HasStructure(m_registry, netId, StructureType::HighPort)) {
            order = BuildOrder::HighPort;
        }
        else {
            return; // fully developed -- nothing to dispatch here
        }
        candidates.push_back({planet, netId, transf.pos, order});
    });
    std::sort(candidates.begin(), candidates.end(),
             [](const Candidate& a, const Candidate& b) { return a.netId < b.netId; });

    // Lab/Space Dock build ships "from finished materials of the Base/High
    // Port it accompanies" (gravity-well-1997.md) -- they hold no materials
    // store of their own (their own Structure::finishedMaterials field is
    // simply never written to), so production spends the ACCOMPANYING
    // structure's funds, not the producer's.
    for (auto& [netId, pe] : byPlanet) {
        for (auto [producer, funder] : {std::pair{pe.lab, pe.base}, std::pair{pe.spaceDock, pe.highPort}}) {
            if (!producer.is_alive() || !funder.is_alive()) continue;
            Structure& funds = funder.get_mut<Structure>();
            if (funds.finishedMaterials < FREIGHTER_COST) continue;

            const Team& producerTeam = producer.get<Team>();
            const Transform& producerTransf = producer.get<Transform>();

            const Candidate* nearest = nullptr;
            double nearestDist = 0.0;
            for (const Candidate& c : candidates) {
                if (alreadyTasked.count(c.netId)) continue; // claimed by an earlier producer this same tick
                if (c.planet.get<Team>().id != producerTeam.id) continue;
                const double dist = (c.pos - producerTransf.pos).length();
                if (!nearest || dist < nearestDist) {
                    nearest = &c;
                    nearestDist = dist;
                }
            }
            if (!nearest) continue;

            funds.finishedMaterials -= FREIGHTER_COST;
            m_entitySpawner.SpawnFreighter("models/ships/freighter-0"_id, producerTransf.pos, producerTeam.id,
                                          nearest->planet, nearest->order);
            alreadyTasked.insert(nearest->netId); // don't let another producer target it too, same tick
            break;
        }
    }
}

} // namespace Gravitaris
