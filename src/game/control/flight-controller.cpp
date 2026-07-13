#include <cmath>

#include <gravitaris/game/control/flight-controller.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

namespace {

constexpr double PI = 3.14159265358979323846;

double WrapToPi(double angle)
{
    angle = std::fmod(angle + PI, 2.0 * PI);
    if (angle < 0.0) angle += 2.0 * PI;
    return angle - PI;
}

} // namespace

ControlFlags FlyToVelocity(const Transform& ship, const Vector2d& desiredVel,
                           const FlightControllerParams& params)
{
    ControlFlags flags{};

    const Vector2d velError = desiredVel - ship.vel;
    const double errorMagnitude = velError.length();

    if (errorMagnitude < params.velocityDeadband) {
        // Close enough; also stop any residual spin (the actuator's own
        // angular damping does the work once we stop commanding turns).
        return flags;
    }

    // Ship forward is local -Y (ShipControlsSystem thrust/bullet spawn), so
    // its world heading is rot - pi/2. Positive turn command = CCW =
    // rotateLeft (+torque increases the Chipmunk angle).
    const double targetHeading = std::atan2(velError.y(), velError.x());
    const double currentHeading = static_cast<double>(ship.rot) - PI / 2.0;
    const double headingError = WrapToPi(targetHeading - currentHeading);

    const double turn = params.headingKp * headingError - params.headingKd * ship.angVel;
    if (turn > params.turnDeadband) {
        flags.rotateLeft = true;
    } else if (turn < -params.turnDeadband) {
        flags.rotateRight = true;
    }

    if (std::abs(headingError) < params.aimTolerance) {
        flags.thrustForward = true;
    }

    return flags;
}

Vector2d HoldPositionDesiredVelocity(const Transform& ship, const Vector2d& anchor,
                                     const FlightControllerParams& params)
{
    Vector2d desired = (anchor - ship.pos) * params.positionKp;
    const double speed = desired.length();
    if (speed > params.maxApproachSpeed) {
        desired *= params.maxApproachSpeed / speed;
    }
    return desired;
}

} // namespace Gravitaris
