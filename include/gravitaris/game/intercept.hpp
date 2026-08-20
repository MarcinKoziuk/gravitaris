#pragma once

#include <optional>

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

using Magnum::Vector2d;

// Most times SolveBallisticAim will re-solve the flight time against the drop
// the last solution implied before giving up on it. A shot that is barely
// falling settles in two or three; the cap is only reached by one that is not
// settling at all, which is how a round that cannot climb out of the well it
// was fired in is told apart from one that can.
constexpr int BALLISTIC_MAX_PASSES = 16;

// Flight times within this of each other are the same answer (seconds).
constexpr double BALLISTIC_TOLERANCE = 1e-4;

// Smallest positive time at which a projectile of `projectileSpeed` (measured
// relative to the shooter) meets a target at `relPos` closing at `relVel`.
// Nothing on a shot that can never catch what it is aimed at.
[[nodiscard]] std::optional<double> SolveInterceptTime(const Vector2d& relPos, const Vector2d& relVel,
                                                       double projectileSpeed);

struct BallisticSolution {
    Vector2d direction;      // unit, where to lay the barrel
    double flightTime = 0.0; // seconds to the meeting
};

// Where to point so a round that is itself falling at `accel` for the whole
// flight still arrives where the target will be. A zero `accel` gives back
// exactly the straight-line lead, so a shooter far from any well pays nothing
// for asking.
//
// `accel` is the round's own acceleration, not the difference between its and
// the target's: a target in the same field falls out of the way by its own
// amount, which no lead ever modelled and this does not pretend to either.
[[nodiscard]] std::optional<BallisticSolution> SolveBallisticAim(const Vector2d& relPos,
                                                                 const Vector2d& relVel,
                                                                 double projectileSpeed,
                                                                 const Vector2d& accel);

} // namespace Gravitaris
