#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Recharges collected shields (ShipLoadout::shieldHp) and holds them to the
// capacity their tier grants. A hit restarts the emitter's delay
// (DamageSystem), so a ship under sustained fire never regains charge; the
// two shield types differ mostly in how long that delay is and how fast the
// refill runs once it expires (data/upgrades.toml).
//
// Runs before DamageSystem so a tick's regen is available to that tick's
// incoming fire, matching how fireCooldown is decremented before it's tested.
class ShieldSystem {
public:
    ShieldSystem(flecs::world& registry, const UpgradeCatalog& catalog);

    void Update();

private:
    flecs::world& m_registry;
    const UpgradeCatalog& m_catalog;
};

} // namespace Gravitaris
