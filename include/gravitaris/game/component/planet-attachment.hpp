#pragma once

#include <cstdint>

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

using Magnum::Vector2d;

// Rides a fixed offset from its parent planet's live position (the boxes
// -within-the-outline look from the original -- see
// docs/gravity-well-1997.md). Not a full Orbit: a planet itself moves (it
// orbits a sun), so a fixed-in-world-space offset would leave the structure
// behind as its planet travels. StructureAttachmentSystem re-derives the
// kinematic position/velocity from the parent's current Transform every
// tick instead.
//
// Replication class: server-only. The resulting position/velocity already
// ride the normal Transform-derived EntityState fields every replicated
// entity has, so clients never need to know this component exists.
struct PlanetSurfaceAttachment {
    std::uint32_t planetNetId = 0;
    Vector2d localOffset;
};

// Same idea as PlanetSurfaceAttachment, but for structures that actually
// orbit the planet (High Port, and Space Dock/Sensor Array attached to it)
// rather than sitting fixed on its surface. Mirrors Orbit's own circular
// -orbit math (see its doc comment) but centered on a live parent position
// instead of a fixed point, since OrbitSystem's Orbit::center can't track a
// moving planet without complicating its simple, replicated, fixed-center
// contract for the existing planet-orbits-sun case.
//
// Replication class: server-only (see PlanetSurfaceAttachment).
struct PlanetOrbitAttachment {
    std::uint32_t planetNetId = 0;
    double centerMass = 0.0; // the planet's own gravitational mass, drives angular speed
    double radius = 0.0;
    double theta = 0.0;
    double direction = 1.0;
};

} // namespace Gravitaris
