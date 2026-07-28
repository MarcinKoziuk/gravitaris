#include <algorithm>
#include <cmath>
#include <string>

#include <toml++/toml.h>

#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>

namespace Gravitaris {

// However fast the gun gets, two ticks between shots is the floor: one would
// make the cadence the tick rate itself.
static constexpr std::uint32_t MIN_FIRE_COOLDOWN_TICKS = 2;

static UpgradeKind ParseKind(std::string_view name, bool& ok);
static UpgradeScope ParseScope(std::string_view name, bool& ok);
static ShieldType ParseShieldType(std::string_view name, bool& ok);
static WeaponDef ParseWeapon(const toml::table& entry, const std::string& key);
static std::uint32_t NextRandom(std::uint32_t& state);

bool UpgradeCatalog::Load(IFilesystem& filesystem, const char* path)
{
    std::string text;
    if (!filesystem.ReadString(std::string(path), &text)) {
        LOG(warning) << "upgrades: " << path << " not found; using built-in defaults";
        return false;
    }

    toml::table root;
    try {
        root = toml::parse(text, std::string(path));
    }
    catch (const toml::parse_error& error) {
        LOG(error) << "upgrades: " << path << ": " << error.description();
        return false;
    }

    std::vector<WeaponDef> weapons;
    if (const toml::array* entries = root["weapon"].as_array()) {
        for (const toml::node& node : *entries) {
            const toml::table* entry = node.as_table();
            if (!entry) continue;

            const auto key = (*entry)["key"].value<std::string>();
            if (!key) {
                LOG(error) << "upgrades: a [[weapon]] entry is missing `key`; skipped";
                continue;
            }
            weapons.push_back(ParseWeapon(*entry, *key));
        }
    }
    if (weapons.empty()) {
        LOG(warning) << "upgrades: " << path << " has no [[weapon]] entries";
        return false;
    }

    Fittings fittings;
    if (const toml::table* ship = root["ship"].as_table()) {
        if (const auto v = (*ship)["gun"].value<std::string>()) fittings.shipGun = ID(v->c_str());
        if (const auto v = (*ship)["missile"].value<std::string>()) fittings.shipMissile = ID(v->c_str());
        if (const auto v = (*ship)["missile_capacity"].value<int>()) fittings.missileCapacity = *v;
    }
    if (const toml::table* turret = root["turret"].as_table()) {
        if (const auto v = (*turret)["weapon"].value<std::string>()) fittings.turretWeapon = ID(v->c_str());
        if (const auto v = (*turret)["fire_range"].value<double>()) fittings.turretFireRange = *v;
        if (const auto v = (*turret)["fire_cooldown_ticks"].value<std::uint32_t>()) {
            fittings.turretFireCooldownTicks = *v;
        }
    }

    std::vector<UpgradeDef> defs;
    if (const toml::array* entries = root["upgrade"].as_array()) {
        for (const toml::node& node : *entries) {
            const toml::table* entry = node.as_table();
            if (!entry) continue;

            const auto key = (*entry)["key"].value<std::string>();
            const auto kindName = (*entry)["kind"].value<std::string>();
            if (!key || !kindName) {
                LOG(error) << "upgrades: an entry is missing `key` or `kind`; skipped";
                continue;
            }

            bool ok = true;
            UpgradeDef def;
            def.kind = ParseKind(*kindName, ok);
            if (!ok) {
                LOG(error) << "upgrades: " << *key << ": unknown kind `" << *kindName << "`; skipped";
                continue;
            }

            def.key = *key;
            def.id = ID(key->c_str());
            def.name = (*entry)["name"].value_or(*key);
            def.description = (*entry)["description"].value_or("");
            def.maxLevel = (*entry)["max_level"].value_or<std::uint8_t>(1);
            def.weight = (*entry)["weight"].value_or(1.f);
            if (const auto scopeName = (*entry)["scope"].value<std::string>()) {
                def.scope = ParseScope(*scopeName, ok);
                if (!ok) {
                    LOG(error) << "upgrades: " << *key << ": unknown scope `" << *scopeName << "`; skipped";
                    continue;
                }
            }

            switch (def.kind) {
            case UpgradeKind::MissileRack:
                def.rack.perPickup = (*entry)["per_pickup"].value_or(0);
                def.rack.capacity = (*entry)["capacity"].value_or(fittings.missileCapacity);
                break;
            case UpgradeKind::FireRate:
                def.fireRate.cooldownScale = (*entry)["cooldown_scale"].value_or(1.f);
                break;
            case UpgradeKind::WeaponTier: {
                const toml::array* tiers = (*entry)["tiers"].as_array();
                if (tiers) {
                    for (const toml::node& tier : *tiers) {
                        if (const auto name = tier.value<std::string>()) def.tiers.push_back(ID(name->c_str()));
                    }
                }
                if (def.tiers.empty()) {
                    LOG(error) << "upgrades: " << *key << ": weapon_tier needs a `tiers` list; skipped";
                    continue;
                }
                // The list is what a level can actually index, so it -- not the
                // authored max_level -- is the real ceiling.
                def.maxLevel = static_cast<std::uint8_t>(
                        std::min<std::size_t>(def.maxLevel, def.tiers.size()));
                break;
            }
            case UpgradeKind::Shield: {
                const auto typeName = (*entry)["shield_type"].value<std::string>();
                def.shield.type = typeName ? ParseShieldType(*typeName, ok) : ShieldType::None;
                if (!ok || def.shield.type == ShieldType::None) {
                    LOG(error) << "upgrades: " << *key << ": shield entries need a bubble/plating `shield_type`; skipped";
                    continue;
                }
                def.shield.capacity = (*entry)["capacity"].value_or(0.f);
                def.shield.regenPerSecond = (*entry)["regen_per_second"].value_or(0.f);
                def.shield.regenDelaySeconds = (*entry)["regen_delay_seconds"].value_or(0.f);
                def.shield.absorbFraction = (*entry)["absorb_fraction"].value_or(1.f);
                break;
            }
            }

            defs.push_back(std::move(def));
        }
    }

    m_weapons = std::move(weapons);
    m_defs = std::move(defs);
    m_fittings = fittings;

    // A weapon named by a fitting or a tier but absent from the table would
    // otherwise surface much later as a ship that silently cannot shoot.
    const auto require = [&](id_t id, const char* what) {
        if (id != 0 && !FindWeapon(id)) LOG(error) << "upgrades: " << what << " names an unknown weapon";
    };
    require(m_fittings.shipGun, "[ship] gun");
    require(m_fittings.shipMissile, "[ship] missile");
    require(m_fittings.turretWeapon, "[turret] weapon");
    for (const UpgradeDef& def : m_defs) {
        for (const id_t tier : def.tiers) require(tier, def.key.c_str());
    }

    LOG(info) << "upgrades: loaded " << m_weapons.size() << " weapons and " << m_defs.size()
              << " upgrades from " << path;
    return true;
}

const UpgradeDef* UpgradeCatalog::Find(id_t id) const
{
    for (const UpgradeDef& def : m_defs) {
        if (def.id == id) return &def;
    }
    return nullptr;
}

const WeaponDef* UpgradeCatalog::FindWeapon(id_t id) const
{
    for (const WeaponDef& weapon : m_weapons) {
        if (weapon.id == id) return &weapon;
    }
    return nullptr;
}

const UpgradeDef* UpgradeCatalog::FindKind(UpgradeKind kind, ShieldType shieldType) const
{
    for (const UpgradeDef& def : m_defs) {
        if (def.kind != kind) continue;
        if (kind == UpgradeKind::Shield && def.shield.type != shieldType) continue;
        return &def;
    }
    return nullptr;
}

ShipStats UpgradeCatalog::ResolveStats(const UpgradeLevels& levels) const
{
    ShipStats stats;
    stats.missileCapacity = m_fittings.missileCapacity;

    stats.gun = FindWeapon(m_fittings.shipGun);
    // A gun tier fits a different weapon rather than scaling the stock one,
    // so what the ship fires is a lookup, not a curve.
    if (const UpgradeDef* def = FindKind(UpgradeKind::WeaponTier); def && levels.gunTier > 0) {
        const std::size_t index = std::min<std::size_t>(levels.gunTier, def->tiers.size()) - 1;
        if (const WeaponDef* tier = FindWeapon(def->tiers[index])) stats.gun = tier;
    }

    stats.missile = FindWeapon(m_fittings.shipMissile);
    if (stats.missile) stats.missileCooldownTicks = stats.missile->cooldownTicks;

    float cooldown = static_cast<float>(stats.gun ? stats.gun->cooldownTicks : 1u);
    if (const UpgradeDef* def = FindKind(UpgradeKind::FireRate)) {
        cooldown *= std::pow(def->fireRate.cooldownScale, static_cast<float>(levels.fireRate));
    }
    stats.fireCooldownTicks = std::max(MIN_FIRE_COOLDOWN_TICKS,
                                       static_cast<std::uint32_t>(std::lround(cooldown)));

    if (const UpgradeDef* def = FindKind(UpgradeKind::Shield, levels.shieldType); def && levels.shield > 0) {
        const auto level = static_cast<float>(levels.shield);
        stats.shieldCapacity = def->shield.capacity * level;
        stats.shieldRegenPerSecond = def->shield.regenPerSecond * level;
        stats.shieldRegenDelayTicks = static_cast<std::uint16_t>(
                std::lround(def->shield.regenDelaySeconds / Game::PHYSICS_DELTA));
        stats.shieldAbsorbFraction = def->shield.absorbFraction;
    }

    return stats;
}

bool UpgradeCatalog::IsEligible(const UpgradeDef& def, const UpgradeLevels& levels) const
{
    if (def.maxLevel == 0) return true; // a restock, always on the table
    return LevelOf(def, levels) < def.maxLevel;
}

UpgradeCatalog::Offers UpgradeCatalog::RollOffers(const UpgradeLevels& levels, std::uint32_t seed) const
{
    Offers offers{};

    std::vector<const UpgradeDef*> pool;
    pool.reserve(m_defs.size());
    for (const UpgradeDef& def : m_defs) {
        if (def.weight > 0.f && IsEligible(def, levels)) pool.push_back(&def);
    }

    std::uint32_t state = seed | 1u;
    for (std::size_t slot = 0; slot < OFFER_COUNT && !pool.empty(); ++slot) {
        float total = 0.f;
        for (const UpgradeDef* def : pool) total += def->weight;

        // 24 bits of the generator as a 0..1 fraction; the pool is a handful
        // of entries, so that's resolution to spare.
        const float roll = total * static_cast<float>(NextRandom(state) & 0xffffffu) / 16777216.f;

        float running = 0.f;
        std::size_t picked = pool.size() - 1;
        for (std::size_t i = 0; i < pool.size(); ++i) {
            running += pool[i]->weight;
            if (roll < running) {
                picked = i;
                break;
            }
        }

        offers[slot] = pool[picked]->id;
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(picked)); // distinct offers
    }

