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
            // Not named `requires` -- that is a keyword from C++20 on.
            if (const auto prereq = (*entry)["requires"].value<std::string>()) {
                def.requiresId = ID(prereq->c_str());
            }
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
                break;
            case UpgradeKind::FireRate:
                def.fireRate.cooldownScale = (*entry)["cooldown_scale"].value_or(1.f);
                break;
            case UpgradeKind::MissileTier:
                def.rack.perPickup = (*entry)["per_pickup"].value_or(0);
                def.rack.capacity = (*entry)["capacity"].value_or(0);
                [[fallthrough]];
            case UpgradeKind::WeaponTier: {
                const toml::array* tiers = (*entry)["tiers"].as_array();
                if (tiers) {
                    for (const toml::node& tier : *tiers) {
                        if (const auto name = tier.value<std::string>()) def.tiers.push_back(ID(name->c_str()));
                    }
                }
                if (def.tiers.empty()) {
                    LOG(error) << "upgrades: " << *key << ": a weapon line needs a `tiers` list; skipped";
                    continue;
                }
                // The list is what a level can actually index, so it -- not the
                // authored max_level -- is the real ceiling.
                def.maxLevel = static_cast<std::uint8_t>(
                        std::min<std::size_t>(def.maxLevel, def.tiers.size()));
                break;
            }
            case UpgradeKind::ResearchStock:
                def.stock.perLevel = (*entry)["per_level"].value_or(0);
                break;
            case UpgradeKind::Boost:
                def.boost.thrustScale = (*entry)["thrust_scale"].value_or(1.f);
                def.boost.maxSpeedScale = (*entry)["max_speed_scale"].value_or(1.f);
                def.boost.durationSeconds = (*entry)["duration_seconds"].value_or(0.f);
                def.boost.cooldownSeconds = (*entry)["cooldown_seconds"].value_or(0.f);
                def.boost.minCooldownSeconds = (*entry)["min_cooldown_seconds"].value_or(0.f);
                break;
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
                def.shield.leakChance = (*entry)["leak_chance"].value_or(0.f);
                if (const toml::array* leaks = (*entry)["leak_fraction"].as_array()) {
                    for (const toml::node& leak : *leaks) {
                        def.shield.leakFraction.push_back(leak.value_or(0.f));
                    }
                }
                if (def.shield.leakChance > 0.f && def.shield.leakFraction.empty()) {
                    LOG(error) << "upgrades: " << *key
                               << ": a leaking shield needs a `leak_fraction` per level; skipped";
                    continue;
                }
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
    require(m_fittings.turretWeapon, "[turret] weapon");
    for (const UpgradeDef& def : m_defs) {
        for (const id_t tier : def.tiers) require(tier, def.key.c_str());
        // A prerequisite that isn't in the pool would take its dependant off
        // the table for good rather than gating it.
        if (def.requiresId != 0 && !Find(def.requiresId)) {
            LOG(error) << "upgrades: " << def.key << " requires an upgrade that isn't in the pool";
        }
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

    stats.gun = FindWeapon(m_fittings.shipGun);
    // A gun tier fits a different weapon rather than scaling the stock one,
    // so what the ship fires is a lookup, not a curve.
    if (const UpgradeDef* def = FindKind(UpgradeKind::WeaponTier); def && levels.gunTier > 0) {
        const std::size_t index = std::min<std::size_t>(levels.gunTier, def->tiers.size()) - 1;
        if (const WeaponDef* tier = FindWeapon(def->tiers[index])) stats.gun = tier;
    }

    // No stock launcher: a hull that hasn't fitted a bay has no missile at all,
    // and both the round and the rack it goes in come from that bay's tier.
    if (const UpgradeDef* def = FindKind(UpgradeKind::MissileTier); def && levels.missileTier > 0) {
        const std::size_t index = std::min<std::size_t>(levels.missileTier, def->tiers.size()) - 1;
        stats.missile = FindWeapon(def->tiers[index]);
        stats.missileCapacity = def->rack.capacity * static_cast<int>(levels.missileTier);
    }
    if (stats.missile) stats.missileCooldownTicks = stats.missile->cooldownTicks;

    float cooldown = static_cast<float>(stats.gun ? stats.gun->cooldownTicks : 1u);
    if (const UpgradeDef* def = FindKind(UpgradeKind::FireRate)) {
        cooldown *= std::pow(def->fireRate.cooldownScale, static_cast<float>(levels.fireRate));
    }
    stats.fireCooldownTicks = std::max(MIN_FIRE_COOLDOWN_TICKS,
                                       static_cast<std::uint32_t>(std::lround(cooldown)));

    if (const UpgradeDef* def = FindKind(UpgradeKind::Boost); def && levels.boost > 0) {
        const auto level = static_cast<float>(levels.boost);
        stats.boostThrustScale = def->boost.thrustScale;
        stats.boostMaxSpeedScale = def->boost.maxSpeedScale;
        stats.boostTicks = static_cast<std::uint16_t>(
                std::lround(def->boost.durationSeconds * level / Game::PHYSICS_DELTA));
        // Levels shorten the wait rather than lengthening it, down to a floor
        // -- otherwise a second tier would be strictly worse between burns.
        const float cooldown = std::max(def->boost.minCooldownSeconds,
                                        def->boost.cooldownSeconds / level);
        stats.boostCooldownTicks =
                static_cast<std::uint16_t>(std::lround(cooldown / Game::PHYSICS_DELTA));
    }

    if (const UpgradeDef* def = FindKind(UpgradeKind::Shield, levels.shieldType); def && levels.shield > 0) {
        const auto level = static_cast<float>(levels.shield);
        stats.shieldCapacity = def->shield.capacity * level;
        stats.shieldRegenPerSecond = def->shield.regenPerSecond * level;
        stats.shieldRegenDelayTicks = static_cast<std::uint16_t>(
                std::lround(def->shield.regenDelaySeconds / Game::PHYSICS_DELTA));
        stats.shieldLeakChance = def->shield.leakChance;
        if (!def->shield.leakFraction.empty()) {
            const std::size_t index =
                    std::min<std::size_t>(levels.shield, def->shield.leakFraction.size()) - 1;
            stats.shieldLeakFraction = def->shield.leakFraction[index];
        }
    }

    return stats;
}

