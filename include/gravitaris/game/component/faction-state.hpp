#pragma once

#include <cstdint>

#include <gravitaris/game/component/team.hpp>

namespace Gravitaris {

// One entity per team that has ever fielded a ship or structure (created
// lazily by FactionSystem, not pre-seeded for every TeamId value) --
// per-faction bookkeeping that outlives any single ship, needed because a
// dead ship's own LandingState (and its lastFriendlySiteNetId) is destructed
// along with it, but a respawn still needs to know where "home" was.
//
// Replication class: replicated (docs/gravity-well-mode-plan.md Phase 4 --
// a client's UI needs the defeated flag to grey out an eliminated faction).
struct FactionState {
    TeamId team = TeamId::None;
    // Most recent friendly-planet landing by ANY ship of this faction --
    // the respawn-site rule's primary source (docs/gravity-well-mode-plan.md
    // Phase 4: "the ship's last friendly landing site... if still alive and
    // friendly; else any remaining friendly planet/high port").
    std::uint32_t lastLandingSiteNetId = 0;
    // Set once a faction has zero colonies AND zero freighters (nothing left
    // that can regrow the economy); sticky -- never clears back to false.
    bool defeated = false;
};

} // namespace Gravitaris
