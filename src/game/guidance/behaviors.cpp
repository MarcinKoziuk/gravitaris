#include <algorithm>
#include <cmath>

#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/guidance/behaviors.hpp>

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

    const double speed = std::min(params.maxSpeed, vArrive);
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
    const double dist = (target.pos - ship.pos).length();
    const double eta = dist / std::max(params.maxSpeed, 1e-6);
    const Vector2d aim = target.pos + target.vel * eta;

    return GotoPoint(ship, aim, params) + target.vel;
}

Vector2d EvadeBody(const Transform& ship, const Vector2d& center, double safeRadius,
                   const GuidanceParams& params)
{
    const Vector2d r = ship.pos - center;
    const double dist = r.length();
    if (dist >= safeRadius || dist < 1e-6) {
        return ship.vel;
    }

    const Vector2d radialDir = r / dist;
    const Vector2d tangentialVel = ship.vel - radialDir * Magnum::Math::dot(ship.vel, radialDir);

    Vector2d desired = tangentialVel + radialDir * params.maxSpeed;
    const double speed = desired.length();
    if (speed > params.maxSpeed) {
        desired *= params.maxSpeed / speed;
    }
    return desired;
}

} // namespace Gravitaris