bool UpgradeCatalog::IsEligible(const UpgradeDef& def, const UpgradeLevels& levels) const
{
    if (def.requiresId != 0) {
        const UpgradeDef* prereq = Find(def.requiresId);
        if (!prereq || LevelOf(*prereq, levels) == 0) return false;
    }
    if (def.maxLevel == 0) return true; // a restock, always on the table
    return LevelOf(def, levels) < def.maxLevel;
}

int UpgradeCatalog::ResearchStockCapacity(const UpgradeLevels& levels, int base) const
{
    const UpgradeDef* def = FindKind(UpgradeKind::ResearchStock);
    if (!def) return base;
    return base + def->stock.perLevel * static_cast<int>(levels.researchStock);
}

UpgradeLevels UpgradeCatalog::Combined(const UpgradeLevels& ship, const UpgradeLevels& faction)
{
    UpgradeLevels levels = ship;
    levels.researchStock = faction.researchStock;
    return levels;
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

std::uint8_t UpgradeCatalog::PreferredOffer(const Offers& offers, const ShipLoadout& loadout,
                                            const UpgradeLevels& levels) const
{
    const int capacity = ResolveStats(levels).missileCapacity;

    // What each kind is worth to *this* hull right now. The shape of it is
    // "cover a gap before deepening a strength": the first shield and the
    // first rounds on an empty rack change what a ship can do at all, while a
    // fourth cannon tier only makes it better at what it already does.
    const auto score = [&](const UpgradeDef& def) {
        const std::uint8_t level = LevelOf(def, levels);
        switch (def.kind) {
        case UpgradeKind::Shield:
            // Nothing keeps a fighter alive like the first shield; a swap to
            // the other type at the cost of the tiers already paid for is the
            // one thing a pilot will not do.
            if (levels.shieldType == ShieldType::None) return 100;
            if (levels.shieldType != def.shield.type) return 0;
            return 60 - 10 * static_cast<int>(level);
        case UpgradeKind::MissileTier:
            // The launcher is a weapon the hull does not otherwise have; the
            // tiers above it are only a better round.
            return level == 0 ? 75 : 45 - 5 * static_cast<int>(level);
        case UpgradeKind::MissileRack:
            // An empty rack is a weapon the ship cannot use. A full one is
            // worth nothing at all, however tempting the card looks.
            if (loadout.missileAmmo == 0) return 80;
            return loadout.missileAmmo < capacity / 2 ? 40 : 5;
        case UpgradeKind::Boost:
            // The first one is mobility it simply lacked; past that it is
            // only a longer burn.
            return level == 0 ? 70 : 30 - 5 * static_cast<int>(level);
        case UpgradeKind::WeaponTier:
            return 65 - 5 * static_cast<int>(level);
        case UpgradeKind::FireRate:
            return 50 - 5 * static_cast<int>(level);
        case UpgradeKind::ResearchStock:
            // A queue only pays off for a side that leaves upgrades waiting,
            // and an AI wing lands constantly -- so it is the card a pilot
            // takes when the other two are worse, not one it goes for.
            return 15;
        }
        return 0;
    };

    std::uint8_t bestSlot = 0;
    int bestScore = 0;
    for (std::size_t i = 0; i < offers.size(); ++i) {
        const UpgradeDef* def = Find(offers[i]);
        if (!def || !IsEligible(*def, levels)) continue;

        // Ties break toward the earlier slot, so the same loadout offered the
        // same three cards always takes the same one.
        const int value = score(*def);
        if (bestSlot != 0 && value <= bestScore) continue;
        bestSlot = static_cast<std::uint8_t>(i + 1);
        bestScore = value;
    }
    return bestSlot;
}

std::uint8_t UpgradeCatalog::LevelOf(const UpgradeDef& def, const UpgradeLevels& levels)
{
    switch (def.kind) {
    case UpgradeKind::MissileRack:  return 0;
    case UpgradeKind::FireRate:     return levels.fireRate;
    case UpgradeKind::WeaponTier:   return levels.gunTier;
    case UpgradeKind::MissileTier:  return levels.missileTier;
    case UpgradeKind::Shield:       return levels.shieldType == def.shield.type ? levels.shield : 0;
    case UpgradeKind::Boost:        return levels.boost;
    case UpgradeKind::ResearchStock: return levels.researchStock;
    }
    return 0;
}

bool UpgradeCatalog::Apply(const UpgradeDef& def, ShipLoadout& loadout) const
{
    UpgradeLevels& levels = loadout.levels;
    if (def.scope != UpgradeScope::Ship || !IsEligible(def, levels)) return false;

    // Rounds are capped by the bay the ship has fitted, resolved rather than
    // authored on the restock: one number decides rack width, and it is the
    // one that also decides which round goes in it.
    const auto load = [&](int rounds) {
        loadout.missileAmmo = static_cast<std::uint8_t>(
                std::min(loadout.missileAmmo + rounds, ResolveStats(levels).missileCapacity));
    };

    switch (def.kind) {
    case UpgradeKind::MissileRack:
        load(def.rack.perPickup);
        return true;
    case UpgradeKind::FireRate:
        ++levels.fireRate;
        return true;
    case UpgradeKind::WeaponTier:
        ++levels.gunTier;
        return true;
    case UpgradeKind::MissileTier:
        ++levels.missileTier;
        load(def.rack.perPickup); // the fitting comes with rounds in it
        return true;
    case UpgradeKind::Boost:
        ++levels.boost;
        return true;
    case UpgradeKind::ResearchStock:
        return false; // faction scope -- ApplyFaction's business, not a hull's
    case UpgradeKind::Shield:
        // Switching type starts the new emitter at level 1 rather than
        // carrying the old one's tiers over -- they're different hardware.
        if (levels.shieldType != def.shield.type) {
            levels.shieldType = def.shield.type;
            levels.shield = 1;
            loadout.shieldHp = 0.f; // charges up from empty
            loadout.plates = {};
            loadout.plateRegenDelay = {};
        }
        else {
            ++levels.shield;
        }
        return true;
    }
    return false;
}

bool UpgradeCatalog::ApplyFaction(const UpgradeDef& def, UpgradeLevels& levels) const
{
    if (def.scope != UpgradeScope::Faction || !IsEligible(def, levels)) return false;

    if (def.kind == UpgradeKind::ResearchStock) {
        ++levels.researchStock;
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
    weapon.hardpoint = entry["hardpoint"].value_or(weapon.hardpoint);

    if (const toml::table* guidance = entry["guidance"].as_table()) {
        weapon.guidance.turnRate = (*guidance)["turn_rate"].value_or(0.0);
        weapon.guidance.acceleration = (*guidance)["acceleration"].value_or(0.0);
        weapon.guidance.topSpeed = (*guidance)["top_speed"].value_or(0.0);
    }
    return weapon;
}

static UpgradeKind ParseKind(std::string_view name, bool& ok)
{
    if (name == "missile_rack")   return UpgradeKind::MissileRack;
    if (name == "fire_rate")      return UpgradeKind::FireRate;
    if (name == "weapon_tier")    return UpgradeKind::WeaponTier;
    if (name == "missile_tier")   return UpgradeKind::MissileTier;
    if (name == "shield")         return UpgradeKind::Shield;
    if (name == "boost")          return UpgradeKind::Boost;
    if (name == "research_stock") return UpgradeKind::ResearchStock;
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
