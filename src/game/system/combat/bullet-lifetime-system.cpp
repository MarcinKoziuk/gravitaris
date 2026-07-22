#include <vector>

#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/system/combat/bullet-lifetime-system.hpp>

namespace Gravitaris {

BulletLifetimeSystem::BulletLifetimeSystem(flecs::world& registry)
    : m_registry(registry)
{}

void BulletLifetimeSystem::Update(double dt)
{
    std::vector<flecs::entity> expired;

    m_registry.each([&](flecs::entity entity, Bullet& bullet) {
        bullet.remainingLifetime -= dt;
        if (bullet.remainingLifetime <= 0.) {
            expired.push_back(entity);
        }
    });

    for (auto entity : expired) {
        entity.destruct();
    }
}

} // namespace Gravitaris