    return offers;
}

std::uint8_t UpgradeCatalog::LevelOf(const UpgradeDef& def, const UpgradeLevels& levels)
{
    switch (def.kind) {
    case UpgradeKind::MissileRack: return 0;
    case UpgradeKind::FireRate:    return levels.fireRate;
    case UpgradeKind::WeaponTier:  return levels.gunTier;
    case UpgradeKind::Shield:      return levels.shieldType == def.shield.type ? levels.shield : 0;
    }
    return 0;
}

bool UpgradeCatalog::Apply(const UpgradeDef& def, ShipLoadout& loadout) const
{
    UpgradeLevels& levels = loadout.levels;
    if (!IsEligible(def, levels)) return false;

    switch (def.kind) {
    case UpgradeKind::MissileRack:
        loadout.missileAmmo = static_cast<std::uint8_t>(
                std::min<int>(loadout.missileAmmo + def.rack.perPickup, def.rack.capacity));
        return true;
    case UpgradeKind::FireRate:
        ++levels.fireRate;
        return true;
    case UpgradeKind::WeaponTier:
        ++levels.gunTier;
        return true;
    case UpgradeKind::Shield:
        // Switching type starts the new emitter at level 1 rather than
        // carrying the old one's tiers over -- they're different hardware.
        if (levels.shieldType != def.shield.type) {
            levels.shieldType = def.shield.type;
            levels.shield = 1;
            loadout.shieldHp = 0.f; // charges up from empty
        }
        else {
            ++levels.shield;
        }
        return true;
    }
    return false;
}

