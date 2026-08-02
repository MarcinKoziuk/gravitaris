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

        // A station deck counts as its planet's: a High Port only ever orbits
        // one, and the pairing that makes the rock home is down there.
        flecs::entity site = m_entitySpawner.EntityForNetId(landing.landedOnNetId);
        if (const PlanetOrbitAttachment* orbit = site.is_alive() ? site.try_get<PlanetOrbitAttachment>() : nullptr) {
            site = m_entitySpawner.EntityForNetId(orbit->planetNetId);
        }
        if (!IsHomePlanet(m_registry, site, team.id)) return;

        hull.hp = std::min(hull.hp + perTick, hull.maxHp);
    });
}

} // namespace Gravitaris
