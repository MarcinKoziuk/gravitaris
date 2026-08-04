#pragma once

#include <cstdint>

#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/upgrade/upgrade-def.hpp>

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
    // Fighter-upgrade research, pooled across the faction's Labs (all labs
    // cooperate, so more labs finish sooner -- gravity-well-1997.md). 0..1;
    // restarts from 0 each time it fills. Authoritative copy: clients read it
    // off each Lab's Structure, which ResearchSystem mirrors it into.
    float researchProgress = 0.f;
    // Technology points: what the bar above pays out each time it fills, and
    // what the PERMANENT tree spends. Uncapped -- the pressure to come home is
    // that a hull stays stock until its pilot lands, not that the labs idle.
    std::uint32_t techPoints = 0;
    // What this side has learned how to build, which is the ceiling on what
    // any of its hulls may fit. Outlives every ship, and every pilot.
    TechUnlocks unlocked;
};

} // namespace Gravitaris