static WeaponDef ParseWeapon(const toml::table& entry, const std::string& key)
{
    WeaponDef weapon;
    weapon.key = key;
    weapon.id = ID(key.c_str());
    weapon.name = entry["name"].value_or(key);
    weapon.cooldownTicks = entry["cooldown_ticks"].value_or<std::uint32_t>(1);
    weapon.damage = entry["damage"].value_or(0.f);
    weapon.speed = entry["speed"].value_or(0.0);
    weapon.lifetimeSeconds = entry["lifetime_seconds"].value_or(0.0);
    if (const auto model = entry["model"].value<std::string>()) weapon.modelId = ID(model->c_str());
    if (const auto sound = entry["sound"].value<std::string>()) weapon.soundId = ID(sound->c_str());
    weapon.soundGain = entry["sound_gain"].value_or(weapon.soundGain);

    if (const toml::table* guidance = entry["guidance"].as_table()) {
        weapon.guidance.turnRate = (*guidance)["turn_rate"].value_or(0.0);
        weapon.guidance.acceleration = (*guidance)["acceleration"].value_or(0.0);
        weapon.guidance.topSpeed = (*guidance)["top_speed"].value_or(0.0);
    }
    return weapon;
}

static UpgradeKind ParseKind(std::string_view name, bool& ok)
{
    if (name == "missile_rack") return UpgradeKind::MissileRack;
    if (name == "fire_rate")    return UpgradeKind::FireRate;
    if (name == "weapon_tier")  return UpgradeKind::WeaponTier;
    if (name == "shield")       return UpgradeKind::Shield;
    ok = false;
    return UpgradeKind::MissileRack;
}

static UpgradeScope ParseScope(std::string_view name, bool& ok)
{
    if (name == "ship")    return UpgradeScope::Ship;
    if (name == "faction") return UpgradeScope::Faction;
    ok = false;
    return UpgradeScope::Ship;
}

static ShieldType ParseShieldType(std::string_view name, bool& ok)
{
    if (name == "bubble")  return ShieldType::Bubble;
    if (name == "plating") return ShieldType::Plating;
    ok = false;
    return ShieldType::None;
}

// xorshift32. Sim-visible randomness has to be reproducible from state alone
// (ADR 0001), so the caller supplies the seed and nothing here is global.
static std::uint32_t NextRandom(std::uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

} // namespace Gravitaris
