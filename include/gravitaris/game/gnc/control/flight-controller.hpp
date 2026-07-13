#pragma once

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/transform.hpp>

namespace Gravitaris {

// GNC control layer (docs/ai-ships.md): desired world-space velocity ->
// this tick's ControlFlags, for a ship with torque-limited rotation and a
// single forward thruster. Pure functions of (state, target, params), shared
// by pilot assists and AI pilots.
struct FlightControllerParams {
    double headingKp = 6.0;        // turn command per rad of heading error
    double headingKd = 1.5;        // damping on angular velocity (rad/s)
    double turnDeadband = 0.25;    // |PD output| below this -> no rotate bit
    double aimTolerance = 0.35;    // rad; thrust only when aimed this well
    double velocityDeadband = 1.5; // units/s; error below this is "done"

    // HoldPositionDesiredVelocity only.
    double positionKp = 0.8;
    double maxApproachSpeed = 60.0;
};

// Fire bits are left false; callers merge their own.
ControlFlags FlyToVelocity(const Transform& ship, const Magnum::Math::Vector2<double>& desiredVel,
                           const FlightControllerParams& params);

// Desired velocity that brings the ship to rest at `anchor`; feed the result
// to FlyToVelocity.
Magnum::Math::Vector2<double> HoldPositionDesiredVelocity(const Transform& ship,
                                                          const Magnum::Math::Vector2<double>& anchor,
                                                          const FlightControllerParams& params);

} // namespace Gravitaris
