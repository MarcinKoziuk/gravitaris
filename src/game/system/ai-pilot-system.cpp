#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/gnc/nav/trajectory-predictor.hpp>
#include <gravitaris/game/gnc/guidance/behaviors.hpp>
#include <gravitaris/game/gnc/control/flight-controller.hpp>
#include <gravitaris/game/util/splitmix.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/ai-pilot-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr double PI = 3.14159265358979323846;

static constexpr double BULLET_SPEED = 200.0; // matches ship-controls-system's BULLET_MUZZLE_SPEED

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
    // Celestial attractors, for picking each pilot's dominant gravity source.
    struct Source {
        flecs::entity entity;
        Vector2d pos;
        double mass;
    };
    std::vector<Source> sources;
    m_registry.each([&](flecs::entity ent, const Transform& transf, const GravitySource& gs) {
        sources.push_back({ent, transf.pos, gs.mass * gs.multiplier});
    });

    m_registry.each([&](flecs::entity ent, Transform& transf, PhysicsRef& ref,
                        AIPilot& pilot, InputQueue& queue) {
        const AIPersonality& personality = pilot.personality;

        // Deterministic per-(tick, entity) seed for this pilot's jitter/
        // danger-ignore rolls below -- same value every replay of this tick.
        std::uint64_t rng = SplitMix64Seed(step, ent.id());

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

        const AIBehavior previous = pilot.behavior;

        if (pilot.decisionCooldown > 0) {
            --pilot.decisionCooldown;
        }
        else {
            if (personality.reactionJitter > 0.0) {
                const double jitter = (SplitMix64NextUnit(rng) - 0.5) * 2.0
                        * personality.reactionJitter * personality.decisionInterval;
                pilot.decisionCooldown = static_cast<std::uint32_t>(
                        std::max(1.0, static_cast<double>(personality.decisionInterval) + jitter));
            }
            else {
                pilot.decisionCooldown = personality.decisionInterval;
            }

            pilot.guidance.accel = ShipControlsSystem::THRUST_FORCE
                    / cpBodyGetMass(m_physicsSystem.GetBody(ref).cp.body.get());

            // Tactical pick among Intercept/Orbit/Idle; the danger check below
            // (which runs every tick, not just on this slower cadence) can
            // still override this with Evade regardless of what's picked here.
            if (targetTransf && (targetTransf->pos - transf.pos).length() < personality.engageRange) {
                pilot.behavior = AIBehavior::Intercept;
            }
            else if (well) {
                pilot.behavior = AIBehavior::Orbit;
                if (previous != AIBehavior::Orbit) {
                    const Vector2d r = transf.pos - well->pos;
                    pilot.patrolRadius = std::max(r.length(), personality.evadeRadius * 2.0);
                    const double cross = r.x() * transf.vel.y() - r.y() * transf.vel.x();
                    pilot.patrolDirection = (cross < 0.0) ? -1.0 : 1.0;
                }
            }
            else {
                pilot.behavior = AIBehavior::Idle;
            }
        }

        // Danger check: every tick, not gated behind decisionCooldown. A
        // pursuit/orbit path is actively thrust-driven and can curve toward a
        // well between decision points; TrajectoryPredictor only coasts
        // (gravity, no thrust -- see its class comment), so checking every
        // tick means the moment the ship's actual velocity starts curving
        // into danger, it's caught within a tick instead of up to
        // decisionInterval ticks late.
        bool predictedDanger = false;
        if (well) {
            const std::vector<Vector2d> path =
                    m_predictor.Predict(ent, personality.dangerLookaheadSteps, Game::PHYSICS_DELTA);
            for (const Vector2d& p : path) {
                if ((p - well->pos).length() < personality.evadeRadius) {
                    predictedDanger = true;
                    break;
                }
            }
        }

        // Roll once per fresh danger episode (not every tick it persists) so
        // a Reckless ship that shrugs off a warning actually commits to the
        // risky path rather than re-rolling itself into evading a tick later.
        if (predictedDanger && !pilot.wasInDanger) {
            pilot.dangerSuppressed = personality.dangerIgnoreChance > 0.0
                    && SplitMix64NextUnit(rng) < personality.dangerIgnoreChance;
        }
        if (!predictedDanger) {
            pilot.dangerSuppressed = false;
        }
        pilot.wasInDanger = predictedDanger;

        const bool effectiveDanger = predictedDanger && !pilot.dangerSuppressed;

        if (effectiveDanger) {
            pilot.behavior = AIBehavior::Evade;
        }
        else if (pilot.behavior == AIBehavior::Evade) {
            // Hysteresis: don't hand control back the instant the prediction
            // clears -- wait until genuinely clear of the well, or this would
            // flap Evade/Intercept right at the trigger boundary.
            const bool clear = !well
                    || (transf.pos - well->pos).length() > personality.evadeRadius * personality.evadeMargin;
            if (clear) {
                pilot.decisionCooldown = 0; // re-pick a tactical behavior next tick
            }
        }

        Vector2d desiredVel = transf.vel; // Idle: no correction
        switch (pilot.behavior) {
            case AIBehavior::Evade:
                if (well) {
                    desiredVel = EvadeBody(transf, well->pos,
                                          personality.evadeRadius * personality.evadeMargin, pilot.guidance);
                }
                break;
            case AIBehavior::Intercept:
                if (targetTransf) {
                    GuidanceParams standoff = pilot.guidance;
                    standoff.arriveRadius = personality.standoffDistance;
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
            if (relPos.length() < personality.fireRange) {
                if (std::optional<double> t = SolveInterceptTime(relPos, relVel, BULLET_SPEED)) {
                    const Vector2d aim = relPos + relVel * (*t);
                    const double aimHeading = std::atan2(aim.y(), aim.x());
                    const double heading = static_cast<double>(transf.rot) - PI / 2.0;

                    // Rolled once per firing opportunity (not every tick) and
                    // held steady while waiting for an aligned shot -- a
                    // sloppy shot is then a real, fixed aiming error rather
                    // than the fire threshold flickering randomly tick to
                    // tick (which would look like the gun spraying).
                    if (personality.aimJitter > 0.0 && !pilot.aimBiasRolled) {
                        pilot.aimBias = (SplitMix64NextUnit(rng) - 0.5) * 2.0 * personality.aimJitter;
                        pilot.aimBiasRolled = true;
                    }
                    const double tolerance = personality.fireTolerance
                            + (personality.aimJitter > 0.0 ? pilot.aimBias : 0.0);

                    if (std::abs(WrapToPi(aimHeading - heading)) < tolerance) {
                        flags.firePrimary = true;
                        pilot.aimBiasRolled = false; // roll fresh for the next shot

                        if (personality.burstCount > 1) {
                            if (pilot.burstShotsRemaining == 0) {
                                pilot.burstShotsRemaining = personality.burstCount;
                            }
                            --pilot.burstShotsRemaining;
                            pilot.fireCooldown = pilot.burstShotsRemaining > 0
                                    ? personality.burstShotInterval
                                    : personality.fireInterval;
                        }
                        else {
                            pilot.fireCooldown = personality.fireInterval;
                        }
                    }
                }
            }
        }
        else {
            pilot.aimBiasRolled = false; // no live shot attempt -- clear for next time
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
