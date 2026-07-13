#pragma once

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/transform.hpp>

namespace Gravitaris {

// GNC guidance layer (docs/ai-ships.md): each behavior maps (self, target,
// params) to a desired world-space velocity for FlyToVelocity. Pure
// functions; gravity is handled reactively by the control layer fighting the
// resulting velocity error, so behaviors stay simple.
struct GuidanceParams {
    double maxSpeed = 80.0;      // cruise cap (units/s)
    double accel = 140.0;        // available thrust acceleration (units/s^2)
    double flipTime = 1.2;       // seconds to turn retrograde before a burn
    double arriveRadius = 3.0;   // inside this, want zero velocity
    double orbitRadialKp = 0.5;  // radial correction per unit of radius error
    double maxRadialSpeed = 20.0;
};

// Arrive at `target` and stop. Approach speed respects flip-and-burn
// stopping distance: dist = v*flipTime + v^2/(2*accel), solved for v.
Magnum::Math::Vector2<double> GotoPoint(const Transform& ship,
                                        const Magnum::Math::Vector2<double>& target,
                                        const GuidanceParams& params);

// Circular orbit around a gravity source: tangential speed for a circular
// orbit at the current radius, plus a clamped radial correction toward
// `radius`. direction: +1 = counter-clockwise, -1 = clockwise.
Magnum::Math::Vector2<double> OrbitBody(const Transform& ship,
                                        const Magnum::Math::Vector2<double>& center, double centerMass,
                                        double radius, double direction,
                                        const GuidanceParams& params);

// Close on a moving target: GotoPoint at the target's dead-reckoned future
// position, plus the target's velocity so the closing speed is relative.
Magnum::Math::Vector2<double> InterceptEntity(const Transform& ship, const Transform& target,
                                              const GuidanceParams& params);

// Climb radially away from `center` until beyond `safeRadius`, preserving
// tangential motion. Returns the current velocity (no correction) when
// already safe.
Magnum::Math::Vector2<double> EvadeBody(const Transform& ship,
                                        const Magnum::Math::Vector2<double>& center, double safeRadius,
                                        const GuidanceParams& params);

} // namespace Gravitaris
