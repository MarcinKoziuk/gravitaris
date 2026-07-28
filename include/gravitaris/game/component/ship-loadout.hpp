#pragma once

#include <cstdint>

#include <gravitaris/game/upgrade/upgrade-def.hpp>

namespace Gravitaris {

// What a fighter carries beyond its hull: the upgrades it has collected from
// its faction's labs (ResearchSystem). Every ship has one from spawn, empty --
// stable membership, values change freely (see CLAUDE.md's ECS design note).
//
// Everything here is lost with the ship. Permanent faction-wide passives get
// their own UpgradeLevels block on FactionState and are folded in at
// resolution time (UpgradeCatalog::ResolveStats); nothing rolls those yet.
//
// Replication class: replicated (server -> clients) -- the sidebar's readouts
// and the shield ring both read the camera subject, not just the own ship.
struct ShipLoadout {
    std::uint8_t missileAmmo = 0;
    UpgradeLevels levels;
    // Shield charge, 0..ShipStats::shieldCapacity. ShieldSystem refills it;
    // DamageSystem spends it ahead of the hull.
    float shieldHp = 0.f;
    // Ticks left before regen resumes, restarted by every hit that reaches
    // the shield.
    std::uint16_t shieldRegenDelay = 0;
};

} // namespace Gravitaris
