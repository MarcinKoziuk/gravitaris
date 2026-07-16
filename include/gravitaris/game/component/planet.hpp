#pragma once

namespace Gravitaris {

// Marks a celestial body: the massive, gravity-well-forming things ships fly
// around, as opposed to ships, bullets or debris. Stable membership -- a planet
// is a planet for its whole life -- so a real component rather than a field on
// something else (see CLAUDE.md's ECS component design note).
//
// Empty tag: add with entity.add<Planet>(), test with entity.has<Planet>();
// flecs stores zero-sized types as tags, which can't be fetched as data in a
// query term.
struct Planet {};

} // namespace Gravitaris
