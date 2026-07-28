#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <gravitaris/game/id.hpp>

namespace Gravitaris {

// One thing that can be fired, loaded from data/upgrades.toml's [[weapon]]
// table. Everything that shoots names one of these -- a ship's gun and rack,
// a turret, and each tier of a gun line -- so a projectile's stats live in
// exactly one place and a new weapon is a data edit plus its assets.
struct WeaponDef {
    id_t id = 0; // FNV of the toml key -- what the wire and the audio system address it by
    std::string key;
    std::string name;

    std::uint32_t cooldownTicks = 1;
    float damage = 0.f;
    double speed = 0.;
    double lifetimeSeconds = 0.;

    id_t modelId = 0; // the projectile's model
    id_t soundId = 0; // played at the muzzle by AudioSystem
    float soundGain = 0.5f;

    // MissileSystem's homing envelope. Zero turnRate means unguided, which is
    // what every gun round is.
    struct Guidance {
        double turnRate = 0.;
        double acceleration = 0.;
        double topSpeed = 0.;
    } guidance;

    [[nodiscard]] bool IsGuided() const { return guidance.turnRate > 0.; }
};

// What an upgrade does to the ship that collects it. The catalog is
// data-driven (data/upgrades.toml), but the effects themselves are not: each
// kind is a hand-written rule in the sim, and the file only supplies its
// magnitudes. Adding a genuinely new *behavior* means a new kind here plus
// the code that reads it; adding another tier, cost or magnitude of an
// existing behavior is a data edit alone.
enum class UpgradeKind : std::uint8_t {
    MissileRack, // rounds onto the rack -- the only repeatable kind
    FireRate,    // shorter cooldown, whichever weapon is fitted
    WeaponTier,  // fits the next weapon up its line
    Shield,      // a damage buffer in front of the hull
};

// Which shield a ship is carrying. Unlike the levels, this is a real choice:
// the two absorb differently (see UpgradeDef::shield), so collecting the
// other type replaces rather than stacks.
enum class ShieldType : std::uint8_t {
    None,
    Bubble,  // absorbs a hit whole, big reservoir, slow to come back
    Plating, // hull-hugging, leaks part of every hit, recharges fast
};

// Where a collected upgrade lands. Only Ship is honored today -- everything
// is carried by the ship that picked it up and lost with it. Faction is the
// hook for permanent faction-wide passives (a researched tech every future
// hull spawns with); nothing rolls it yet.
enum class UpgradeScope : std::uint8_t {
    Ship,
    Faction,
};

// One entry of the research pool, loaded from data/upgrades.toml.
struct UpgradeDef {
    id_t id = 0; // FNV of the toml key -- what the wire and the UI address it by
    std::string key;
    std::string name;
    std::string description;

    UpgradeKind kind = UpgradeKind::MissileRack;
    UpgradeScope scope = UpgradeScope::Ship;

    // How many times one ship can take it. 0 means unlimited -- a restock,
    // not a tier, so it can always be rolled.
    std::uint8_t maxLevel = 1;
    // Relative odds of appearing in a draft, among everything still eligible.
    float weight = 1.f;

    struct Rack {
        int perPickup = 0;
        int capacity = 0;
    } rack;

    struct FireRate {
        // Cooldown multiplier per level, compounded: 0.75 at level 2 is
        // 0.5625x the fitted weapon's cooldown.
        float cooldownScale = 1.f;
    } fireRate;

    // WeaponTier only: the weapon fitted at each level, in order. Level N
    // means tiers[N - 1], so maxLevel is bounded by this list's length.
    std::vector<id_t> tiers;

    struct Shield {
        ShieldType type = ShieldType::None;
        float capacity = 0.f;         // per level
        float regenPerSecond = 0.f;   // per level
        float regenDelaySeconds = 0.f; // quiet time after a hit before regen resumes
        // Share of an incoming hit the shield can eat; the remainder always
        // reaches the hull, however much charge is left.
        float absorbFraction = 1.f;
    } shield;
};

// A ship's collected tiers. Split out of ShipLoadout so the same block can
// later hang off a faction for permanent passives (UpgradeScope::Faction) and
// be resolved through the same UpgradeCatalog::ResolveStats.
struct UpgradeLevels {
    std::uint8_t fireRate = 0;
    std::uint8_t gunTier = 0;
    std::uint8_t shield = 0;
    ShieldType shieldType = ShieldType::None;
};

// UpgradeLevels resolved against the catalog into what the sim actually
// reads. Cheap enough to recompute wherever it's needed rather than cache and
// invalidate.
//
// `gun` and `missile` point into the catalog's own storage, which outlives
// every ship, and are null only when the file names a weapon that isn't in
// the table.
struct ShipStats {
    const WeaponDef* gun = nullptr;
    const WeaponDef* missile = nullptr;

    // The gun's own cooldown with the fire-rate tier applied; the missile's
    // cadence is not upgradable, so it is the weapon's unmodified value.
    std::uint32_t fireCooldownTicks = 1;
    std::uint32_t missileCooldownTicks = 1;
    int missileCapacity = 0;

    float shieldCapacity = 0.f;
    float shieldRegenPerSecond = 0.f;
    std::uint16_t shieldRegenDelayTicks = 0;
    float shieldAbsorbFraction = 1.f;
};

} // namespace Gravitaris
