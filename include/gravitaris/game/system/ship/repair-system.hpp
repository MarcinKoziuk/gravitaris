#pragma once

#include <flecs.h>

#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Hull repair for a ship standing on one of its own developed planets: the
// only thing in the sim that gives hp back, and the reason a hurt pilot has
// somewhere to go rather than a hull fraction it can never climb out of.
// Player and AI alike -- coming home is the cost.
class RepairSystem {
private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    const EconomyConfig& m_config;

public:
    RepairSystem(flecs::world& registry, EntitySpawner& entitySpawner, const EconomyConfig& config);

    void Update();
};

} // namespace Gravitaris
