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
    // tiered upgrade is maxed, or while its prerequisite (UpgradeDef::requiresId)
    // is unfitted. A shield of the other type is always eligible: taking it
    // swaps, and swapping down is the player's call to make.
    [[nodiscard]] bool IsEligible(const UpgradeDef& def, const UpgradeLevels& levels) const;

    // How many finished upgrades this faction can hold before its labs idle:
    // `base` (economy.toml) widened by the ResearchStock levels it has taken.
    [[nodiscard]] int ResearchStockCapacity(const UpgradeLevels& levels, int base) const;

    // OFFER_COUNT distinct eligible upgrades drawn by weight, most eligible
    // first if the pool is too small to fill it (unused slots are 0). `seed`
    // must be derived from sim state only (ADR 0001: no std::rand), so two
    // peers rolling the same draft agree.
    [[nodiscard]] Offers RollOffers(const UpgradeLevels& levels, std::uint32_t seed) const;

    // The level a ship holds in `def`, for the draft panel's "II -> III".
    [[nodiscard]] static std::uint8_t LevelOf(const UpgradeDef& def, const UpgradeLevels& levels);

    // A ship's own levels overlaid with its faction's. Every field belongs to
    // exactly one scope, so this is a merge rather than a choice -- and it is
    // what a draft has to be rolled and scored against, since the pool holds
    // cards of both scopes.
    [[nodiscard]] static UpgradeLevels Combined(const UpgradeLevels& ship, const UpgradeLevels& faction);

    // Which of `offers` an AI takes, as a 1-based slot (0 when none of them
    // can be applied). A human reads the three cards and decides; a pilot
    // scores them against what it is already carrying, so a wing ends up
    // spread across shields, racks and guns instead of every ship taking
    // whatever landed in slot one. `levels` is Combined(). Deterministic --
    // state in, slot out, no randomness (ADR 0001).
    [[nodiscard]] std::uint8_t PreferredOffer(const Offers& offers, const ShipLoadout& loadout,
                                              const UpgradeLevels& levels) const;

    // Grants one UpgradeScope::Ship pick. Returns false if it was a no-op
    // (unknown id, or an already-maxed tier), which keeps the research bar
    // full so the pick can be retried rather than silently burning the
    // upgrade.
    bool Apply(const UpgradeDef& def, ShipLoadout& loadout) const;

    // The same, for an UpgradeScope::Faction pick: nothing it grants touches a
    // hull, so there is no loadout to hand it -- it lands on the faction's own
    // levels block and outlives every ship that collected it.
    bool ApplyFaction(const UpgradeDef& def, UpgradeLevels& levels) const;

private:
    std::vector<WeaponDef> m_weapons;
    std::vector<UpgradeDef> m_defs;
    Fittings m_fittings;
};

} // namespace Gravitaris
