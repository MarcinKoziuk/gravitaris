#include <algorithm>

#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/system/combat/shield-system.hpp>

namespace Gravitaris {

static void RegenPlates(ShipLoadout& loadout, const ShipStats& stats);

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
            loadout.plates = {};
            loadout.plateRegenDelay = {};
            return;
        }

        if (IsPlated(loadout)) {
            RegenPlates(loadout, stats);
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

static void RegenPlates(ShipLoadout& loadout, const ShipStats& stats)
{
    const float plateCapacity = PerPlate(loadout, stats.shieldCapacity);
    const float plateRegen = PerPlate(loadout, stats.shieldRegenPerSecond)
                             * static_cast<float>(Game::PHYSICS_DELTA);

    float total = 0.f;
    for (std::uint8_t i = 0; i < loadout.plateCount; ++i) {
        if (loadout.plateRegenDelay[i] > 0) {
            --loadout.plateRegenDelay[i];
        }
        else {
            loadout.plates[i] = std::min(plateCapacity, loadout.plates[i] + plateRegen);
        }
        total += loadout.plates[i];
    }

    loadout.shieldHp = total;
}

} // namespace Gravitaris
