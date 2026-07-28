#include <cstdint>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/gwell/faction-system.hpp>
#include <gravitaris/game/system/gwell/conquest-system.hpp>

namespace Gravitaris {

ConquestSystem::ConquestSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                               GameEventQueue& eventQueue, FactionSystem& factionSystem,
                               const EconomyConfig& config)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
        , m_factionSystem(factionSystem)
        , m_config(config)
{}

void ConquestSystem::Update()
{
    // Collected here, applied after the .each() below completes:
    // FactionSystem::GetOrCreate can create an entity, a structural change
    // flecs doesn't allow safely from inside an active iterator (observed
    // as an intermittent crash) -- so no FactionSystem call can happen
    // inside this loop itself.
    std::vector<std::pair<TeamId, std::uint32_t>> newClaims;

    // A planet's structures are its garrison: while one stands, the planet
    // stays with the team that built it, however long an enemy ship parks on
    // the surface. Taking a developed world means levelling it first.
    ankerl::unordered_dense::map<std::uint32_t, std::uint8_t> garrisonTeams;
    const auto noteGarrison = [&](std::uint32_t planetNetId, const Team& team) {
        garrisonTeams[planetNetId] |= static_cast<std::uint8_t>(1u << static_cast<int>(team.id));
    };
    m_registry.each([&](const Structure&, const Team& team, const PlanetSurfaceAttachment& attach) {
        noteGarrison(attach.planetNetId, team);
    });
    m_registry.each([&](const Structure&, const Team& team, const PlanetOrbitAttachment& attach) {
        noteGarrison(attach.planetNetId, team);
    });

    m_registry.each([&](flecs::entity ship, LandingState& state, Team& shipTeam) {
        // == rather than >= so a ship parked long-term claims exactly once.
        if (!state.landed || state.landedTicks != m_config.conquest.claimTicks) return;

        flecs::entity planet = m_entitySpawner.EntityForNetId(state.landedOnNetId);
        if (!planet.is_alive()) return;
        if (!planet.has<Orbit>()) return; // suns are not claimable

        Team* planetTeam = planet.try_get_mut<Team>();
        if (!planetTeam || planetTeam->id == shipTeam.id) return;

        const auto garrison = garrisonTeams.find(state.landedOnNetId);
        if (garrison != garrisonTeams.end()
            && (garrison->second & ~(1u << static_cast<int>(shipTeam.id))) != 0) {
            return;
        }

        planetTeam->id = shipTeam.id;
        state.lastFriendlySiteNetId = state.landedOnNetId;
        newClaims.emplace_back(shipTeam.id, state.landedOnNetId);

        const Transform& t = planet.get<Transform>();
        m_eventQueue.Emit(GameEventType::PlanetClaimed, planet,
                          Magnum::Vector2{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())},
                          static_cast<std::uint32_t>(shipTeam.id));
    });

    for (const auto& [team, landedOnNetId] : newClaims) {
        flecs::entity factionState = m_factionSystem.GetOrCreate(team);
        factionState.get_mut<FactionState>().lastLandingSiteNetId = landedOnNetId;
    }
}

} // namespace Gravitaris
