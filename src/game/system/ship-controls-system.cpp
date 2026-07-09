#include <cmath>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/util/chipmunk-safe.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr double BULLET_LIFETIME_SECONDS = 3.0;

ShipControlsSystem::ShipControlsSystem(entt::registry& registry, EntitySpawner& entitySpawner)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
{}

static inline void
cpBodyApplyTorque(cpBody *body, cpFloat torque)
{
    cpVect g = cpBodyGetPosition(body);
    cpVect c = cpBodyGetCenterOfGravity(body);
    cpBodyApplyImpulseAtLocalPoint(body, cpv(0.0, torque), cpv(1.0 + c.x, c.y));
    cpBodyApplyImpulseAtLocalPoint(body, cpv(0.0, -torque), cpv(-1.0 + c.x, c.y));
}

static std::pair<Vector2d, Vector2d> GetBulletSpawnPosAndVel(const Transform& transf, const Physics& phys)
{
    if (!phys.body->GetHardpoints().empty()) {
        Body::Hardpoint hp = phys.body->GetHardpoints().front();
        double s = std::sin(double(transf.rot));
        double c = std::cos(double(transf.rot));

        Vector2d pos(
                hp.pos.x() * c - hp.pos.y() * s,
                hp.pos.x() * s + hp.pos.y() * c
        );

        pos += transf.pos;

        Vector2d vel(
                200.0 * std::cos(double(transf.rot - Radd(1.5708))),
                200.0 * std::sin(double(transf.rot - Radd(1.5708)))
        );

        vel += transf.vel;

        return std::make_pair(pos, vel);
    }
    else {
        return std::make_pair(Vector2d{}, Vector2d{});
    }
}

void ShipControlsSystem::Update(std::uint64_t step)
{
    auto view = m_registry.view<Transform, Physics, Controls>();
    for (auto entity : view) {
        Transform& transf = view.get<Transform>(entity);
        Physics& phys = view.get<Physics>(entity);
        Controls& scontrols = view.get<Controls>(entity);

        cpBody* body = phys.cp.body.get();

        cpFloat ang = cpBodyGetAngularVelocity(body);
        const cpFloat maxAng = 15.0;

        cpBodyApplyTorque(body, -ang * 4);

        if (scontrols.actionFlags.rotateLeft && ang < maxAng) {
            cpBodyApplyTorque(body, 20.0);
        }
        if (scontrols.actionFlags.rotateRight && ang > -maxAng) {
            cpBodyApplyTorque(body, -20.0);
        }
        if (scontrols.actionFlags.thrustForward) {
            cpBodyApplyForceAtLocalPoint(body, cpv(0, -140), cpv(0, 0));
        }
        if (scontrols.actionFlags.firePrimary) {
            scontrols.actionFlags.firePrimary = false;
            std::pair<Vector2d, Vector2d> ret = GetBulletSpawnPosAndVel(transf, phys);

            entt::entity bulletEntity = m_entitySpawner.SpawnBullet("models/bullets/bullet-0"_id, ret.first, ret.second);
            m_registry.emplace<Bullet>(bulletEntity, BULLET_LIFETIME_SECONDS);
        }
        if (scontrols.actionFlags.fireSecondary) {
            scontrols.actionFlags.fireSecondary = false;
            std::pair<Vector2d, Vector2d> ret = GetBulletSpawnPosAndVel(transf, phys);
            m_entitySpawner.SpawnBullet("models/doodads/box"_id, ret.first, transf.vel);
        }
    }
}


} // namespace Gravitaris
