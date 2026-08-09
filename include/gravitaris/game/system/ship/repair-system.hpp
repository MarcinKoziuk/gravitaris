#pragma once

#include <flecs.h>

#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Everything in the sim that gives hp back, which is two things: a ship
// standing on one of its own developed planets, and a ship carrying an emitter
// that mends its hull wherever it is (the field plating, see
// UpgradeDef::Shield::hullRegenSeconds). The first is the reason a hurt pilot
// has somewhere to go rather than a hull fraction it can never climb out of;
// the second is slow enough that it is a reason to keep flying, not a reason
// not to come home. Player and AI alike.
class RepairSystem {
private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    const UpgradeCatalog& m_catalog;
    const EconomyConfig& m_config;

public:
    RepairSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                 const UpgradeCatalog& catalog, const EconomyConfig& config);

    void Update();

private:
    void RepairOnHomeGround();
    void MendUnderPlating();
};

} // namespace Gravitaris
