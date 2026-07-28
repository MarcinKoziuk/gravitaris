#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/upgrade/upgrade-def.hpp>

namespace Gravitaris {

// The weapon table, the research pool, and the ship stats they feed, parsed
// from data/upgrades.toml. One instance lives on Game; CGame reads the same
// one for the draft panel's names and the audio system's fire sounds.
//
// A missing or malformed file is not fatal: the built-in defaults below stand
// in, so the sim always has a base gun and an empty pool rather than a ship
// that cannot shoot.
class UpgradeCatalog {
public:
    static constexpr std::size_t OFFER_COUNT = 3;
    using Offers = std::array<id_t, OFFER_COUNT>;

    // What an unupgraded hull is fitted with, and what a structure's turret
    // fires (data/upgrades.toml's [ship] and [turret]).
    struct Fittings {
        id_t shipGun = 0;
        id_t shipMissile = 0;
        int missileCapacity = 60;

        id_t turretWeapon = 0;
        double turretFireRange = 400.0;
        std::uint32_t turretFireCooldownTicks = 90;
    };

    UpgradeCatalog() = default;

    // Reads `path` (default "upgrades.toml"). Returns false and keeps the
    // defaults if it can't be read or parsed.
    bool Load(IFilesystem& filesystem, const char* path = "upgrades.toml");

    [[nodiscard]] const std::vector<UpgradeDef>& Defs() const { return m_defs; }

    [[nodiscard]] const std::vector<WeaponDef>& Weapons() const { return m_weapons; }

    [[nodiscard]] const Fittings& Fitted() const { return m_fittings; }

    // Null for an id that isn't in the table (a stale wire value, or a client
    // whose data file disagrees with the server's).
    [[nodiscard]] const UpgradeDef* Find(id_t id) const;

    [[nodiscard]] const WeaponDef* FindWeapon(id_t id) const;

    // The single definition of a kind -- shields excepted, which have one per
    // ShieldType. Null when the pool has none.
    [[nodiscard]] const UpgradeDef* FindKind(UpgradeKind kind, ShieldType shieldType = ShieldType::None) const;

    [[nodiscard]] ShipStats ResolveStats(const UpgradeLevels& levels) const;

    // Whether `def` can still be offered to a ship at `levels` -- false once a
    // tiered upgrade is maxed. A shield of the other type is always eligible:
    // taking it swaps, and swapping down is the player's call to make.
    [[nodiscard]] bool IsEligible(const UpgradeDef& def, const UpgradeLevels& levels) const;

    // OFFER_COUNT distinct eligible upgrades drawn by weight, most eligible
    // first if the pool is too small to fill it (unused slots are 0). `seed`
    // must be derived from sim state only (ADR 0001: no std::rand), so two
    // peers rolling the same draft agree.
    [[nodiscard]] Offers RollOffers(const UpgradeLevels& levels, std::uint32_t seed) const;

    // The level a ship holds in `def`, for the draft panel's "II -> III".
    [[nodiscard]] static std::uint8_t LevelOf(const UpgradeDef& def, const UpgradeLevels& levels);

    // Grants one pick. Returns false if it was a no-op (unknown id, or an
    // already-maxed tier), which keeps the research bar full so the pick can
    // be retried rather than silently burning the upgrade.
    bool Apply(const UpgradeDef& def, ShipLoadout& loadout) const;

private:
    std::vector<WeaponDef> m_weapons;
    std::vector<UpgradeDef> m_defs;
    Fittings m_fittings;
};

} // namespace Gravitaris
