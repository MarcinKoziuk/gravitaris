#pragma once

#include <cstdint>

namespace Gravitaris {

// Which side an entity belongs to. Order is the default color-schema order
// (see cgame team-color.hpp); Blue is the player's default.
enum class TeamId : std::uint8_t {
    Blue,
    Red,
    Green,
    Yellow,
    Magenta,
    Cyan,
};

struct Team {
    TeamId id = TeamId::Blue;
};

} // namespace Gravitaris
