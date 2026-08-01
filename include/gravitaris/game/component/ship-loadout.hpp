#pragma once

#include <array>
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
    // DamageSystem spends it ahead of the hull. For plating this is the sum of
    // `plates` -- kept in step by ShieldSystem so the HUD, the wire format and
    // ResolveStats need no per-type branch.
    float shieldHp = 0.f;
    // Ticks left before regen resumes, restarted by every hit that reaches
    // the shield. Bubble only; plating keeps one of these per plate.
    std::uint16_t shieldRegenDelay = 0;

    // ShieldType::Plating only: the ablative plates, indexed exactly as the
    // model's '+plating' paths are (Body::GetPlates). `plateCount` is that
    // model's count, copied in at spawn, so nothing downstream has to walk
    // back to the resource to know how many of the array is live.
    //
    // A plate spends down independently and regenerates independently, so a
    // ship under fire from one side keeps its far-side armour -- the whole
    // point of plates over one pooled bubble.
    std::uint8_t plateCount = 0;
    std::array<float, MAX_SHIELD_PLATES> plates{};
    // Per-plate quiet time, restarted only on the plate that was hit.
    // Deliberately NOT serialized: like Damageable::landingFragility, only the
    // server resolves damage, so a client never needs it.
    std::array<std::uint16_t, MAX_SHIELD_PLATES> plateRegenDelay{};
};

// Whether this ship's shield resolves per plate rather than as one pool. A
// plating emitter on a hull whose model authors no '+plating' layer falls back
// to the pooled shieldHp, so the upgrade is never a dead pickup on a hull that
// hasn't been drawn plates yet.
inline bool IsPlated(const ShipLoadout& loadout)
{
    return loadout.levels.shieldType == ShieldType::Plating && loadout.plateCount > 0;
}

// Capacity and regen split evenly across the plates, so a hull's plate count
// decides how *concentrated* its armour is, not how much of it there is --
// upgrades.toml's one set of numbers keeps meaning the same total charge and
// the same full-refill time whichever model carries them.
inline float PerPlate(const ShipLoadout& loadout, float total)
{
    return loadout.plateCount > 0 ? total / static_cast<float>(loadout.plateCount) : 0.f;
}

} // namespace Gravitaris
