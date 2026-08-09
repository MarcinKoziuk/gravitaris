#include <cmath>
#include <vector>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/math-utils.hpp>
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
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/system/combat/missile-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

// How long one hunt of a wobbling seeker takes. Slow enough to read as a weave
// on the way in rather than as a vibration.
static constexpr double WOBBLE_PERIOD_TICKS = 33.0;

MissileSystem::MissileSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                             PhysicsSystem& physicsSystem, const UpgradeCatalog& catalog)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_physicsSystem(physicsSystem)
        , m_catalog(catalog)
{}

void MissileSystem::Update()
{
    // Candidates: anything damageable on another team. Ships and structures
    // both qualify; planets carry a Team but no Damageable, so they can't be
    // locked (nor could a missile hurt one).
    struct Candidate {
        Vector2d pos;
        Vector2d vel;
        TeamId team;
        std::uint32_t netId;
    };
    std::vector<Candidate> candidates;
    m_registry.each([&](const Transform& transf, const Team& team, const Damageable&, const NetId& netId) {
        candidates.push_back(Candidate{transf.pos, transf.vel, team.id, netId.value});
    });

    m_registry.each([&](flecs::entity round, Missile& missile, const Bullet& bullet,
                        const Transform& transf, PhysicsRef& ref) {
        const WeaponDef* weapon = m_catalog.FindWeapon(missile.weaponId);
        if (!weapon || !weapon->IsGuided()) return; // an unguided round just coasts
        const WeaponDef::Guidance& guidance = weapon->guidance;

        ++missile.age;

        Vector2d targetPos;
        Vector2d targetVel;
        bool haveTarget = false;

        if (missile.targetNetId != 0) {
            const flecs::entity locked = m_entitySpawner.EntityForNetId(missile.targetNetId);
            const Team* lockedTeam = locked.is_alive() ? locked.try_get<Team>() : nullptr;
            if (lockedTeam && lockedTeam->id != bullet.team) {
                const Transform& lockedTransf = locked.get<Transform>();
                targetPos = lockedTransf.pos;
                targetVel = lockedTransf.vel;
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
                    targetVel = c.vel;
                    missile.targetNetId = c.netId;
                    haveTarget = true;
                }
            }
        }

        if (!haveTarget) return; // nothing hostile left -- coast on

        const Vector2d toTarget = targetPos - transf.pos;
        if (toTarget.length() < 1e-6) return;

        const double speed = transf.vel.length();

        // Where the target will be when the round gets there, rather than
        // where it is now. Pure pursuit is what made a missile weave: chasing
        // the present position of something crossing in front of it commands a
        // turn that reverses every time it crosses the line of sight, and the
        // turn-rate cap turns that into an S. Solved by fixed-point iteration
        // on the flight time -- three passes is well inside a tick's worth of
        // travel at any speed a round flies.
        const double travelSpeed = std::max(speed, guidance.topSpeed * 0.5);
        Vector2d lead = toTarget;
        if (travelSpeed > 1e-6) {
            double eta = toTarget.length() / travelSpeed;
            for (int i = 0; i < 3; ++i) {
                lead = toTarget + targetVel * eta;
                eta = lead.length() / travelSpeed;
            }
        }
        if (lead.length() < 1e-6) return;
        Vector2d desired = lead.normalized();

        // What is left of the old weave, now a property of the seeker rather
        // than of the geometry: the cheap round hunts around its solution and
        // the top tier flies it dead straight. Phased off the entity id so two
        // rounds off the same rack don't weave in lockstep.
        if (guidance.wobble > 0.0) {
            const double phase = static_cast<double>(round.id() % 64u) / 64.0 * TWO_PI;
            const double offset = guidance.wobble
                    * std::sin(static_cast<double>(missile.age) * TWO_PI / WOBBLE_PERIOD_TICKS + phase);
            const double c = std::cos(offset);
            const double s = std::sin(offset);
            desired = Vector2d{desired.x() * c - desired.y() * s, desired.x() * s + desired.y() * c};
        }

        Vector2d heading = speed > 1e-6 ? transf.vel / speed : desired;

        // Turn toward the target, capped per tick; the sign of the 2D cross
        // product picks the shorter way round.
        const double cosAngle = Magnum::Math::clamp(Magnum::Math::dot(heading, desired), -1.0, 1.0);
        const double angle = std::acos(cosAngle);
        const double maxStep = guidance.turnRate * Game::PHYSICS_DELTA;
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

        // top_speed is what the motor can reach on its own, not a speed limit
        // the airframe is held to: a missile launched from a fast ship keeps
        // the velocity it inherited (nothing in space bleeds it off), and only
        // accelerates while it is still under its own top speed. Clamping
        // outright made a launch visibly brake to top_speed on its first tick,
        // which read as the missile not inheriting ship velocity at all.
        const double newSpeed = std::max(
                speed, std::min(speed + guidance.acceleration * Game::PHYSICS_DELTA, guidance.topSpeed));
        const Vector2d vel = heading * newSpeed;

        cpBody* body = m_physicsSystem.GetBody(ref).cp.body.get();
        cpBodySetVelocity(body, cpv(vel.x(), vel.y()));
        // Nose along the flight path (the drawing's nose is local -Y).
        cpBodySetAngle(body, std::atan2(heading.y(), heading.x()) + HALF_PI);
        cpBodySetAngularVelocity(body, 0.0);
    });
}

} // namespace Gravitaris
