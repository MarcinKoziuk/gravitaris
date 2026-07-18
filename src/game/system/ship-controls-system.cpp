#include <cmath>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/util/chipmunk-safe.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr double BULLET_LIFETIME_SECONDS = 3.0;
static constexpr float BULLET_DAMAGE = 10.f;
static constexpr double BULLET_MUZZLE_SPEED = 200.0; // matches ai-pilot-system's BULLET_SPEED; 33% slower than the original 300
static constexpr float BOX_HP = 30.f; // a couple of primary hits or one ram

ShipControlsSystem::ShipControlsSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                                       PhysicsSystem& physicsSystem, GameEventQueue& eventQueue)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_physicsSystem(physicsSystem)
        , m_eventQueue(eventQueue)
{}

static inline void
cpBodyApplyTorque(cpBody *body, cpFloat torque)
{
    cpVect g = cpBodyGetPosition(body);
    cpVect c = cpBodyGetCenterOfGravity(body);
    cpBodyApplyImpulseAtLocalPoint(body, cpv(0.0, torque), cpv(1.0 + c.x, c.y));
    cpBodyApplyImpulseAtLocalPoint(body, cpv(0.0, -torque), cpv(-1.0 + c.x, c.y));
}

static std::pair<Vector2d, Vector2d> GetBulletSpawnPosAndVel(const Transform& transf, const PhysicsBody& phys)
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
                BULLET_MUZZLE_SPEED * std::cos(double(transf.rot - Radd(1.5708))),
                BULLET_MUZZLE_SPEED * std::sin(double(transf.rot - Radd(1.5708)))
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
    m_registry.each([&](flecs::entity entity, Transform& transf, PhysicsRef& ref, Controls& scontrols) {
        PhysicsBody& phys = m_physicsSystem.GetBody(ref);
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
            cpBodyApplyForceAtLocalPoint(body, cpv(0, -ShipControlsSystem::THRUST_FORCE), cpv(0, 0));
        }
        if (scontrols.fireCooldown > 0) {
            --scontrols.fireCooldown;
        }
        // firePrimary is held, not one-shot; the cooldown paces the fire rate
        // so holding the button auto-fires at a fixed cadence.
        if (scontrols.actionFlags.firePrimary && scontrols.fireCooldown == 0) {
            scontrols.fireCooldown = ShipControlsSystem::FIRE_COOLDOWN_TICKS;
            std::pair<Vector2d, Vector2d> ret = GetBulletSpawnPosAndVel(transf, phys);

            const Team* shooterTeam = entity.try_get<Team>();
            flecs::entity bulletEntity = m_entitySpawner.SpawnBullet(
                    "models/bullets/bullet-0"_id, ret.first, ret.second, /*sensor=*/true);
            bulletEntity.emplace<Bullet>(BULLET_LIFETIME_SECONDS,
                                         shooterTeam ? shooterTeam->id : TeamId::Blue,
                                         BULLET_DAMAGE);

            m_eventQueue.Emit(GameEventType::BulletFired, entity,
                              Magnum::Vector2{static_cast<float>(ret.first.x()),
                                              static_cast<float>(ret.first.y())});
        }
        if (scontrols.actionFlags.fireSecondary) {
            scontrols.actionFlags.fireSecondary = false;
            std::pair<Vector2d, Vector2d> ret = GetBulletSpawnPosAndVel(transf, phys);
            flecs::entity box = m_entitySpawner.SpawnBullet("models/doodads/box"_id, ret.first, transf.vel);
            box.emplace<Damageable>(Damageable{BOX_HP, BOX_HP});

            m_eventQueue.Emit(GameEventType::BulletFired, entity,
                              Magnum::Vector2{static_cast<float>(ret.first.x()),
                                              static_cast<float>(ret.first.y())});
        }
    });
}


} // namespace Gravitaris
