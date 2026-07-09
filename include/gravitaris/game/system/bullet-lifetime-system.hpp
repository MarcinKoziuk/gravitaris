#pragma once

#include <entt/entity/registry.hpp>

namespace Gravitaris {

class BulletLifetimeSystem {
private:
    entt::registry& m_registry;

public:
    explicit BulletLifetimeSystem(entt::registry& registry);

    void Update(double dt);
};

} // namespace Gravitaris
