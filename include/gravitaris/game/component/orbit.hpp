#pragma once

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

using Magnum::Vector2d;

// A pre-calculated circular 2-body orbit: the entity's position is a pure
// function of the tick, not an integrated n-body result. So it stays exactly
// reproducible and the planet rides fixed rails no matter what bounces into
// it. OrbitSystem writes it onto a kinematic physics body before each physics
// step, deriving the angular speed from centerMass and PhysicsSystem's
// gravity constant/multiplier each tick -- the same circular-orbit velocity a
// freely falling ship would need at this radius, so a manual orbit lines up
// with the scripted one.
//
// Replicated (ADR 0001 constraint 2): a client re-derives the same positions
// from these parameters plus the shared tick -- no per-frame position stream.
struct Orbit {
    Vector2d center;
    double centerMass = 0.0; // gravitational mass orbited (drives angular speed)
    double radius = 0.0;
    double theta = 0.0;      // current orbital angle; advanced by OrbitSystem each tick
    double direction = 1.0;  // sign only (+1/-1): orbit direction
};

} // namespace Gravitaris
