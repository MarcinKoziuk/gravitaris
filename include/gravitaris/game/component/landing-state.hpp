#pragma once

#include <cstdint>

namespace Gravitaris {

// Per-ship landing status, updated every tick by LandingSystem. Stable
// membership (every fighter has one for its whole life); the fields toggle
// freely instead of add/remove tag churn (see CLAUDE.md's ECS design note).
//
// Replication class: server-only. Clients see landing through position/
// velocity; the HUD's "safe to land" aid is derived from predicted local
// state, not from this.
struct LandingState {
    bool landed = false;
    std::uint32_t landedOnNetId = 0;         // planet currently rested on (0 = none)
    std::uint32_t landedTicks = 0;           // consecutive ticks landed (claim gate)
    std::uint32_t lastFriendlySiteNetId = 0; // most recent friendly planet landed on (respawn site)
};

} // namespace Gravitaris
