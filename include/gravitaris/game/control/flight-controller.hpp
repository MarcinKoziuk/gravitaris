#pragma once

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/transform.hpp>

namespace Gravitaris {

// Control layer of the pilot GNC stack (docs/ai-ships.md, phase 2): turns a
// desired world-space velocity into this tick's binary ControlFlags, given
// the ship's actual actuators -- torque-limited rotation and a single
// forward thruster (local -Y; see ShipControlsSystem). Pure function of
// (state, target, params): no internal state, so human autopilot assists and
// AI pilots share it, and the server can re-run ticks (ADR 0001 p5).
//
// Scheme: bang-bang steering from a PD term on heading error (the actuator
// only has rotate-left/right bits), thrust gated on being roughly aimed at
// the velocity error and the error being worth correcting.
struct FlightControllerParams {
    double headingKp = 6.0;        // turn command per rad of heading error
    double headingKd = 1.5;        // damping on angular velocity (rad/s)
    double turnDeadband = 0.25;    // |PD output| below this -> no rotate bit
    double aimTolerance = 0.35;    // rad; thrust only when aimed this well
    double velocityDeadband = 1.5; // units/s; error below this is "done"

    // Guidance helper (hold position): approach speed per unit of distance,
    // clamped. Not used by FlyToVelocity itself.
    double positionKp = 0.8;
    double maxApproachSpeed = 60.0;
};

// One tick's control decision to reach `desiredVel`. Fire bits are left
// false; callers merge their own.
ControlFlags FlyToVelocity(const Transform& ship, const Magnum::Math::Vector2<double>& desiredVel,
                           const FlightControllerParams& params);

// Desired velocity that brings the ship to rest at `anchor` (proportional
// approach with clamped speed). Feed the result to FlyToVelocity.
Magnum::Math::Vector2<double> HoldPositionDesiredVelocity(const Transform& ship,
                                                          const Magnum::Math::Vector2<double>& anchor,
                                                          const FlightControllerParams& params);

} // namespace Gravitaris
