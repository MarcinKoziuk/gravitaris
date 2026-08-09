#pragma once

#include <cmath>

#include <gravitaris/gravitaris.hpp>

namespace Gravitaris {

constexpr double HALF_PI = PI / 2.0;
constexpr double TWO_PI = PI * 2.0;

// The equivalent angle in (-PI, PI]. What every "how far round is that, the
// short way" question wants, and what four separate translation units each
// kept their own copy of.
inline double WrapToPi(double angle)
{
    angle = std::fmod(angle + PI, TWO_PI);
    if (angle < 0.0) angle += TWO_PI;
    return angle - PI;
}

} // namespace Gravitaris
