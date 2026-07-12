#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

class ShipControlsSystem {
private:
    flecs::world& m_registry;

    EntitySpawner& m_entitySpawner;

public:
    explicit ShipControlsSystem(flecs::world& registry, EntitySpawner& entitySpawner);

    ~ShipControlsSystem() = default;

    void Update(std::uint64_t step);
};

} // namespace Gravitaris
