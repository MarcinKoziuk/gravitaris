#pragma once

#include <cstdint>

#include <entt/entity/registry.hpp>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

class ShipControlsSystem {
private:
    entt::registry& m_registry;

    EntitySpawner& m_entitySpawner;

public:
    explicit ShipControlsSystem(entt::registry& registry, EntitySpawner& entitySpawner);

    ~ShipControlsSystem() = default;

    void Update(std::uint64_t step);
};

} // namespace Gravitaris
