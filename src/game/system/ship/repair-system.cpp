#include <algorithm>

#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/gwell/home-site.hpp>
#include <gravitaris/game/system/ship/repair-system.hpp>
#include <gravitaris/game/game.hpp>

namespace Gravitaris {

RepairSystem::RepairSystem(flecs::world& registry, EntitySpawner& entitySpawner, const EconomyConfig& config)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_config(config)
{}

void RepairSystem::Update()
{
    const float perTick = m_config.repair.hullPerSecond * static_cast<float>(Game::PHYSICS_DELTA);
    if (perTick <= 0.f) return;

    m_registry.each([&](const LandingState& landing, const Team& team, Damageable& hull) {
        if (!landing.landed || landing.landedOnNetId == 0) return;
        if (hull.hp >= hull.maxHp) return;

        const flecs::entity site = m_entitySpawner.EntityForNetId(landing.landedOnNetId);
        const flecs::entity planet = m_entitySpawner.EntityForNetId(SitePlanetNetId(site));
        if (!IsHomePlanet(m_registry, planet, team.id)) return;

        hull.hp = std::min(hull.hp + perTick, hull.maxHp);
    });
}

} // namespace Gravitaris
