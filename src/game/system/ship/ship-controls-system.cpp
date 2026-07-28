#include <cmath>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/util/chipmunk-safe.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/missile.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr float BOX_HP = 30.f; // a couple of primary hits or one ram
static constexpr double HALF_PI = 1.5707963267948966;

ShipControlsSystem::ShipControlsSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                                       PhysicsSystem& physicsSystem, GameEventQueue& eventQueue,
                                       const UpgradeCatalog& catalog)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_physicsSystem(physicsSystem)
        , m_eventQueue(eventQueue)
        , m_catalog(catalog)
{}

static inline void
cpBodyApplyTorque(cpBody *body, cpFloat torque)
{
    cpVect c = cpBodyGetCenterOfGravity(body);
    cpBodyApplyImpulseAtLocalPoint(body, cpv(0.0, torque), cpv(1.0 + c.x, c.y));
    cpBodyApplyImpulseAtLocalPoint(body, cpv(0.0, -torque), cpv(-1.0 + c.x, c.y));
}

std::pair<Vector2d, Vector2d> ShipControlsSystem::ComputeBulletSpawn(const Transform& transf,
                                                                    const PhysicsBody& phys,
                                                                    double muzzleSpeed)
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
                muzzleSpeed * std::cos(double(transf.rot - Radd(1.5708))),
                muzzleSpeed * std::sin(double(transf.rot - Radd(1.5708)))
        );

        vel += transf.vel;

        return std::make_pair(pos, vel);
    }
    else {
        return std::make_pair(Vector2d{}, Vector2d{});
    }
}

void ShipControlsSystem::ApplyMovement(cpBody* body, const ControlFlags& flags)
{
    cpFloat ang = cpBodyGetAngularVelocity(body);
    const cpFloat maxAng = 15.0;

    cpBodyApplyTorque(body, -ang * 4);

    if (flags.rotateLeft && ang < maxAng) {
        cpBodyApplyTorque(body, 20.0);
    }
    if (flags.rotateRight && ang > -maxAng) {
        cpBodyApplyTorque(body, -20.0);
    }
    if (flags.thrustForward) {
        cpBodyApplyForceAtLocalPoint(body, cpv(0, -ShipControlsSystem::THRUST_FORCE), cpv(0, 0));
    }
}

void ShipControlsSystem::Update(std::uint64_t step)
{
    m_registry.each([&](flecs::entity entity, Transform& transf, PhysicsRef& ref, Controls& scontrols) {
        PhysicsBody& phys = m_physicsSystem.GetBody(ref);
        cpBody* body = phys.cp.body.get();

        ApplyMovement(body, scontrols.actionFlags);

        ShipLoadout* loadout = entity.try_get_mut<ShipLoadout>();
        const ShipStats stats =
                m_catalog.ResolveStats(loadout ? loadout->levels : UpgradeLevels{});

        if (scontrols.fireCooldown > 0) {
            --scontrols.fireCooldown;
        }
        // firePrimary is held, not one-shot; the cooldown paces the fire rate
        // so holding the button auto-fires at a fixed cadence.
        if (scontrols.actionFlags.firePrimary && scontrols.fireCooldown == 0 && stats.gun) {
            const WeaponDef& gun = *stats.gun;
            scontrols.fireCooldown = stats.fireCooldownTicks;
            std::pair<Vector2d, Vector2d> ret =
                    ShipControlsSystem::ComputeBulletSpawn(transf, phys, gun.speed);

            const Team* shooterTeam = entity.try_get<Team>();
            const NetId* shooterNetId = entity.try_get<NetId>();
            flecs::entity bulletEntity =
                    m_entitySpawner.SpawnBullet(gun.modelId, ret.first, ret.second, /*sensor=*/true);
            bulletEntity.emplace<Bullet>(gun.lifetimeSeconds,
                                         shooterTeam ? shooterTeam->id : TeamId::Blue,
                                         gun.damage,
                                         shooterNetId ? shooterNetId->value : 0u);

            // param carries the weapon's id so a listener that never sees the
            // shooter's loadout -- the audio system on a remote client --
            // still knows which gun to play.
            m_eventQueue.Emit(GameEventType::BulletFired, entity,
                              Magnum::Vector2{static_cast<float>(ret.first.x()),
                                              static_cast<float>(ret.first.y())},
                              gun.id);
        }
        // Missiles: same held-button pacing as the primary, but each shot
        // spends a round off the rack the Lab's upgrade filled (ResearchSystem).
        // ownerNetId stays 0 deliberately, unlike a bullet's: a peer's own
        // bullets are omitted from its snapshots because it predicts them
        // locally, and missiles are not predicted -- suppressing them would
        // leave the shooter the only player who never sees their own missile.
        if (scontrols.missileCooldown > 0) {
            --scontrols.missileCooldown;
        }
        if (scontrols.actionFlags.fireMissile && scontrols.missileCooldown == 0 && loadout
            && loadout->missileAmmo > 0 && stats.missile) {
            const WeaponDef& round = *stats.missile;
            scontrols.missileCooldown = stats.missileCooldownTicks;
            --loadout->missileAmmo;

            const Vector2d muzzlePos =
                    ShipControlsSystem::ComputeBulletSpawn(transf, phys, round.speed).first;
            const double heading = static_cast<double>(transf.rot) - HALF_PI; // nose is local -Y
            const Vector2d vel =
                    Vector2d{std::cos(heading), std::sin(heading)} * round.speed + transf.vel;

            const Team* shooterTeam = entity.try_get<Team>();
            flecs::entity missile =
                    m_entitySpawner.SpawnBullet(round.modelId, muzzlePos, vel, /*sensor=*/true,
                                                static_cast<double>(transf.rot), Vector2d{1., 1.});
            missile.emplace<Bullet>(round.lifetimeSeconds,
                                    shooterTeam ? shooterTeam->id : TeamId::Blue, round.damage,
                                    /*ownerNetId=*/0u);
            // MissileSystem locks a target on its first tick, and steers by
            // this weapon's own guidance envelope.
            missile.emplace<Missile>(Missile{0u, round.id});

            m_eventQueue.Emit(GameEventType::BulletFired, entity,
                              Magnum::Vector2{static_cast<float>(muzzlePos.x()),
                                              static_cast<float>(muzzlePos.y())},
                              round.id);
        }

        if (scontrols.actionFlags.fireSecondary) {
            scontrols.actionFlags.fireSecondary = false;
            std::pair<Vector2d, Vector2d> ret = ShipControlsSystem::ComputeBulletSpawn(
                    transf, phys, stats.gun ? stats.gun->speed : 0.);
            flecs::entity box = m_entitySpawner.SpawnBullet("models/doodads/box"_id, ret.first, transf.vel);
            box.emplace<Damageable>(Damageable{BOX_HP, BOX_HP});

            m_eventQueue.Emit(GameEventType::BulletFired, entity,
                              Magnum::Vector2{static_cast<float>(ret.first.x()),
                                              static_cast<float>(ret.first.y())});
        }
    });
}


} // namespace Gravitaris
