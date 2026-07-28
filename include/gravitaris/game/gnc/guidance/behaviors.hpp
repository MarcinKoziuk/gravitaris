#pragma once

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/transform.hpp>

namespace Gravitaris {

// GNC guidance layer (docs/ai-ships.md): each behavior maps (self, target,
// params) to a desired world-space velocity for FlyToVelocity. Pure
// functions; gravity is handled reactively by the control layer fighting the
// resulting velocity error, so behaviors stay simple.
struct GuidanceParams {
    double maxSpeed = 80.0;      // combat cruise cap (units/s)
    double accel = 140.0;        // available thrust acceleration (units/s^2)

    // Cap for crossings that end in an arrival (GotoPoint, LandOnBody), well
    // above maxSpeed: both solve the fastest speed they can still brake from,
    // so the cap is what a transit actually flies at, and maxSpeed is a
    // dogfighting figure. InterceptEntity flies the transit figure too while
    // the target is further off than mergeRange (see its own comment).
    double transitSpeed = 400.0;

    // Range at which an intercept stops being a crossing and becomes a
    // fight, i.e. where InterceptEntity has to be down to maxSpeed.
    double mergeRange = 500.0;

    double flipTime = 1.2;       // seconds to turn retrograde before a burn
    double arriveRadius = 3.0;   // inside this, want zero velocity
    double orbitRadialKp = 0.5;  // radial correction per unit of radius error
    double maxRadialSpeed = 20.0;

    // LandOnBody only: the closing speed the descent solves to arrive at
    // (under LandingStateSystem::SAFE_LANDING_SPEED, with room for control
    // -layer overshoot), and the altitude below which it stops asking for
    // one. Inside flareAltitude the ship only matches the site's velocity --
    // a commanded descent there aims the thruster, and so the legs, away
    // from the surface, failing the uprightness test at contact.
    double touchdownSpeed = 7.0;
    double flareAltitude = 26.0;
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

// Set down on `center`, a body of radius `surfaceRadius` moving at
// `centerVel` -- planets orbit, so a touchdown is only gentle relative to
// the site. Descends radially in the body's frame at the fastest closing
// speed a flip-and-burn can still bleed to params.touchdownSpeed before
// contact, which kills tangential drift on the way in.
//
// `effectiveMass` includes the live gravity multiplier
// (PhysicsSystem::GetGravityMultiplier), unlike OrbitBody's centerMass:
// gravity is subtracted from the available thrust here, so its magnitude is
// the braking margin.
Magnum::Math::Vector2<double> LandOnBody(const Transform& ship,
                                         const Magnum::Math::Vector2<double>& center,
                                         const Magnum::Math::Vector2<double>& centerVel,
                                         double effectiveMass, double surfaceRadius,
                                         const GuidanceParams& params);

// Open the range on a threat: full cruise speed directly away from it,
// solved in the threat's frame so running from something that is itself
// moving asks for the speed that actually opens the gap.
Magnum::Math::Vector2<double> FleeThreat(const Transform& ship,
                                         const Magnum::Math::Vector2<double>& threatPos,
                                         const Magnum::Math::Vector2<double>& threatVel,
                                         const GuidanceParams& params);

// Climb radially away from `center` until beyond `safeRadius`, preserving
// tangential motion. Returns the current velocity (no correction) when
// already safe. Solved in the body's frame, so climbing off something that
// is itself moving fast (a station on its orbit) asks for a climb rather
// than for the world-space brake maxSpeed would otherwise clamp it to.
Magnum::Math::Vector2<double> EvadeBody(const Transform& ship,
                                        const Magnum::Math::Vector2<double>& center,
                                        const Magnum::Math::Vector2<double>& centerVel,
                                        double safeRadius, const GuidanceParams& params);

} // namespace Gravitaris
