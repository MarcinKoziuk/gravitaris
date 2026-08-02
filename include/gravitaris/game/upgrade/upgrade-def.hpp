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

    // Which family of mounts this leaves from: the model's `<hardpoint>_N`
    // markers (Body::FindMount), falling back to its gun mounts and then to
    // the hull's own center. Data rather than a rule over the weapon's stats,
    // so a new heavy line is a toml edit and the sim never learns weapon names.
    std::string hardpoint = "gun";

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
    MissileRack,  // rounds onto the rack -- the only repeatable kind
    FireRate,     // shorter cooldown, whichever weapon is fitted
    WeaponTier,   // fits the next weapon up its line
    MissileTier,  // fits the launcher, then the next round up, and widens the rack
    Shield,       // a damage buffer in front of the hull
    Boost,        // an overburn: more thrust, and briefly past the speed cap
    ResearchStock, // how many finished upgrades a faction can queue up
};

// Which shield a ship is carrying. Unlike the levels, this is a real choice:
// the two absorb differently (see UpgradeDef::shield), so collecting the
// other type replaces rather than stacks.
enum class ShieldType : std::uint8_t {
    None,
    Bubble,  // absorbs a hit whole, big reservoir, slow to come back
    Plating, // hull-hugging, leaks part of every hit, recharges fast
};

// Fixed width of everything indexed by ablative plate: ShipLoadout's per-plate
// charge, its wire form, and the renderer's per-instance charge word. A model
// authoring more '+plating' paths than this is clamped at load (Body::AddPlates),
// so the index a hit reports always addresses all three.
inline constexpr std::size_t MAX_SHIELD_PLATES = 16;

// Plate index meaning "the bubble", which is one element rather than one of an
// indexed array. Lives here, with the plate width it sits outside of, so both
// the physics shapes (PhysicsBody::SHIELD_BUBBLE) and the geometry query the
// client resolves its own hits with (QueryBodySegment) name the same value.
inline constexpr std::uint8_t SHIELD_BUBBLE_ELEMENT = 0xFF;

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

    // Another upgrade this one is locked behind: it stays off the table until
    // that one is at level 1. Zero means no prerequisite. This is what makes a
    // weapon line a line rather than a set of independent pickups -- rounds
    // are not offered to a hull with nothing to fire them from.
    id_t requiresId = 0;

    struct Rack {
        int perPickup = 0;
        int capacity = 0; // MissileTier: per level; MissileRack: unused (the fitted bay decides)
    } rack;

    struct FireRate {
        // Cooldown multiplier per level, compounded: 0.75 at level 2 is
        // 0.5625x the fitted weapon's cooldown.
        float cooldownScale = 1.f;
    } fireRate;

    // WeaponTier/MissileTier only: the weapon fitted at each level, in order.
    // Level N means tiers[N - 1], so maxLevel is bounded by this list's length.
    std::vector<id_t> tiers;

    // ResearchStock only: how much each level widens the faction's queue of
    // finished-but-uncollected upgrades, over economy.toml's base.
    struct Stock {
        int perLevel = 0;
    } stock;

    // Boost only. The whole point is stopping: a ship bearing down on a
    // planet gets the thrust to kill its speed in time, and the same button
    // buys a burst of closing or breaking speed in a fight. Levels lengthen
    // the burn and shorten the wait; the speed ceiling does not move with
    // them -- one number decides how fast a boosted hull can ever go.
    struct Boost {
        float thrustScale = 1.f;         // multiplier on the hull's own thrust
        float maxSpeedScale = 1.f;       // multiplier on the hull's own speed cap
        float durationSeconds = 0.f;     // per level
        float cooldownSeconds = 0.f;     // per level, floored at minCooldownSeconds
        float minCooldownSeconds = 0.f;
    } boost;

    struct Shield {
        ShieldType type = ShieldType::None;
        float capacity = 0.f;         // per level
        float regenPerSecond = 0.f;   // per level
        float regenDelaySeconds = 0.f; // quiet time after a hit before regen resumes
        // How often a hit gets through at all, however much charge is left.
        // Zero is an emitter that stops everything until it is spent (the
        // bubble); the plates trade that for a bigger, faster reservoir.
        float leakChance = 0.f;
        // Share of a leaking hit that reaches the hull, one entry per level --
        // a deeper stack of plates leaks less of each round that gets through,
        // not fewer of them.
        std::vector<float> leakFraction;
    } shield;
};

// A ship's collected tiers. Split out of ShipLoadout so the same block can
// later hang off a faction for permanent passives (UpgradeScope::Faction) and
// be resolved through the same UpgradeCatalog::ResolveStats.
struct UpgradeLevels {
    std::uint8_t fireRate = 0;
    std::uint8_t gunTier = 0;
    // Zero means the hull carries no launcher at all, not a stock one: there
    // is nothing to fire and nowhere to put rounds until the bay is fitted.
    std::uint8_t missileTier = 0;
    std::uint8_t shield = 0;
    ShieldType shieldType = ShieldType::None;
    std::uint8_t boost = 0;
    // UpgradeScope::Faction, so this one lives on FactionState's own block
    // rather than a ship's -- see ResearchSystem.
    std::uint8_t researchStock = 0;
};

// UpgradeLevels resolved against the catalog into what the sim actually
// reads. Cheap enough to recompute wherever it's needed rather than cache and
// invalidate.
//
// `gun` and `missile` point into the catalog's own storage, which outlives
// every ship. `missile` is null on a hull that has not fitted a bay, which is
// most of them -- there is no stock launcher.
struct ShipStats {
    const WeaponDef* gun = nullptr;
    const WeaponDef* missile = nullptr;

    // The gun's own cooldown with the fire-rate tier applied; the missile's is
    // the fitted round's own, since its tier already carries the cadence.
    std::uint32_t fireCooldownTicks = 1;
    std::uint32_t missileCooldownTicks = 1;
    // Rack width, which the bay's tier decides -- zero on a hull without one.
    int missileCapacity = 0;

    float shieldCapacity = 0.f;
    float shieldRegenPerSecond = 0.f;
    std::uint16_t shieldRegenDelayTicks = 0;
    // A hit leaks this often, and when it does this much of it reaches the
    // hull. Both zero on an emitter that stops everything it has charge for.
    float shieldLeakChance = 0.f;
    float shieldLeakFraction = 0.f;

    // Zero boostTicks means the ship isn't carrying the upgrade at all, which
    // is what ShipControlsSystem tests before granting a burn -- the scales
    // below are 1 in that case, so an unboosted hull needs no special case.
    std::uint16_t boostTicks = 0;
    std::uint16_t boostCooldownTicks = 0;
    float boostThrustScale = 1.f;
    float boostMaxSpeedScale = 1.f;
};

} // namespace Gravitaris
