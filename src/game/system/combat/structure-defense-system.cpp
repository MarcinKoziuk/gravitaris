#include <cmath>
#include <limits>
#include <optional>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/intercept.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/system/combat/structure-defense-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

StructureDefenseSystem::StructureDefenseSystem(flecs::world& registry, PhysicsSystem& physicsSystem,
                                               EntitySpawner& entitySpawner, GameEventQueue& eventQueue,
                                               const UpgradeCatalog& catalog)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
        , m_catalog(catalog)
{}

void StructureDefenseSystem::Update()
{
    const UpgradeCatalog::Fittings& fittings = m_catalog.Fitted();
    const WeaponDef* round = m_catalog.FindWeapon(fittings.turretWeapon);
    if (!round) return; // no [turret] weapon in the catalog: nothing to fire

    struct Target {
        Vector2d pos;
        Vector2d vel;
        TeamId team;
    };
    std::vector<Target> targets;
    m_registry.each([&](const Transform& t, const Team& team, const Damageable&) {
        if (team.id != TeamId::None) targets.push_back({t.pos, t.vel, team.id});
    });

    m_registry.each([&](flecs::entity turret, const Transform& transf, const Team& turretTeam,
                        const PhysicsRef& ref, StructureDefense& defense) {
        if (defense.fireCooldown > 0) {
            --defense.fireCooldown;
            return;
        }

        // The closest thing inside the envelope, rather than whichever hostile
        // the registry happened to walk first: an emplacement firing steadily
        // has to keep working the same target to be worth anything, and the
        // list's order changes for reasons that have nothing to do with the
        // fight.
        const Target* chosen = nullptr;
        double bestDistSq = fittings.turretFireRange * fittings.turretFireRange;
        for (const Target& target : targets) {
            if (target.team == turretTeam.id) continue;
            const double distSq = (target.pos - transf.pos).dot();
            if (distSq > bestDistSq) continue;
            bestDistSq = distSq;
            chosen = &target;
        }
        if (!chosen) return;

        const Vector2d relPos = chosen->pos - transf.pos;
        const Vector2d relVel = chosen->vel - transf.vel;

        // An emplacement sits at the bottom of the well it is defending -- a
        // Base is at its planet's core -- so its rounds climb the whole way
        // out and drop harder than anything else that shoots.
        const id_t spaceId = m_physicsSystem.GetBody(ref).spaceId;
        const Vector2d drop = m_physicsSystem.MeanGravityAccel(spaceId, transf.pos, chosen->pos);

        const std::optional<BallisticSolution> solution =
                SolveBallisticAim(relPos, relVel, round->speed, drop);
        if (!solution) return;

        const Vector2d aim = solution->direction;
        const Vector2d vel = aim * round->speed + transf.vel;

        // Along the firing solution, not along the turret's own facing:
        // a round drawn as a streak has to lie on its line of flight.
        const double rot = std::atan2(aim.y(), aim.x()) + PI / 2.0;

        const flecs::entity bullet =
                m_entitySpawner.SpawnBullet(round->modelId, transf.pos, vel, /*sensor=*/true, rot);
        bullet.emplace<Bullet>(round->lifetimeSeconds, turretTeam.id, round->damage,
                               /*ownerNetId=*/0u, turret.id());

        m_eventQueue.Emit(GameEventType::BulletFired, turret,
                          Magnum::Vector2{static_cast<float>(transf.pos.x()), static_cast<float>(transf.pos.y())},
                          round->id);

        defense.fireCooldown = fittings.turretFireCooldownTicks;
    });
}

} // namespace Gravitaris
