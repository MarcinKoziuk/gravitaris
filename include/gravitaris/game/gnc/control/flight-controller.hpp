#pragma once

#include <cstdint>

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

    // Throttle dwell, for callers that pass a ThrottleState. Ticks at
    // Game::PHYSICS_DELTA; larger values read as a more deliberate hand on
    // the throttle, smaller ones as a twitchier pilot.
    std::uint32_t minBurnTicks = 14;
    std::uint32_t minCoastTicks = 12;
    double restartFactor = 3.0; // velocityDeadband multiple that restarts a burn
    // velocityDeadband multiple above which a coast is abandoned mid-dwell.
    // Without it a dwell holds through a descent's last seconds, where gravity
    // adds more speed per tick than the burn afterwards can take back.
    double urgentFactor = 6.0;

    // HoldPositionDesiredVelocity only.
    double positionKp = 0.8;
    double maxApproachSpeed = 60.0;
};

// Throttle memory for one ship across ticks. Without it the thruster is pure
// bang-bang and chatters at tick rate whenever the velocity error hovers near
// velocityDeadband; with it a burn lasts at least minBurnTicks and the coast
// after it at least minCoastTicks, which is what makes the cadence read as a
// hand on a throttle rather than a solenoid.
struct ThrottleState {
    std::uint32_t holdTicks = 0;
    bool thrusting = false;
};

// Fire bits are left false; callers merge their own. `throttle` is optional:
// pass null for the original chattery bang-bang throttle (player assists,
// where the input is only ever held for a moment anyway).
ControlFlags FlyToVelocity(const Transform& ship, const Magnum::Math::Vector2<double>& desiredVel,
                           const FlightControllerParams& params, ThrottleState* throttle = nullptr);

// Desired velocity that brings the ship to rest at `anchor`; feed the result
// to FlyToVelocity.
Magnum::Math::Vector2<double> HoldPositionDesiredVelocity(const Transform& ship,
                                                          const Magnum::Math::Vector2<double>& anchor,
                                                          const FlightControllerParams& params);

} // namespace Gravitaris
