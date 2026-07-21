#include <utility>
#include <vector>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/faction-system.hpp>
#include <gravitaris/game/system/conquest-system.hpp>

namespace Gravitaris {

ConquestSystem::ConquestSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                               GameEventQueue& eventQueue, FactionSystem& factionSystem)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
        , m_factionSystem(factionSystem)
{}

void ConquestSystem::Update()
{
    // Collected here, applied after the .each() below completes:
    // FactionSystem::GetOrCreate can create an entity, a structural change
    // flecs doesn't allow safely from inside an active iterator (observed
    // as an intermittent crash) -- so no FactionSystem call can happen
    // inside this loop itself.
    std::vector<std::pair<TeamId, std::uint32_t>> newClaims;

    m_registry.each([&](flecs::entity ship, LandingState& state, Team& shipTeam) {
        // == rather than >= so a ship parked long-term claims exactly once.
        if (!state.landed || state.landedTicks != CLAIM_TICKS) return;

        flecs::entity planet = m_entitySpawner.EntityForNetId(state.landedOnNetId);
        if (!planet.is_alive()) return;
        if (!planet.has<Orbit>()) return; // suns are not claimable

        Team* planetTeam = planet.try_get_mut<Team>();
        if (!planetTeam || planetTeam->id == shipTeam.id) return;

        // TODO(gravity-well-mode-plan.md Phase 2): once structures exist, an
        // enemy complex must be destroyed before the planet can flip.

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
