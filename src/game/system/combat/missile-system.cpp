#include <cmath>
#include <vector>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/missile.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/combat/missile-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr double HALF_PI = 1.5707963267948966;

MissileSystem::MissileSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                             PhysicsSystem& physicsSystem)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_physicsSystem(physicsSystem)
{}

void MissileSystem::Update()
{
    // Candidates: anything damageable on another team. Ships and structures
    // both qualify; planets carry a Team but no Damageable, so they can't be
    // locked (nor could a missile hurt one).
    struct Candidate {
        Vector2d pos;
        TeamId team;
        std::uint32_t netId;
    };
    std::vector<Candidate> candidates;
    m_registry.each([&](const Transform& transf, const Team& team, const Damageable&, const NetId& netId) {
        candidates.push_back(Candidate{transf.pos, team.id, netId.value});
    });

    m_registry.each([&](Missile& missile, const Bullet& bullet, const Transform& transf, PhysicsRef& ref) {
        Vector2d targetPos;
        bool haveTarget = false;

        if (missile.targetNetId != 0) {
            const flecs::entity locked = m_entitySpawner.EntityForNetId(missile.targetNetId);
            const Team* lockedTeam = locked.is_alive() ? locked.try_get<Team>() : nullptr;
            if (lockedTeam && lockedTeam->id != bullet.team) {
                targetPos = locked.get<Transform>().pos;
                haveTarget = true;
            }
            else {
                missile.targetNetId = 0; // dead, or no longer hostile
            }
        }

        if (!haveTarget) {
            double nearestDistSq = 0.0;
            for (const Candidate& c : candidates) {
                if (c.team == bullet.team) continue;
                const double distSq = (c.pos - transf.pos).dot();
                if (!haveTarget || distSq < nearestDistSq) {
                    nearestDistSq = distSq;
                    targetPos = c.pos;
                    missile.targetNetId = c.netId;
                    haveTarget = true;
                }
            }
        }

        if (!haveTarget) return; // nothing hostile left -- coast on

        const Vector2d toTarget = targetPos - transf.pos;
        if (toTarget.length() < 1e-6) return;
        const Vector2d desired = toTarget.normalized();

        const double speed = transf.vel.length();
        Vector2d heading = speed > 1e-6 ? transf.vel / speed : desired;

        // Turn toward the target, capped per tick; the sign of the 2D cross
        // product picks the shorter way round.
        const double cosAngle = Magnum::Math::clamp(Magnum::Math::dot(heading, desired), -1.0, 1.0);
        const double angle = std::acos(cosAngle);
        const double maxStep = TURN_RATE * Game::PHYSICS_DELTA;
        if (angle > maxStep) {
            const double cross = heading.x() * desired.y() - heading.y() * desired.x();
            const double step = std::copysign(maxStep, cross);
            const double c = std::cos(step);
            const double s = std::sin(step);
            heading = Vector2d{heading.x() * c - heading.y() * s, heading.x() * s + heading.y() * c};
        }
        else {
            heading = desired;
        }

        const double newSpeed = std::min(speed + ACCELERATION * Game::PHYSICS_DELTA, TOP_SPEED);
        const Vector2d vel = heading * newSpeed;

        cpBody* body = m_physicsSystem.GetBody(ref).cp.body.get();
        cpBodySetVelocity(body, cpv(vel.x(), vel.y()));
        // Nose along the flight path (the drawing's nose is local -Y).
        cpBodySetAngle(body, std::atan2(heading.y(), heading.x()) + HALF_PI);
        cpBodySetAngularVelocity(body, 0.0);
    });
}

} // namespace Gravitaris
