#include <cmath>
#include <limits>
#include <optional>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/system/combat/structure-defense-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

    // Claude: we have so many duplicates, please create a math utils: include/gravitaris/game/math-utils.hpp (move PI there too)
static double WrapToPi(double angle)
{
    angle = std::fmod(angle + PI, 2.0 * PI);
    if (angle < 0.0) angle += 2.0 * PI;
    return angle - PI;
}

    // Claude: how can we best share code? I think small generic math units can go into math-fns
    // maybe intercept there too (but in a related .cpp unit)
// Smallest positive time at which a projectile of `projectileSpeed`
// (relative to the shooter) meets a target at relPos moving at relVel. Same
// formula as AIPilotSystem's own (ai-pilot-system.cpp) -- not shared since
// that one is file-local there too.
static std::optional<double> SolveInterceptTime(const Vector2d& relPos, const Vector2d& relVel,
                                                double projectileSpeed)
{
    const double a = relVel.dot() - projectileSpeed * projectileSpeed;
    const double b = 2.0 * Magnum::Math::dot(relPos, relVel);
    const double c = relPos.dot();

    if (std::abs(a) < 1e-9) {
        if (std::abs(b) < 1e-9) return std::nullopt;
        const double t = -c / b;
        return t > 0.0 ? std::optional<double>(t) : std::nullopt;
    }

    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return std::nullopt;

    const double sq = std::sqrt(disc);
    const double t1 = (-b - sq) / (2.0 * a);
    const double t2 = (-b + sq) / (2.0 * a);

    double t = std::numeric_limits<double>::max();
    if (t1 > 0.0) t = std::min(t, t1);
    if (t2 > 0.0) t = std::min(t, t2);
    return t != std::numeric_limits<double>::max() ? std::optional<double>(t) : std::nullopt;
}

StructureDefenseSystem::StructureDefenseSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                                               GameEventQueue& eventQueue, const UpgradeCatalog& catalog)
        : m_registry(registry)
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

    m_registry.each([&](flecs::entity turret, const Transform& transf, const Team& turretTeam, StructureDefense& defense) {
        if (defense.fireCooldown > 0) {
            --defense.fireCooldown;
            return;
        }

        for (const Target& target : targets) {
            if (target.team == turretTeam.id) continue;

            const Vector2d relPos = target.pos - transf.pos;
            if (relPos.length() > fittings.turretFireRange) continue;

            const Vector2d relVel = target.vel - transf.vel;
            const std::optional<double> t = SolveInterceptTime(relPos, relVel, round->speed);
            if (!t) continue;

            const Vector2d aim = (relPos + relVel * (*t)).normalized();
            const Vector2d vel = aim * round->speed + transf.vel;

            const flecs::entity bullet =
                    m_entitySpawner.SpawnBullet(round->modelId, transf.pos, vel, /*sensor=*/true);
            bullet.emplace<Bullet>(round->lifetimeSeconds, turretTeam.id, round->damage);

            m_eventQueue.Emit(GameEventType::BulletFired, turret,
                              Magnum::Vector2{static_cast<float>(transf.pos.x()), static_cast<float>(transf.pos.y())},
                              round->id);

            defense.fireCooldown = fittings.turretFireCooldownTicks;
            break; // one shot per tick per turret
        }
    });
}

} // namespace Gravitaris
