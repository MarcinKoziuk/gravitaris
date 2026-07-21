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
    // (the same mechanism High Port uses) while its cargo is unloaded.
    bool arrived = false;
    // Two cargo pods (matches the model -- freighter-0.svg's hull carries
    // two visible pods), unloaded one at a time after arrival: the first
    // tops up the target planet's existing Base (if any) with raw
    // materials; the second resolves the freighter's build order (or is
    // simply consumed if someone else already built it). Counts down
    // 2 -> 1 -> 0; the freighter is destructed once it reaches 0.
    std::uint8_t cargoRemaining = 2;
    // Ticks since arrival (or since the last cargo unload) -- gates the
    // next unload so the two events read as sequential, not simultaneous.
    std::uint32_t ticksSinceUnload = 0;
};

} // namespace Gravitaris
