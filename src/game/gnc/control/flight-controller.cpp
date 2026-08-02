#include <cmath>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/gnc/control/flight-controller.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static double WrapToPi(double angle);
static double HeadingError(const Transform& ship, const Vector2d& bearing);
static ControlFlags SteerTo(const Transform& ship, double headingError,
                            const FlightControllerParams& params);
static void SetThrottle(ThrottleState& throttle, bool thrust,
                        const FlightControllerParams& params);

ControlFlags FlyToVelocity(const Transform& ship, const Vector2d& desiredVel,
                           const FlightControllerParams& params, ThrottleState* throttle,
                           const Vector2d* coastFacing)
{
    ControlFlags flags{};

    const Vector2d velError = desiredVel - ship.vel;

    // Asymmetric deadband: a burn ends at velocityDeadband but only starts
    // again at restartFactor times it, so an error sitting on the threshold
    // can't toggle the thruster every other tick. Needs the throttle's memory
    // of which side it is on, so a caller without one keeps the plain band.
    double deadband = params.velocityDeadband;
    if (throttle && !throttle->thrusting) {
        deadband *= params.restartFactor;
    }
    const bool coasting = velError.length() < deadband;

    // Coasting still steers, onto the velocity it wants rather than onto the
    // (near-zero, so directionless) error: a ship left at whatever attitude
    // its last correction ended on flies its whole cruise sideways. A caller
    // that knows a better resting attitude names it instead.
    const Vector2d steer = coasting && coastFacing ? *coastFacing
                         : coasting                ? desiredVel
                                                   : velError;
    if (steer.length() < 1e-6) {
        if (throttle) SetThrottle(*throttle, false, params);
        return flags;
    }

    const double headingError = HeadingError(ship, steer);
    flags = SteerTo(ship, headingError, params);

    const bool aimed = !coasting && std::abs(headingError) < params.aimTolerance;
    if (!throttle) {
        flags.thrustForward = aimed;
        return flags;
    }

    if (throttle->holdTicks > 0) {
        --throttle->holdTicks;
    }
    const bool urgent = velError.length() > params.velocityDeadband * params.urgentFactor;
    const bool dwelling = throttle->holdTicks > 0 && !urgent;
    // Aim always wins over the dwell -- holding a burn while swinging off
    // course would push the ship somewhere it never asked to go.
    SetThrottle(*throttle, dwelling ? (throttle->thrusting && aimed) : aimed, params);
    flags.thrustForward = throttle->thrusting;

    return flags;
}

ControlFlags TrackBearing(const Transform& ship, const Vector2d& bearing, const Vector2d& velError,
                          const FlightControllerParams& params, ThrottleState* throttle)
{
    ControlFlags flags{};
    if (bearing.length() < 1e-6) {
        if (throttle) SetThrottle(*throttle, false, params);
        return flags;
    }

    flags = SteerTo(ship, HeadingError(ship, bearing), params);

    // The nose is spoken for, so the only burn on offer is one it was already
    // pointed at. Everything else in the correction is given up rather than
    // taken out of the aim.
    bool burn = false;
    if (velError.length() > params.velocityDeadband) {
        burn = std::abs(HeadingError(ship, velError)) < params.aimTolerance;
    }

    if (!throttle) {
        flags.thrustForward = burn;
        return flags;
    }

    if (throttle->holdTicks > 0) {
        --throttle->holdTicks;
    }
    const bool dwelling = throttle->holdTicks > 0;
    SetThrottle(*throttle, dwelling ? (throttle->thrusting && burn) : burn, params);
    flags.thrustForward = throttle->thrusting;

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

// Ship forward is local -Y (see ShipControlsSystem), so world heading is
// rot - pi/2.
static double HeadingError(const Transform& ship, const Vector2d& bearing)
{
    const double targetHeading = std::atan2(bearing.y(), bearing.x());
    const double currentHeading = static_cast<double>(ship.rot) - PI / 2.0;
    return WrapToPi(targetHeading - currentHeading);
}

// Positive turn = CCW = rotateLeft (+torque).
static ControlFlags SteerTo(const Transform& ship, double headingError,
                            const FlightControllerParams& params)
{
    ControlFlags flags{};
    const double turn = params.headingKp * headingError - params.headingKd * ship.angVel;
    if (turn > params.turnDeadband) {
        flags.rotateLeft = true;
    }
    else if (turn < -params.turnDeadband) {
        flags.rotateRight = true;
    }
    return flags;
}

static void SetThrottle(ThrottleState& throttle, bool thrust,
                        const FlightControllerParams& params)
{
    if (throttle.thrusting == thrust) return;
    throttle.thrusting = thrust;
    throttle.holdTicks = thrust ? params.minBurnTicks : params.minCoastTicks;
}

static double WrapToPi(double angle)
{
    angle = std::fmod(angle + PI, 2.0 * PI);
    if (angle < 0.0) angle += 2.0 * PI;
    return angle - PI;
}

} // namespace Gravitaris
