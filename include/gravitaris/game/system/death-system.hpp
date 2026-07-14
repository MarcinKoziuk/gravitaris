#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Destroys entities whose Damageable hp has reached zero. Ships explode into
// a ring of ownerless frag shrapnel (TeamId::None) that damages anyone nearby,
// including the killer -- a first-pass frag-grenade death.
class DeathSystem {
private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;

    void Explode(flecs::entity ship, std::uint64_t step);

public:
    DeathSystem(flecs::world& registry, EntitySpawner& entitySpawner);

    void Update(std::uint64_t step);
};

} // namespace Gravitaris
