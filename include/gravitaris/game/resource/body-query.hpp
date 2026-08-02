#pragma once

#include <cstdint>
#include <optional>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/resource/body.hpp>

namespace Gravitaris {

// Where a swept segment first meets a Body's shapes, resolved straight from the
// resource instead of from a live Chipmunk space. The server has a space and
// uses cpSpaceSegmentQuery (DamageSystem); a client's snapshot-mirror world is
// presentation-only (ADR 0001) and has none, so this is how it resolves its own
// cosmetic hits against the same authored geometry the server collided with.
//
// The shapes tested are exactly the ones PhysicsSystem builds: the hull
// polygons and circles, the '+shield' bubble outline, and one segment per
// '+plating' span -- so an element reported here means the same thing an
// element reported by PhysicsBody::ShieldElementOf does.
struct BodyHit {
    Magnum::Vector2d point;
    double alpha = 0.;                          // 0..1 along the queried segment
    std::optional<std::uint8_t> shieldElement;  // SHIELD_BUBBLE_ELEMENT, a plate index, or none for hull
};

// Nearest hit along `from` -> `to` on a body sitting at `pos`/`rot`/`scale`, or
// none. `radius` is a forgiveness margin around the segment, in world units,
// matching DamageSystem's own BULLET_QUERY_RADIUS.
[[nodiscard]] std::optional<BodyHit> QueryBodySegment(const Body& body, const Magnum::Vector2d& pos, double rot,
                                                     const Magnum::Vector2d& scale,
                                                     const Magnum::Vector2d& from, const Magnum::Vector2d& to,
                                                     double radius);

} // namespace Gravitaris
