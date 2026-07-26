#include <algorithm>
#include <cmath>

#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/gnc/guidance/behaviors.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

Vector2d GotoPoint(const Transform& ship, const Vector2d& target, const GuidanceParams& params)
{
    const Vector2d toTarget = target - ship.pos;
    const double dist = toTarget.length();
    if (dist < params.arriveRadius) {
        return {0.0, 0.0};
    }

    // dist = v*flipTime + v^2/(2a), solved for v: the fastest approach speed
    // that can still stop at the target after turning around.
    const double a = params.accel;
    const double t = params.flipTime;
    const double vArrive = a * (-t + std::sqrt(t * t + 2.0 * dist / a));

    const double speed = std::min(params.transitSpeed, vArrive);
    return toTarget * (speed / dist);
}

Vector2d OrbitBody(const Transform& ship, const Vector2d& center, double centerMass,
                   double radius, double direction, const GuidanceParams& params)
{
    const Vector2d r = ship.pos - center;
    const double dist = r.length();
    if (dist < 1e-6) {
        return {0.0, 0.0};
    }

    const Vector2d radialDir = r / dist;
    const Vector2d tangentDir = Vector2d{-radialDir.y(), radialDir.x()} * direction;

    // v = sqrt(G*M/r) at the *current* radius, so gravity is exactly
    // balanced while the radial term walks the orbit toward `radius`.
    const double vCircular = std::sqrt(PhysicsSystem::GRAVITY_CONSTANT * centerMass / dist);

    const double radialErr = radius - dist;
    const double vRadial = std::clamp(radialErr * params.orbitRadialKp,
                                      -params.maxRadialSpeed, params.maxRadialSpeed);

    return tangentDir * vCircular + radialDir * vRadial;
}

Vector2d InterceptEntity(const Transform& ship, const Transform& target, const GuidanceParams& params)
{
    const Vector2d toTarget = target.pos - ship.pos;
    const double dist = toTarget.length();
    const double eta = dist / std::max(params.maxSpeed, 1e-6);
    const Vector2d aim = target.pos + target.vel * eta;

    // Only the target's *lateral* motion is matched. Matching its
    // line-of-sight motion too would mean a chased ship mirrors its pursuer's
    // charge and runs at the pursuer's speed (and a fleeing target would drag
    // its interceptor past the cruise cap); the closing speed along the line
    // of sight is GotoPoint's job alone.
    Vector2d matchVel;
    if (dist > 1e-6) {
        const Vector2d los = toTarget / dist;
        matchVel = target.vel - los * Magnum::Math::dot(target.vel, los);
    }

    Vector2d desired = GotoPoint(ship, aim, params) + matchVel;
    const double speed = desired.length();
    if (speed > params.maxSpeed) {
        desired *= params.maxSpeed / speed;
    }
    return desired;
}

Vector2d LandOnBody(const Transform& ship, const Vector2d& center, const Vector2d& centerVel,
                    double effectiveMass, double surfaceRadius, const GuidanceParams& params)
{
    const Vector2d r = ship.pos - center;
    const double dist = r.length();
    if (dist < 1e-6) {
        return centerVel;
    }

    const Vector2d up = r / dist;
    const double altitude = dist - surfaceRadius;
    if (altitude < params.flareAltitude) {
        return centerVel;
    }

    // Only the thrust left over from holding the ship up can bleed off
    // descent speed. The floor covers thrust <= local gravity, where no
    // approach speed is actually stoppable.
    const double gravity = PhysicsSystem::GRAVITY_CONSTANT * effectiveMass / (dist * dist);
    const double brake = std::max(params.accel - gravity, 1.0);

    // altitude = v*flipTime + (v^2 - touchdown^2)/(2*brake), solved for v.
    const double t = params.flipTime;
    const double vDescent = -brake * t
            + std::sqrt(brake * brake * t * t + 2.0 * brake * (altitude - params.flareAltitude)
                        + params.touchdownSpeed * params.touchdownSpeed);

    return centerVel - up * std::clamp(vDescent, params.touchdownSpeed, params.transitSpeed);
}

Vector2d EvadeBody(const Transform& ship, const Vector2d& center, const Vector2d& centerVel,
                   double safeRadius, const GuidanceParams& params)
{
    const Vector2d r = ship.pos - center;
    const double dist = r.length();
    if (dist >= safeRadius || dist < 1e-6) {
        return ship.vel;
    }

    const Vector2d radialDir = r / dist;
    const Vector2d relVel = ship.vel - centerVel;
    const Vector2d tangentialVel = relVel - radialDir * Magnum::Math::dot(relVel, radialDir);

    Vector2d desired = tangentialVel + radialDir * params.maxSpeed;
    const double speed = desired.length();
    if (speed > params.maxSpeed) {
        desired *= params.maxSpeed / speed;
    }
    return centerVel + desired;
}

} // namespace Gravitaris
