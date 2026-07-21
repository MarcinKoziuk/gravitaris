#pragma once

#include <cstdint>

namespace Gravitaris {

// Which structure this freighter will build once it arrives -- decided at
// dispatch time (EconomySystem) from what the target planet was missing
// then; re-validated at build time (FreighterSystem) in case it's since
// been built by someone else.
enum class BuildOrder : std::uint8_t {
    Base,
    Colony,
    HighPort,
};

// A freighter in flight to build a structure at a claimed planet
// (docs/gravity-well-mode-plan.md Phase 3). Pre-loaded at spawn -- its
// materials cost is paid out of the producing Lab/Space Dock's
// finishedMaterials at dispatch time -- so unlike the original there is no
// separate "visit a colony to load cargo" trip for this construction role
// (see FreighterSystem's own doc comment for the full scope note).
//
// Replication class: replicated (a client needs to render/track it in
// flight), though every field here is consumed purely server-side today.
struct Freighter {
    std::uint32_t targetPlanetNetId = 0;
    BuildOrder buildOrder = BuildOrder::Base;
    // false: FreighterSystem drives its transit toward the target planet.
    // true: it has arrived and orbits via a real PlanetOrbitAttachment
    // (the same mechanism High Port uses) while its build is resolved.
    bool arrived = false;
};

} // namespace Gravitaris
