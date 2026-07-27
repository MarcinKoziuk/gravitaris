#pragma once

#include <cstdint>

namespace Gravitaris {

// What a fighter carries beyond its hull: the upgrades it has collected from
// its faction's labs (ResearchSystem). Every ship has one from spawn, empty --
// stable membership, values change freely (see CLAUDE.md's ECS design note).
// Faster guns and shields land here as fields too, once they exist.
//
// Replication class: replicated (server -> clients) -- the sidebar's ammo
// readout is the only reader, and it reads the camera subject, not just the
// own ship.
struct ShipLoadout {
    std::uint8_t missileAmmo = 0;
};

} // namespace Gravitaris
