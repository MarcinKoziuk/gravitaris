#include <algorithm>
#include <cmath>
#include <limits>

#include <Magnum/Math/Functions.h>

#include <gravitaris/game/intercept.hpp>

namespace Gravitaris {

std::optional<double> SolveInterceptTime(const Vector2d& relPos, const Vector2d& relVel,
                                         double projectileSpeed)
{
    const double a = relVel.dot() - projectileSpeed * projectileSpeed;
    const double b = 2.0 * Magnum::Math::dot(relPos, relVel);
    const double c = relPos.dot();

    if (std::abs(a) < 1e-9) {
        if (std::abs(b) < 1e-9) return std::nullopt;
        const double t = -c / b;
        return t > 0.0 ? std::optional<double>(t) : std::nullopt;
    }

    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return std::nullopt;

    const double sq = std::sqrt(disc);
    const double t1 = (-b - sq) / (2.0 * a);
    const double t2 = (-b + sq) / (2.0 * a);

    double t = std::numeric_limits<double>::max();
    if (t1 > 0.0) t = std::min(t, t1);
    if (t2 > 0.0) t = std::min(t, t2);
    return t != std::numeric_limits<double>::max() ? std::optional<double>(t) : std::nullopt;
}

std::optional<BallisticSolution> SolveBallisticAim(const Vector2d& relPos, const Vector2d& relVel,
                                                   double projectileSpeed, const Vector2d& accel)
{
    std::optional<double> t = SolveInterceptTime(relPos, relVel, projectileSpeed);
    if (!t) return std::nullopt;

    // Aiming off a point the drop has already been taken out of, rather than
    // off the target: the further out that puts the shot, the longer it is in
    // the air, and the further it falls on the way -- so the time has to be
    // re-solved against it until the two stop moving each other.
    //
    // They do not always: a round with no hope of climbing out of the well it
    // was fired in runs away to longer flights and bigger drops every pass,
    // and a firing solution is the wrong answer to give for that.
    Vector2d aimFrom = relPos;
    if (accel.dot() > 0.0) {
        bool settled = false;
        for (int pass = 0; pass < BALLISTIC_MAX_PASSES && !settled; ++pass) {
            aimFrom = relPos - accel * (0.5 * (*t) * (*t));
            const std::optional<double> next = SolveInterceptTime(aimFrom, relVel, projectileSpeed);
            if (!next) return std::nullopt;
            settled = std::abs(*next - *t) <= BALLISTIC_TOLERANCE * std::max(1.0, *t);
            t = next;
        }
        if (!settled) return std::nullopt;
    }

    const Vector2d lead = aimFrom + relVel * (*t);
    const double reach = lead.length();
    if (reach < 1e-9) return std::nullopt;
    return BallisticSolution{lead / reach, *t};
}

} // namespace Gravitaris
