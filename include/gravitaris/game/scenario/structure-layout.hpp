#pragma once

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/structure.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

// Where a structure sits relative to its planet. Single source of truth for
// the starting complex, freighter-built structures and Base
// self-development alike -- a grown complex has to look like a hand-placed
// one, and those three used to carry their own drifting copies of the
// numbers (the freighter built High Ports at half the starting complex's
// orbit radius).
namespace StructureLayout {

// Planetside structures are upright boxes nested inside the planet outline
// (the original's look -- see docs/gwell/screenshots/start-game.png), never
// rotated. Slots are fractions of the planet radius so the cluster keeps its
// arrangement at any planet size; the models themselves are authored around
// their own center, sized against the current 120-unit planet (they do not
// rescale with it, so a much smaller planet would need them redrawn).
//
// The Base sits dead centre -- the planet's ownership marker is drawn there
// too, so a claimed planet reads as a Base with a team-coloured core. The
// other three are spaced to clear it and each other at their authored sizes
// (see each model's `scale`); Lab and Comm Center are pulled in slightly
// from a uniform spacing so their far corners stay inside the outline.
inline Vector2d SurfaceOffset(StructureType type, double planetRadius)
{
    switch (type) {
        case StructureType::Base:       return Vector2d{};
        case StructureType::Colony:     return Vector2d{-0.56,  0.00} * planetRadius;
        case StructureType::Lab:        return Vector2d{ 0.40, -0.55} * planetRadius;
        case StructureType::CommCenter: return Vector2d{ 0.27,  0.50} * planetRadius;
        default:                        return Vector2d{};
    }
}

// High Port orbits at a fixed multiple of the planet's own radius, so it
// clears the surface at any planet size.
inline constexpr double ORBIT_RADIUS_FACTOR = 2.0;

inline double OrbitRadius(double planetRadius)
{
    return planetRadius * ORBIT_RADIUS_FACTOR;
}

// A station tracks its orbit this much slower than a free body would at the
// same radius -- it is under thrust (its _thrust group burns permanently),
// so it is not obliged to fly a ballistic orbit, and at the true circular
// speed matching it well enough to set down on the deck is unreasonable.
// Landing on the deck is the only way to refit at a port, so this figure is
// what makes the yard reachable by hand as well as by autopilot.
inline constexpr double ORBIT_SPEED_FACTOR = 1.0 / 5.0;

} // namespace StructureLayout

} // namespace Gravitaris
