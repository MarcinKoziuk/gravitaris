#include <algorithm>
#include <cmath>
#include <limits>

#include <Magnum/Math/Functions.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/resource/body-query.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

// Half-width the plating segments are given, matching PhysicsSystem's own
// PLATE_THICKNESS so a plate stops a round here exactly where it stops one in
// the sim.
static constexpr double PLATE_THICKNESS = 0.6;

static std::optional<double> SegmentVsSegment(const Vector2d& a, const Vector2d& b, const Vector2d& c,
                                              const Vector2d& d, double radius);
static std::optional<double> SegmentVsCircle(const Vector2d& a, const Vector2d& b, const Vector2d& center,
                                             double radius);

std::optional<BodyHit> QueryBodySegment(const Body& body, const Vector2d& pos, double rot,
                                        const Vector2d& scale, const Vector2d& from, const Vector2d& to,
                                        double radius)
{
    // Into the body's own frame, where the authored geometry lives: rotation
    // and translation undone, but NOT the scale -- PhysicsSystem scales the
    // shape vertices rather than the space around them (InitBody), so scaling
    // the geometry the same way here keeps distances in world units.
    const double s = std::sin(rot);
    const double c = std::cos(rot);
    const auto toLocal = [&](const Vector2d& world) {
        const Vector2d rel = world - pos;
        return Vector2d{rel.x() * c + rel.y() * s, -rel.x() * s + rel.y() * c};
    };
    const Vector2d a = toLocal(from);
    const Vector2d b = toLocal(to);
    if (a == b) return std::nullopt;

    std::optional<BodyHit> nearest;
    const auto consider = [&](std::optional<double> alpha, std::optional<std::uint8_t> element) {
        if (!alpha || (nearest && *alpha >= nearest->alpha)) return;
        nearest = BodyHit{from + (to - from) * *alpha, *alpha, element};
    };

    for (const std::vector<TVector2<cpFloat>>& poly : body.GetPolygonShapes()) {
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const Vector2d p = Vector2d{poly[i]} * scale;
            const Vector2d q = Vector2d{poly[(i + 1) % poly.size()]} * scale;
            consider(SegmentVsSegment(a, b, p, q, radius), std::nullopt);
        }
    }

    for (const Body::CircleShape& circle : body.GetCircleShapes()) {
        consider(SegmentVsCircle(a, b, Vector2d{circle.pos} * scale, circle.radius * scale.x() + radius),
                 std::nullopt);
    }

    const std::vector<TVector2<cpFloat>>& outline = body.GetShieldOutline();
    if (outline.size() >= 3) {
        for (std::size_t i = 0; i < outline.size(); ++i) {
            const Vector2d p = Vector2d{outline[i]} * scale;
            const Vector2d q = Vector2d{outline[(i + 1) % outline.size()]} * scale;
            consider(SegmentVsSegment(a, b, p, q, radius), SHIELD_BUBBLE_ELEMENT);
        }
    }

    const std::vector<Body::Plate>& plates = body.GetPlates();
    for (std::size_t i = 0; i < plates.size() && i < MAX_SHIELD_PLATES; ++i) {
        const Body::Plate& plate = plates[i];
        for (std::size_t j = 0; j + 1 < plate.size(); ++j) {
            const Vector2d p = Vector2d{plate[j]} * scale;
            const Vector2d q = Vector2d{plate[j + 1]} * scale;
            consider(SegmentVsSegment(a, b, p, q, radius + PLATE_THICKNESS * scale.x()),
                     static_cast<std::uint8_t>(i));
        }
    }

    return nearest;
}

// Fraction along a->b at which it first comes within `radius` of c->d. A proper
// crossing is solved exactly; a near miss falls back to the closest approach,
// which is what makes `radius` mean the same forgiveness margin the sim's own
// segment query is given.
static std::optional<double> SegmentVsSegment(const Vector2d& a, const Vector2d& b, const Vector2d& c,
                                              const Vector2d& d, double radius)
{
    const Vector2d ab = b - a;
    const Vector2d cd = d - c;
    const Vector2d ac = c - a;

    const double denom = Magnum::Math::cross(ab, cd);
    if (std::abs(denom) > 1e-12) {
        const double t = Magnum::Math::cross(ac, cd) / denom;
        const double u = Magnum::Math::cross(ac, ab) / denom;
        if (t >= 0. && t <= 1. && u >= 0. && u <= 1.) return t;
    }

    if (radius <= 0.) return std::nullopt;

    // Closest point on a->b to the other segment, by sampling both endpoints
    // and the perpendicular foot of each -- enough for the small margins this
    // is used with, and free of the degenerate cases a full solve needs.
    const double abLengthSq = ab.dot();
    double bestT = 0.;
    double bestDistSq = std::numeric_limits<double>::max();
    const auto tryPoint = [&](const Vector2d& point) {
        const double t = abLengthSq > 0. ? std::clamp(Magnum::Math::dot(point - a, ab) / abLengthSq, 0., 1.) : 0.;
        const Vector2d onAb = a + ab * t;
        const double cdLengthSq = cd.dot();
        const double u = cdLengthSq > 0. ? std::clamp(Magnum::Math::dot(onAb - c, cd) / cdLengthSq, 0., 1.) : 0.;
        const double distSq = (onAb - (c + cd * u)).dot();
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestT = t;
        }
    };
    tryPoint(a);
    tryPoint(b);
    tryPoint(c);
    tryPoint(d);

    return bestDistSq <= radius * radius ? std::optional<double>(bestT) : std::nullopt;
}

static std::optional<double> SegmentVsCircle(const Vector2d& a, const Vector2d& b, const Vector2d& center,
                                             double radius)
{
    const Vector2d ab = b - a;
    const Vector2d ac = a - center;

    const double qa = ab.dot();
    if (qa <= 0.) return std::nullopt;
    const double qb = 2. * Magnum::Math::dot(ac, ab);
    const double qc = ac.dot() - radius * radius;

    if (qc <= 0.) return 0.; // already inside

    const double discriminant = qb * qb - 4. * qa * qc;
    if (discriminant < 0.) return std::nullopt;

    const double t = (-qb - std::sqrt(discriminant)) / (2. * qa);
    return t >= 0. && t <= 1. ? std::optional<double>(t) : std::nullopt;
}

} // namespace Gravitaris
