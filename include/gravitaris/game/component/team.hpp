#pragma once

#include <cstdint>

namespace Gravitaris {

// Which side an entity belongs to. Order is the default color-schema order
// (see cgame team-color.hpp); Blue is the player's default. None is the
// ownerless "hostile to everyone" team used by frag-explosion shrapnel, so
// the friendly-fire check (which only skips matching teams) never spares it.
enum class TeamId : std::uint8_t {
    Blue,
    Red,
    Violet,
    Yellow,
    Magenta,
    Cyan,
    None,
};

struct Team {
    TeamId id = TeamId::Blue;
};

// The lowercase colour a side's ships are addressed by: "red-leader",
// "red-1", and what /tp resolves and /spawn takes as a team argument. Distinct
// from TeamDisplayName (death-report.hpp), which is the same colours written
// for prose -- these are identifiers somebody types.
inline const char* TeamCallsignPrefix(TeamId team)
{
    switch (team) {
    case TeamId::Blue: return "blue";
    case TeamId::Red: return "red";
    case TeamId::Violet: return "violet";
    case TeamId::Yellow: return "yellow";
    case TeamId::Magenta: return "magenta";
    case TeamId::Cyan: return "cyan";
    case TeamId::None: break;
    }
    return "rogue";
}

// The colours a round hands out, in order: faction i of a round is
// FACTION_ROSTER[i]. Shared by sector generation, the server's auto-assign
// and the setup screen so the three can't disagree about who faction 2 is.
// None is deliberately absent -- it is the ownerless team, not a side.
inline constexpr TeamId FACTION_ROSTER[] = {
    TeamId::Blue, TeamId::Red, TeamId::Violet, TeamId::Yellow, TeamId::Magenta, TeamId::Cyan,
};

} // namespace Gravitaris
