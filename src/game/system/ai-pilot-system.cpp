#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/gnc/nav/trajectory-predictor.hpp>
#include <gravitaris/game/gnc/guidance/behaviors.hpp>
#include <gravitaris/game/gnc/control/flight-controller.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/ai-pilot-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr double PI = 3.14159265358979323846;

static constexpr double ENGAGE_RANGE = 500.0;   // pursue the target inside this
static constexpr double STANDOFF_DISTANCE = 50.0;
static constexpr double FIRE_RANGE = 250.0;
static constexpr double FIRE_TOLERANCE = 0.12;  // rad off the lead solution
static constexpr double EVADE_RADIUS = 90.0;    // distance to a well considered dangerous
static constexpr double BULLET_SPEED = 200.0;   // matches ship-controls-system muzzle speed
static constexpr std::uint32_t DECISION_INTERVAL = 15;
static constexpr std::uint32_t FIRE_INTERVAL = 30;
static constexpr int DANGER_LOOKAHEAD_STEPS = 120; // 2 s at the fixed tick

static double WrapToPi(double angle);
static std::optional<double> SolveInterceptTime(const Vector2d& relPos, const Vector2d& relVel,
                                                double projectileSpeed);

AIPilotSystem::AIPilotSystem(flecs::world& registry, PhysicsSystem& physicsSystem,
                             TrajectoryPredictor& predictor)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_predictor(predictor)
{}

void AIPilotSystem::Update(std::uint64_t step, std::optional<flecs::entity> player)
{
    // Non-bullet bodies, for picking each pilot's dominant gravity source.
    struct Source {
        flecs::entity entity;
        Vector2d pos;
        double mass;
    };
    std::vector<Source> sources;
    m_registry.each([&](flecs::entity ent, Transform& transf, PhysicsRef& ref) {
        if (ent.has<Bullet>()) return;
        const double mass = cpBodyGetMass(m_physicsSystem.GetBody(ref).cp.body.get());
        sources.push_back({ent, transf.pos, mass});
    });

    m_registry.each([&](flecs::entity ent, Transform& transf, PhysicsRef& ref,
                        AIPilot& pilot, InputQueue& queue) {
        const Source* well = nullptr;
        for (const Source& src : sources) {
            if (src.entity == ent) continue;
            if (!well || src.mass > well->mass) well = &src;
        }

        if (!pilot.target.is_alive() && player) {
            pilot.target = *player;
        }
        const Transform* targetTransf =
                pilot.target.is_alive() ? pilot.target.try_get<Transform>() : nullptr;

        if (pilot.decisionCooldown > 0) {
            --pilot.decisionCooldown;
        }
        else {
            pilot.decisionCooldown = DECISION_INTERVAL;

            pilot.guidance.accel = ShipControlsSystem::THRUST_FORCE
                    / cpBodyGetMass(m_physicsSystem.GetBody(ref).cp.body.get());

            bool danger = false;
            if (well) {
                const std::vector<Vector2d> path =
                        m_predictor.Predict(ent, DANGER_LOOKAHEAD_STEPS, Game::PHYSICS_DELTA);
                for (const Vector2d& p : path) {
                    if ((p - well->pos).length() < EVADE_RADIUS) {
                        danger = true;
                        break;
                    }
                }
            }

            const AIBehavior previous = pilot.behavior;
            if (danger) {
                pilot.behavior = AIBehavior::Evade;
            }
            else if (targetTransf && (targetTransf->pos - transf.pos).length() < ENGAGE_RANGE) {
                pilot.behavior = AIBehavior::Intercept;
            }
            else if (well) {
                pilot.behavior = AIBehavior::Orbit;
                if (previous != AIBehavior::Orbit) {
                    const Vector2d r = transf.pos - well->pos;
                    pilot.patrolRadius = std::max(r.length(), EVADE_RADIUS * 2.0);
                    const double cross = r.x() * transf.vel.y() - r.y() * transf.vel.x();
                    pilot.patrolDirection = (cross < 0.0) ? -1.0 : 1.0;
                }
            }
            else {
                pilot.behavior = AIBehavior::Idle;
            }
        }

        Vector2d desiredVel = transf.vel; // Idle: no correction
        switch (pilot.behavior) {
            case AIBehavior::Evade:
                if (well) {
                    desiredVel = EvadeBody(transf, well->pos, EVADE_RADIUS * 1.5, pilot.guidance);
                }
                break;
            case AIBehavior::Intercept:
                if (targetTransf) {
                    GuidanceParams standoff = pilot.guidance;
                    standoff.arriveRadius = STANDOFF_DISTANCE;
                    desiredVel = InterceptEntity(transf, *targetTransf, standoff);
                }
                break;
            case AIBehavior::Orbit:
                if (well) {
                    desiredVel = OrbitBody(transf, well->pos, well->mass,
                                           pilot.patrolRadius, pilot.patrolDirection, pilot.guidance);
                }
                break;
            case AIBehavior::Idle:
                break;
        }

        ControlFlags flags = FlyToVelocity(transf, desiredVel, pilot.flight);

        if (pilot.fireCooldown > 0) {
            --pilot.fireCooldown;
        }
        else if (pilot.behavior == AIBehavior::Intercept && targetTransf) {
            const Vector2d relPos = targetTransf->pos - transf.pos;
            const Vector2d relVel = targetTransf->vel - transf.vel;
            if (relPos.length() < FIRE_RANGE) {
                if (std::optional<double> t = SolveInterceptTime(relPos, relVel, BULLET_SPEED)) {
                    const Vector2d aim = relPos + relVel * (*t);
                    const double aimHeading = std::atan2(aim.y(), aim.x());
                    const double heading = static_cast<double>(transf.rot) - PI / 2.0;
                    if (std::abs(WrapToPi(aimHeading - heading)) < FIRE_TOLERANCE) {
                        flags.firePrimary = true;
                        pilot.fireCooldown = FIRE_INTERVAL;
                    }
                }
            }
        }

        queue.Push(InputCommand{step, flags});
    });
}

static double WrapToPi(double angle)
{
    angle = std::fmod(angle + PI, 2.0 * PI);
    if (angle < 0.0) angle += 2.0 * PI;
    return angle - PI;
}

// Smallest positive time at which a projectile of `projectileSpeed` (relative
// to the shooter) meets a target at relPos moving at relVel.
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

} // namespace Gravitaris
