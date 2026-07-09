#include <vector>

#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/system/bullet-lifetime-system.hpp>

namespace Gravitaris {

BulletLifetimeSystem::BulletLifetimeSystem(entt::registry& registry)
    : m_registry(registry)
{}

void BulletLifetimeSystem::Update(double dt)
{
    std::vector<entt::entity> expired;

    auto view = m_registry.view<Bullet>();
    for (auto entity : view) {
        Bullet& bullet = view.get<Bullet>(entity);
        bullet.remainingLifetime -= dt;
        if (bullet.remainingLifetime <= 0.) {
            expired.push_back(entity);
        }
    }

    for (auto entity : expired) {
        m_registry.destroy(entity);
    }
}

} // namespace Gravitaris
