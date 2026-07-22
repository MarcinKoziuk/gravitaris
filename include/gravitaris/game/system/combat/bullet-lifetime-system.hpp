#pragma once

#include <flecs.h>

namespace Gravitaris {

class BulletLifetimeSystem {
private:
    flecs::world& m_registry;

public:
    explicit BulletLifetimeSystem(flecs::world& registry);

    void Update(double dt);
};

} // namespace Gravitaris
