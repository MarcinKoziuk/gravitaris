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
    // What each of the hull's weapon mounts is armed with, indexed exactly as
    // its 'weapon_N' hardpoints are (Body::FindMount). This is the placement;
    // UpgradeLevels holds the rank of each line, which is the ship's rather
    // than the mount's.
    std::array<MountArm, MAX_WEAPON_MOUNTS> mounts{};

    // Which of the hull's 'missile_N' bays carry a launcher, one bit per bay.
    // The same split as `mounts`: UpgradeLevels::missileTier is the round the
    // ship has paid for, and this is where the tubes to fire it are. A
    // bitmask rather than an array because a bay is fitted or it isn't, and
    // one byte covers MAX_WEAPON_MOUNTS of them -- which is also what the
    // wire sends.
    //
    // The rack itself stays the ship's, exactly as the cannon's magazine is
    // shared across its heavy mounts: a second bay buys another tube to empty
    // it through, not a second rack.
    std::uint8_t missileBays = 0;

    // What rides in each of the hull's 'ammo_N' stowage bays, indexed the same
    // way. A bay is generic -- either locker goes in either one -- so this is
    // where the choice between shells and rounds actually lives;
    // UpgradeLevels::ammoStore is only the count of each, which is what
    // ResolveStats needs and all it needs.
    std::array<AmmoPool, MAX_AMMO_BAYS> ammoBays{};

    std::uint16_t missileAmmo = 0;
    // Rounds left in the cannon's magazine. Deeper than the rack by an order
    // of magnitude -- see the HUD, which draws one tick per ten.
    std::uint16_t cannonAmmo = 0;
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
// How many mounts carry a line. Zero means the trigger has nothing to work
// even if the rank is owned -- fitting a line and mounting it are now two
// different things.
inline int MountsArmedWith(const ShipLoadout& loadout, MountArm arm)
{
    int count = 0;
    for (const MountArm mount : loadout.mounts) {
        if (mount == arm) ++count;
    }
    return count;
}

// Whether that bay carries a launcher. Out-of-range bays never do, which is
// what keeps a hull that authors more 'missile_N' markers than the mask is
// wide simply quiet on the surplus rather than reading past it.
inline bool MissileBayFitted(const ShipLoadout& loadout, unsigned bay)
{
    return bay < MAX_WEAPON_MOUNTS && (loadout.missileBays & (1u << bay)) != 0;
}

inline void SetMissileBay(ShipLoadout& loadout, unsigned bay, bool fitted)
{
    if (bay >= MAX_WEAPON_MOUNTS) return;
    const auto bit = static_cast<std::uint8_t>(1u << bay);
    loadout.missileBays = static_cast<std::uint8_t>(fitted ? (loadout.missileBays | bit)
                                                           : (loadout.missileBays & ~bit));
}

// How many bays carry one. Zero means the rack has nowhere to fire from, the
// same way an unmounted gun line has nothing to shoot out of.
inline int MissileBaysFitted(const ShipLoadout& loadout)
{
    int count = 0;
    for (unsigned bay = 0; bay < MAX_WEAPON_MOUNTS; ++bay) {
        if (MissileBayFitted(loadout, bay)) ++count;
    }
    return count;
}

// How many stowage bays hold a locker feeding `pool`. The authority on it:
// UpgradeLevels::ammoStore is this count, cached where ResolveStats can reach
// it without a loadout.
inline std::uint8_t AmmoBaysHolding(const ShipLoadout& loadout, AmmoPool pool)
{
    if (pool == AmmoPool::None) return 0;

    std::uint8_t count = 0;
    for (const AmmoPool held : loadout.ammoBays) {
        if (held == pool) ++count;
    }
    return count;
}

// Copies the bay placement back onto the levels. Every path that puts a locker
// in a bay or takes one out has to call it, or the two disagree about how deep
// the magazines are.
inline void SyncAmmoStoreCounts(ShipLoadout& loadout)
{
    SetAmmoStoreRank(loadout.levels, AmmoPool::Cannon, AmmoBaysHolding(loadout, AmmoPool::Cannon));
    SetAmmoStoreRank(loadout.levels, AmmoPool::Missile, AmmoBaysHolding(loadout, AmmoPool::Missile));
}

inline bool IsPlated(const ShipLoadout& loadout)
{
    return loadout.levels.shieldType == ShieldType::Plating && loadout.plateCount > 0;
}

// A shield element stops a shot only while it is the emitter the ship is
// actually carrying and still has charge to spend against it. `element` is a
// plate index or SHIELD_BUBBLE_ELEMENT, as reported by the geometry a round
// met (PhysicsBody::ShieldElementOf server-side, QueryBodySegment on a client).
inline bool ShieldElementLive(const ShipLoadout& loadout, std::uint8_t element)
{
    if (element == SHIELD_BUBBLE_ELEMENT) {
        return loadout.levels.shieldType == ShieldType::Bubble && loadout.shieldHp > 0.f;
    }
    return IsPlated(loadout) && element < loadout.plateCount && loadout.plates[element] > 0.f;
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
