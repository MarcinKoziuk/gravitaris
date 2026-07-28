#include <algorithm>

#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/system/combat/shield-system.hpp>

namespace Gravitaris {

ShieldSystem::ShieldSystem(flecs::world& registry, const UpgradeCatalog& catalog)
        : m_registry(registry)
        , m_catalog(catalog)
{}

void ShieldSystem::Update()
{
    m_registry.each([&](ShipLoadout& loadout) {
        const ShipStats stats = m_catalog.ResolveStats(loadout.levels);

        // Losing the emitter (there is no way to yet, but a stripped or
        // swapped loadout is coming) must not leave charge behind that
        // nothing can spend down.
        if (stats.shieldCapacity <= 0.f) {
            loadout.shieldHp = 0.f;
            loadout.shieldRegenDelay = 0;
            return;
        }

        if (loadout.shieldRegenDelay > 0) {
            --loadout.shieldRegenDelay;
            return;
        }

        loadout.shieldHp = std::min(stats.shieldCapacity,
                                    loadout.shieldHp + stats.shieldRegenPerSecond
                                            * static_cast<float>(Game::PHYSICS_DELTA));
    });
}

} // namespace Gravitaris
