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
    // Cached copy of the angular speed OrbitSystem computes fresh each tick
    // (from centerMass/radius/direction and PhysicsSystem's live gravity
    // multiplier) -- stored back here so GatherSnapshot can replicate it
    // directly without also needing a PhysicsSystem reference, and a client
    // can re-derive this planet's exact position at any tick from just
    // (center, radius, theta, angularSpeed) + the shared tick (see
    // EvaluateOrbit in game/net/snapshot.hpp) instead of only ever knowing
    // its raw position as of whichever snapshot last happened to arrive.
    double angularSpeed = 0.0;
};

} // namespace Gravitaris
