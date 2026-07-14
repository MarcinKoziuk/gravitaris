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
    Green,
    Yellow,
    Magenta,
    Cyan,
    None,
};

struct Team {
    TeamId id = TeamId::Blue;
};

} // namespace Gravitaris
