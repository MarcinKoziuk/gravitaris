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

// What filling a magazine costs against what the thing feeding it cost to fit.
// Under 1 on purpose: rounds are the cheap part of owning a heavy weapon, and a
// pilot who keeps landing to rearm should still come out ahead of one who
// refits from scratch.
static constexpr float RESUPPLY_SHARE = 0.75f;

static UpgradeKind ParseKind(std::string_view name, bool& ok);
static ShieldType ParseShieldType(std::string_view name, bool& ok);
static AmmoPool ParseAmmoPool(std::string_view name, bool& ok);
static bool Exclusive(const UpgradeDef& a, const UpgradeDef& b);
static WeaponDef ParseWeapon(const toml::table& entry, const std::string& key);
static void ParseCostCurve(const toml::table& entry, const char* key,
                           std::uint16_t (&out)[MAX_UPGRADE_RANKS]);

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
            def.icon = (*entry)["icon"].value_or(key->substr(0, std::min<std::size_t>(3, key->size())));

            if (const toml::array* slots = (*entry)["slots"].as_array()) {
                for (const toml::node& slot : *slots) {
                    if (const auto name = slot.value<std::string>()) def.slots.push_back(*name);
                }
            }
            def.maxLevel = static_cast<std::uint8_t>(
                    std::min<std::size_t>((*entry)["max_level"].value_or<std::uint8_t>(1),
                                          MAX_UPGRADE_RANKS));
            def.weight = (*entry)["weight"].value_or(1.f);
            def.researched = (*entry)["research"].value_or(true);
            // Not named `requires` -- that is a keyword from C++20 on.
            if (const auto prereq = (*entry)["requires"].value<std::string>()) {
                def.requiresId = ID(prereq->c_str());
            }
            ParseCostCurve(*entry, "tech_cost", def.techCost);
            ParseCostCurve(*entry, "supply_cost", def.supplyCost);

            switch (def.kind) {
            case UpgradeKind::FireRate:
                def.fireRate.cooldownScale = (*entry)["cooldown_scale"].value_or(1.f);
                break;
            case UpgradeKind::CannonTier:
            case UpgradeKind::MissileTier:
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
            case UpgradeKind::AmmoStore: {
                const auto poolName = (*entry)["ammo_pool"].value<std::string>();
                def.ammo.pool = poolName ? ParseAmmoPool(*poolName, ok) : AmmoPool::None;
                if (!ok || def.ammo.pool == AmmoPool::None) {
                    LOG(error) << "upgrades: " << *key
                               << ": ammo entries need a cannon/missile `ammo_pool`; skipped";
                    continue;
                }
                def.ammo.capacity = (*entry)["capacity"].value_or(0);
                break;
            }
            case UpgradeKind::EngineTier:
                def.engine.thrustScale = (*entry)["thrust_scale"].value_or(1.f);
                def.engine.maxSpeedScale = (*entry)["max_speed_scale"].value_or(1.f);
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
                if (const toml::array* mends = (*entry)["hull_regen_seconds"].as_array()) {
                    for (const toml::node& seconds : *mends) {
                        def.shield.hullRegenSeconds.push_back(seconds.value_or(0.f));
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

            if (defs.size() == MAX_UPGRADE_DEFS) {
                LOG(error) << "upgrades: more than " << MAX_UPGRADE_DEFS
                           << " entries; the rest are ignored (TechUnlocks is indexed by this)";
                break;
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

    BuildLayout();

    LOG(info) << "upgrades: loaded " << m_weapons.size() << " weapons and " << m_defs.size()
              << " upgrades from " << path;
    return true;
}

// Column is the length of a def's prerequisite chain, row its order among the
// defs sharing that column. Walks the chain per def rather than sorting: the
// pool is a couple of dozen entries and this runs once, at load.
void UpgradeCatalog::BuildLayout()
{
    std::uint8_t nextRow[MAX_UPGRADE_DEFS] = {};
    m_treeColumns = 0;
    m_treeRows = 0;

    for (std::size_t i = 0; i < m_defs.size(); ++i) {
        std::uint8_t depth = 0;
        // Bounded one short of the pool size, so a `requires` cycle in a
        // hand-edited file stops rather than hanging the load -- and cannot
        // walk `depth` off the end of nextRow, which a full pool would.
        id_t prereq = m_defs[i].requiresId;
        for (std::size_t step = 0; prereq != 0 && step + 1 < m_defs.size(); ++step) {
            const UpgradeDef* def = Find(prereq);
            if (!def) break;
            ++depth;
            prereq = def->requiresId;
        }

        m_layout[i].col = depth;
        m_layout[i].row = nextRow[depth]++;
        m_treeColumns = std::max(m_treeColumns, static_cast<int>(depth) + 1);
        m_treeRows = std::max(m_treeRows, static_cast<int>(nextRow[depth]));
    }
}

UpgradeCatalog::TreeSlot UpgradeCatalog::SlotOf(std::size_t defIndex) const
{
    return defIndex < m_defs.size() ? m_layout[defIndex] : TreeSlot{};
}

std::size_t UpgradeCatalog::IndexOf(id_t id) const
{
    for (std::size_t i = 0; i < m_defs.size(); ++i) {
        if (m_defs[i].id == id) return i;
    }
    return MAX_UPGRADE_DEFS;
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

const UpgradeDef* UpgradeCatalog::FindAmmoStore(AmmoPool pool) const
{
    if (pool == AmmoPool::None) return nullptr;
    for (const UpgradeDef& def : m_defs) {
        if (def.kind == UpgradeKind::AmmoStore && def.ammo.pool == pool) return &def;
    }
    return nullptr;
}

ShipStats UpgradeCatalog::ResolveStats(const UpgradeLevels& levels) const
{
    ShipStats stats;

    // Spares from each locker the hull carries. Stowage is stowage: a box holds
    // its rounds whether or not the weapon they belong to is on the hull right
    // now, so a pilot can buy the two in either order. What a hull with nothing
    // to fire them through cannot do is *fill* them -- see FillableCapacity,
    // which is what the yard prices a resupply against.
    const auto sparesFor = [&](AmmoPool pool) {
        const UpgradeDef* def = FindAmmoStore(pool);
        const std::uint8_t rank = AmmoStoreRank(levels, pool);
        return def && rank > 0 ? def->ammo.capacity * static_cast<int>(rank) : 0;
    };
    const int cannonSpares = sparesFor(AmmoPool::Cannon);
    const int missileSpares = sparesFor(AmmoPool::Missile);

    stats.gun = FindWeapon(m_fittings.shipGun);
    // A gun tier fits a different weapon rather than scaling the stock one,
    // so what the ship fires is a lookup, not a curve.
    if (const UpgradeDef* def = FindKind(UpgradeKind::WeaponTier); def && levels.gunTier > 0) {
        const std::size_t index = std::min<std::size_t>(levels.gunTier, def->tiers.size()) - 1;
        if (const WeaponDef* tier = FindWeapon(def->tiers[index])) stats.gun = tier;
    }

    // No stock cannon either: the heavy mount is empty until one is fitted,
    // and the tier decides both the weapon and how deep its magazine is.
    stats.cannonCapacity = cannonSpares;
    if (const UpgradeDef* def = FindKind(UpgradeKind::CannonTier); def && levels.cannonTier > 0) {
        const std::size_t index = std::min<std::size_t>(levels.cannonTier, def->tiers.size()) - 1;
        stats.cannon = FindWeapon(def->tiers[index]);
        stats.cannonCapacity += def->rack.capacity * static_cast<int>(levels.cannonTier);
    }

    // No stock launcher: a hull that hasn't fitted a bay has no missile at all,
    // and both the round and the rack it goes in come from that bay's tier.
    stats.missileCapacity = missileSpares;
    if (const UpgradeDef* def = FindKind(UpgradeKind::MissileTier); def && levels.missileTier > 0) {
        const std::size_t index = std::min<std::size_t>(levels.missileTier, def->tiers.size()) - 1;
        stats.missile = FindWeapon(def->tiers[index]);
        stats.missileCapacity += def->rack.capacity * static_cast<int>(levels.missileTier);
    }
    if (stats.missile) stats.missileCooldownTicks = stats.missile->cooldownTicks;

    // The feed, on both primaries: the autoloader is a rule about how fast a
    // mount cycles, not about which of the two lines is in it, and its own
    // description has always said so.
    float feedScale = 1.f;
    if (const UpgradeDef* def = FindKind(UpgradeKind::FireRate)) {
        feedScale = std::pow(def->fireRate.cooldownScale, static_cast<float>(levels.fireRate));
    }
    const auto cycled = [feedScale](const WeaponDef* weapon) {
        const float cooldown = static_cast<float>(weapon ? weapon->cooldownTicks : 1u) * feedScale;
        return std::max(MIN_FIRE_COOLDOWN_TICKS, static_cast<std::uint32_t>(std::lround(cooldown)));
    };
    stats.fireCooldownTicks = cycled(stats.gun);
    if (stats.cannon) stats.cannonCooldownTicks = cycled(stats.cannon);

    if (const UpgradeDef* def = FindKind(UpgradeKind::EngineTier)) {
        if (levels.engine > 0) {
            // Rank 1 IS the motor the hull was drawn with, so it scales nothing:
            // the exponent counts ranks ABOVE stock. That keeps a hull's authored
            // [physics] thrust and max_speed the truth about how a stock one
            // flies, which is what every other number in the game is tuned
            // against -- and it is why the scales read as "per rank above stock".
            const auto above = static_cast<float>(levels.engine - 1);
            stats.thrustScale = std::pow(def->engine.thrustScale, above);
            stats.maxSpeedScale = std::pow(def->engine.maxSpeedScale, above);
        }
        else {
            // No drive, no acceleration. Rank 1 is issued to every faction and
            // fitted from spawn, so a hull reaching here is one that has had its
            // engine pulled at a yard -- it steers, coasts and falls, and cannot
            // add a metre per second of its own.
            stats.thrustScale = 0.f;
        }
    }

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
        if (!def->shield.hullRegenSeconds.empty()) {
            const std::size_t index =
                    std::min<std::size_t>(levels.shield, def->shield.hullRegenSeconds.size()) - 1;
            const float seconds = def->shield.hullRegenSeconds[index];
            if (seconds > 0.f) stats.hullRegenFractionPerSecond = 1.f / seconds;
        }
    }

    return stats;
}

std::uint16_t UpgradeCatalog::TechCostOf(const UpgradeDef& def, std::uint8_t rank)
{
    if (rank < 1 || rank > MAX_UPGRADE_RANKS) return 0;
    return def.techCost[rank - 1];
}

std::uint16_t UpgradeCatalog::SupplyCostOf(const UpgradeDef& def, std::uint8_t rank)
{
    if (rank < 1 || rank > MAX_UPGRADE_RANKS) return 0;
    return def.supplyCost[rank - 1];
}

std::uint8_t UpgradeCatalog::UnlockedRank(const UpgradeDef& def, const TechUnlocks& unlocked) const
{
    const std::size_t index = IndexOf(def.id);
    return index < m_defs.size() ? unlocked.rank[index] : std::uint8_t{0};
}

TechNodeState UpgradeCatalog::PermanentState(const UpgradeDef& def, std::uint8_t rank,
                                             const TechUnlocks& unlocked,
                                             std::uint32_t techPoints) const
{
    if (rank < 1 || rank > RankCount(def)) return TechNodeState::Held;
    // Stock hardware: there is nothing here for a faction to learn, so the
    // permanent board never offers it (and never draws it -- see GetTechTree).
    if (!def.researched) return TechNodeState::Held;

    const std::uint8_t held = UnlockedRank(def, unlocked);
    if (rank <= held) return TechNodeState::Held;
    // A ladder: the faction learns the ranks of a line in order, so the only
    // rank ever on offer is the one past what it holds.
    if (rank > held + 1) return TechNodeState::Locked;

    // A line is not researched before the thing it hangs off: no rounds before
    // there is a launcher to fire them from.
    if (def.requiresId != 0) {
        const UpgradeDef* prereq = Find(def.requiresId);
        if (!prereq || UnlockedRank(*prereq, unlocked) == 0) return TechNodeState::Locked;
    }

    if (techPoints < TechCostOf(def, rank)) return TechNodeState::Unaffordable;
    return TechNodeState::Available;
}

TechNodeState UpgradeCatalog::ShipState(const UpgradeDef& def, std::uint8_t rank,
                                        const ShipContext& context) const
{
    if (!context.loadout || !context.unlocked) return TechNodeState::Locked;
    const ShipLoadout& loadout = *context.loadout;
    const UpgradeLevels& levels = loadout.levels;

    if (rank < 1 || rank > RankCount(def)) return TechNodeState::Held;

    // The prerequisite has to be on *this* hull, not merely known to the side:
    // rounds are no use to a fighter with nothing to fire them from, however
    // well the faction understands them.
    //
    // Unless the two could never be aboard at once. Plating hangs off the
    // bubble, and fitting either *replaces* the other -- so a hull can never
    // hold a bubble at the moment it fits plating, and asking it to would lock
    // the line behind a state that cannot exist. For those the gate is what the
    // faction has learned, which is what a research prerequisite means anyway.
    //
    // A locker is gated the same way: the yard stocks shells for a weapon the
    // side has learned to build, so a pilot refitting from scratch buys the
    // rounds and the gun it feeds in one visit rather than having to land twice.
    if (def.requiresId != 0) {
        const UpgradeDef* prereq = Find(def.requiresId);
        if (!prereq) return TechNodeState::Locked;
        const bool researchGate =
                def.kind == UpgradeKind::AmmoStore || Exclusive(def, *prereq);
        const bool held = researchGate ? UnlockedRank(*prereq, *context.unlocked) > 0
                                       : LevelOf(*prereq, levels) > 0;
        if (!held) return TechNodeState::Locked;
    }

    // What the hull already carries is held, whatever the faction has since
    // learned -- and before the unlock gate, because a ship spawns with its
    // light guns fitted at a rank no side has researched yet. Reporting the
    // weapon in its mounts as unresearched would be a plain lie.
    //
    // For a line that goes into a mount or a bay, held is asked of *that* hole:
    // owning a rank is not the same as having it in the one being armed, and
    // reading it ship-wide would let a hull arm exactly one position with each
    // line and then refuse every other.
    //
    // Only the rank actually carried is held. The ones under it are still for
    // sale, because fitting one is a *downgrade* -- a lighter drive or a
    // smaller gun is a trade a pilot may want to make, and a rank is set rather
    // than climbed (see FitRank), so there is nothing in the way of it.
    //
    // Swapping to the other shield emitter is always on offer, at every
    // unlocked rank: it replaces rather than stacks, and swapping down is the
    // player's call to make.
    const bool swapping =
            def.kind == UpgradeKind::Shield && levels.shieldType != def.shield.type;
    if (!swapping && HeldInMount(def, loadout, context.mount) && LevelOf(def, levels) == rank) {
        return TechNodeState::Held;
    }

    // A rank the hull already carries is one it can carry again -- arming a
    // second mount with the guns already in the nose asks nothing new of the
    // faction. Only a rank above both gates is genuinely unresearched.
    //
    // Stock hardware skips the ceiling: a yard sells shells off the shelf, so
    // there is no unlocked rank for them to be under.
    if (def.researched && rank > UnlockedRank(def, *context.unlocked)
        && rank > LevelOf(def, levels)) {
        return TechNodeState::NotUnlocked;
    }

    if (context.supplies < SupplyCostOf(def, rank)) return TechNodeState::Unaffordable;
    if (!context.atLab) return TechNodeState::NeedsLanding;
    return TechNodeState::Available;
}

std::uint8_t UpgradeCatalog::LevelOf(const UpgradeDef& def, const UpgradeLevels& levels)
{
    switch (def.kind) {
    case UpgradeKind::FireRate:     return levels.fireRate;
    case UpgradeKind::WeaponTier:   return levels.gunTier;
    case UpgradeKind::CannonTier:   return levels.cannonTier;
    case UpgradeKind::MissileTier:  return levels.missileTier;
    // A box is a box: however many bays hold one, the rank on offer is still
    // rank I. Clamped rather than reported raw so the board's pips and its
    // "1/1" counter stay inside the ranks the def actually has.
    case UpgradeKind::AmmoStore:
        return std::min<std::uint8_t>(AmmoStoreRank(levels, def.ammo.pool), RankCount(def));
    case UpgradeKind::EngineTier:   return levels.engine;
    case UpgradeKind::Shield:       return levels.shieldType == def.shield.type ? levels.shield : 0;
    case UpgradeKind::Boost:        return levels.boost;
    }
    return 0;
}

bool UpgradeCatalog::UnlockRank(const UpgradeDef& def, std::uint8_t rank, TechUnlocks& unlocked,
                                std::uint32_t& techPoints) const
{
    if (PermanentState(def, rank, unlocked, techPoints) != TechNodeState::Available) return false;

    const std::size_t index = IndexOf(def.id);
    if (index >= m_defs.size()) return false;

    techPoints -= TechCostOf(def, rank);
    unlocked.rank[index] = rank;
    return true;
}

MountArm UpgradeCatalog::ArmOf(const UpgradeDef& def)
{
    switch (def.kind) {
    case UpgradeKind::WeaponTier: return MountArm::Light;
    case UpgradeKind::CannonTier: return MountArm::Heavy;
    default:                      return MountArm::None;
    }
}

SlotFamily UpgradeCatalog::FamilyOf(const UpgradeDef& def)
{
    if (ArmOf(def) != MountArm::None) return SlotFamily::Weapon;
    switch (def.kind) {
    case UpgradeKind::MissileTier: return SlotFamily::MissileBay;
    case UpgradeKind::AmmoStore:   return SlotFamily::AmmoBay;
    default:                       return SlotFamily::None;
    }
}

// How many holes of a family a hull can address. The stowage bays are narrower
// than everything else, and deliberately so -- see MAX_AMMO_BAYS.
static std::uint8_t HoleCount(SlotFamily family)
{
    return static_cast<std::uint8_t>(family == SlotFamily::AmmoBay ? MAX_AMMO_BAYS
                                                                   : MAX_WEAPON_MOUNTS);
}

bool UpgradeCatalog::HeldInMount(const UpgradeDef& def, const ShipLoadout& loadout,
                                 std::uint8_t mount)
{
    // Nothing that goes into a hole, or nobody asking about a particular one:
    // the ship-wide answer, which is what the branch view and the AI want.
    if (!IsMounted(def) || mount >= HoleCount(FamilyOf(def))) return true;

    switch (FamilyOf(def)) {
    case SlotFamily::MissileBay: return MissileBayFitted(loadout, mount);
    case SlotFamily::AmmoBay:    return loadout.ammoBays[mount] == def.ammo.pool;
    default:                     return loadout.mounts[mount] == ArmOf(def);
    }
}

// What a pool can hold on this hull as it stands, which is zero when nothing is
// mounted to fire from it -- a rank stays on the levels after its last mount
// comes off (StripRank), and rounds for a gun the hull cannot shoot are not
// something a yard should sell.
static int FillableCapacity(AmmoPool pool, const ShipLoadout& loadout, const ShipStats& stats)
{
    if (pool == AmmoPool::Cannon) {
        return MountsArmedWith(loadout, MountArm::Heavy) > 0 ? stats.cannonCapacity : 0;
    }
    return MissileBaysFitted(loadout) > 0 ? stats.missileCapacity : 0;
}

std::uint32_t UpgradeCatalog::ResupplyCost(const ShipLoadout& loadout) const
{
    const ShipStats stats = ResolveStats(loadout.levels);
    const UpgradeLevels& levels = loadout.levels;

    // What the hardware feeding one pool cost to fit: the weapon's own rank,
    // plus the locker's if this is the pool it deepens. Charging off that rather
    // than off a per-round price is what makes a heavier gun cost more to keep
    // fed without a second set of numbers to author and keep in step.
    const auto poolPrice = [&](AmmoPool pool) {
        std::uint32_t price = 0;
        const UpgradeKind weapon =
                pool == AmmoPool::Cannon ? UpgradeKind::CannonTier : UpgradeKind::MissileTier;
        const std::uint8_t weaponRank =
                pool == AmmoPool::Cannon ? levels.cannonTier : levels.missileTier;
        if (const UpgradeDef* def = FindKind(weapon); def && weaponRank > 0) {
            price += SupplyCostOf(*def, weaponRank);
        }
        // Per box, not per rank: the lockers are one rank apiece, and a hull
        // that stowed two of them paid twice.
        if (const std::uint8_t boxes = AmmoStoreRank(levels, pool); boxes > 0) {
            if (const UpgradeDef* def = FindAmmoStore(pool)) {
                price += static_cast<std::uint32_t>(SupplyCostOf(*def, 1)) * boxes;
            }
        }
        return price;
    };

    // Pro-rata for the share missing, and never free while anything is missing:
    // a single round back costs a supply, so there is no way to rearm for
    // nothing by landing with one shot left in the magazine.
    const auto poolCost = [&](AmmoPool pool, int have, int capacity) -> std::uint32_t {
        const int missing = capacity - have;
        if (capacity <= 0 || missing <= 0) return 0;
        const float full = static_cast<float>(poolPrice(pool)) * RESUPPLY_SHARE;
        const float share = static_cast<float>(missing) / static_cast<float>(capacity);
        return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(full * share)));
    };

    return poolCost(AmmoPool::Cannon, loadout.cannonAmmo,
                    FillableCapacity(AmmoPool::Cannon, loadout, stats))
         + poolCost(AmmoPool::Missile, loadout.missileAmmo,
                    FillableCapacity(AmmoPool::Missile, loadout, stats));
}

bool UpgradeCatalog::Resupply(ShipLoadout& loadout, std::uint32_t& supplies, bool atLab) const
{
    if (!atLab) return false;

    const std::uint32_t cost = ResupplyCost(loadout);
    if (cost == 0 || supplies < cost) return false;

    const ShipStats stats = ResolveStats(loadout.levels);
    loadout.cannonAmmo =
            static_cast<std::uint16_t>(FillableCapacity(AmmoPool::Cannon, loadout, stats));
    loadout.missileAmmo =
            static_cast<std::uint16_t>(FillableCapacity(AmmoPool::Missile, loadout, stats));
    supplies -= cost;
    return true;
}

void UpgradeCatalog::ClampAmmo(ShipLoadout& loadout) const
{
    const ShipStats stats = ResolveStats(loadout.levels);
    loadout.cannonAmmo = static_cast<std::uint16_t>(
            std::min<int>(loadout.cannonAmmo, stats.cannonCapacity));
    loadout.missileAmmo = static_cast<std::uint16_t>(
            std::min<int>(loadout.missileAmmo, stats.missileCapacity));
}

bool UpgradeCatalog::FitRank(const UpgradeDef& def, std::uint8_t rank, ShipLoadout& loadout,
                             const TechUnlocks& unlocked, std::uint32_t& supplies, bool atLab,
                             std::uint8_t mount) const
{
    const ShipContext context{&loadout, &unlocked, supplies, atLab, mount};
    if (ShipState(def, rank, context) != TechNodeState::Available) return false;

    UpgradeLevels& levels = loadout.levels;

    // A fitting that feeds a magazine arrives with that magazine full. The
    // alternative -- so many rounds per purchase -- left a pilot who bought
    // rank II holding rank I's load and having to buy a resupply on the same
    // visit, which is not a decision anyone was making.
    //
    // Resolved rather than authored: one number decides how deep a pool is,
    // and it is the same one that decides what goes in it.
    const auto fill = [&](AmmoPool pool) {
        const ShipStats fitted = ResolveStats(levels);
        if (pool == AmmoPool::Cannon) {
            loadout.cannonAmmo = static_cast<std::uint16_t>(fitted.cannonCapacity);
        }
        else {
            loadout.missileAmmo = static_cast<std::uint16_t>(fitted.missileCapacity);
        }
    };

    // Where it goes, for the lines that go somewhere. A pick with no hole
    // named takes the first free one, and failing that the first already
    // holding this line -- re-ranking what is there rather than refusing.
    if (IsMounted(def)) {
        const SlotFamily family = FamilyOf(def);
        const std::uint8_t holes = HoleCount(family);
        const auto occupied = [&](std::uint8_t i) {
            switch (family) {
            case SlotFamily::MissileBay: return MissileBayFitted(loadout, i);
            case SlotFamily::AmmoBay:    return loadout.ammoBays[i] != AmmoPool::None;
            default:                     return loadout.mounts[i] != MountArm::None;
            }
        };
        if (mount >= holes) {
            mount = TechPick::NO_MOUNT;
            for (std::uint8_t i = 0; i < holes; ++i) {
                if (!occupied(i)) { mount = i; break; }
            }
            for (std::uint8_t i = 0; mount == TechPick::NO_MOUNT && i < holes; ++i) {
                if (HeldInMount(def, loadout, i)) mount = i;
            }
        }
        // Nowhere to put it. Refused rather than charged for: the hull is full
        // of this family and there is nothing this purchase would change.
        if (mount >= holes) return false;

        switch (family) {
        case SlotFamily::MissileBay: SetMissileBay(loadout, mount, true); break;
        // Straight over whatever was in the bay: the two lockers are the same
        // hole's alternatives, so buying one where the other sat is a swap
        // rather than a refusal.
        case SlotFamily::AmmoBay:    loadout.ammoBays[mount] = def.ammo.pool; break;
        default:                     loadout.mounts[mount] = ArmOf(def); break;
        }
    }

    // Set, never increment: a supply price buys the rank named, and a hull
    // that skipped the ranks below it never paid for them.
    switch (def.kind) {
    case UpgradeKind::FireRate:
        levels.fireRate = rank;
        break;
    case UpgradeKind::WeaponTier:
        levels.gunTier = rank;
        break;
    case UpgradeKind::CannonTier:
        levels.cannonTier = rank;
        fill(AmmoPool::Cannon);
        break;
    case UpgradeKind::MissileTier:
        levels.missileTier = rank;
        fill(AmmoPool::Missile);
        break;
    case UpgradeKind::AmmoStore:
        // Both pools, not just this one: the bay this went into may have been
        // holding the other locker, whose spares have just gone with it.
        SyncAmmoStoreCounts(loadout);
        ClampAmmo(loadout);
        fill(def.ammo.pool);
        break;
    case UpgradeKind::EngineTier:
        levels.engine = rank;
        break;
    case UpgradeKind::Boost:
        levels.boost = rank;
        break;
    case UpgradeKind::Shield:
        // Switching type starts the new emitter empty rather than carrying the
        // old one's charge over -- they're different hardware.
        if (levels.shieldType != def.shield.type) {
            levels.shieldType = def.shield.type;
            loadout.shieldHp = 0.f;
            loadout.plates = {};
            loadout.plateRegenDelay = {};
        }
        levels.shield = rank;
        break;
    }

    supplies -= SupplyCostOf(def, rank);
    return true;
}

bool UpgradeCatalog::StripRank(const UpgradeDef& def, ShipLoadout& loadout, bool atLab,
                               std::uint8_t mount) const
{
    if (!atLab) return false;

    // A mounted line comes out hole by hole: a hull may carry it in three
    // places and be giving up one of them. The rank stays on the levels --
    // what it paid for, it has paid for -- so the mount can be re-armed at
    // that rank later without the faction having to know it.
    const SlotFamily family = FamilyOf(def);
    if (family != SlotFamily::None) {
        const std::uint8_t holes = HoleCount(family);
        // Nobody named a hole: the last one holding this line, so repeating
        // the call empties them one at a time from the outside in.
        for (std::uint8_t i = holes; i > 0 && mount >= holes; --i) {
            if (HeldInMount(def, loadout, static_cast<std::uint8_t>(i - 1))) {
                mount = static_cast<std::uint8_t>(i - 1);
            }
        }
        if (mount >= holes || !HeldInMount(def, loadout, mount)) return false;

        switch (family) {
        case SlotFamily::Weapon:
            loadout.mounts[mount] = MountArm::None;
            // The last heavy mount gone leaves nothing to fire the magazine
            // through, so its rounds go back to the yard with the gun.
            if (ArmOf(def) == MountArm::Heavy && MountsArmedWith(loadout, MountArm::Heavy) == 0) {
                loadout.cannonAmmo = 0;
            }
            break;
        case SlotFamily::AmmoBay:
            loadout.ammoBays[mount] = AmmoPool::None;
            SyncAmmoStoreCounts(loadout);
            ClampAmmo(loadout); // the spares in it go back to the yard with it
            break;
        default:
            SetMissileBay(loadout, mount, false);
            if (MissileBaysFitted(loadout) == 0) loadout.missileAmmo = 0;
            break;
        }
        return true;
    }

    UpgradeLevels& levels = loadout.levels;
    if (LevelOf(def, levels) == 0) return false;

    switch (def.kind) {
    case UpgradeKind::FireRate:
        levels.fireRate = 0;
        break;
    case UpgradeKind::EngineTier:
        levels.engine = 0;
        break;
    case UpgradeKind::Boost:
        levels.boost = 0;
        break;
    case UpgradeKind::Shield:
        levels.shield = 0;
        levels.shieldType = ShieldType::None;
        loadout.shieldHp = 0.f;
        loadout.plates = {};
        loadout.plateRegenDelay = {};
        break;
    // Mounted lines never reach here -- they came out above.
    case UpgradeKind::WeaponTier:
    case UpgradeKind::CannonTier:
    case UpgradeKind::MissileTier:
    case UpgradeKind::AmmoStore:
        return false;
    }
    return true;
}

// What each kind is worth to *this* hull right now. The shape of it is "cover
// a gap before deepening a strength": the first shield and the first rounds on
// an empty rack change what a ship can do at all, while a fourth cannon tier
// only makes it better at what it already does.
static float FitScore(const UpgradeDef& def, const ShipLoadout& loadout, int missileCapacity)
{
    const UpgradeLevels& levels = loadout.levels;
    const std::uint8_t level = UpgradeCatalog::LevelOf(def, levels);

    switch (def.kind) {
    case UpgradeKind::Shield:
        // Nothing keeps a fighter alive like the first shield; a swap to the
        // other type at the cost of the ranks already paid for is the one
        // thing a pilot will not do.
        if (levels.shieldType == ShieldType::None) return 100.f;
        if (levels.shieldType != def.shield.type) return 0.f;
        return 60.f - 10.f * static_cast<float>(level);
    case UpgradeKind::MissileTier:
        // The launcher is a weapon the hull does not otherwise have; the ranks
        // above it are only a better round.
        return level == 0 ? 75.f : 45.f - 5.f * static_cast<float>(level);
    case UpgradeKind::Boost:
        // The first one is mobility it simply lacked; past that it is only a
        // longer burn.
        return level == 0 ? 70.f : 30.f - 5.f * static_cast<float>(level);
    case UpgradeKind::WeaponTier:
        return 65.f - 5.f * static_cast<float>(level);
    case UpgradeKind::CannonTier:
        // The heavy mount is a weapon the hull does not otherwise have; the
        // ranks above it are a better round and a deeper magazine.
        return level == 0 ? 70.f : 40.f - 5.f * static_cast<float>(level);
    case UpgradeKind::FireRate:
        return 50.f - 5.f * static_cast<float>(level);
    // Nothing yet: an AI buys neither spares nor a better drive, so both score
    // below every line above and are never chosen. Deliberate for now -- see
    // docs/tech-tree-plan.md, which records what scoring these properly needs.
    case UpgradeKind::AmmoStore:
    case UpgradeKind::EngineTier:
        return 0.f;
    }
    return 0.f;
}

UpgradeCatalog::Choice UpgradeCatalog::PreferredFit(const ShipLoadout& loadout,
                                                    const ShipContext& context,
                                                    const FitWeights& weights) const
{
    const int capacity = ResolveStats(loadout.levels).missileCapacity;

    Choice best;
    float bestScore = 0.f;
    for (const UpgradeDef& def : m_defs) {
        // Highest affordable rank of a line first: an AI that can reach III
        // has no reason to pay for II on the way, since the price is absolute.
        // Never below what it already carries -- a downgrade is on offer to a
        // pilot weighing a trade, which is not a thing to be scored into.
        for (std::uint8_t rank = RankCount(def); rank > LevelOf(def, loadout.levels); --rank) {
            if (ShipState(def, rank, context) != TechNodeState::Available) continue;

            // Ties break toward the earlier def and the higher rank, so the
            // same hull in the same position always buys the same thing.
            const float value = FitScore(def, loadout, capacity) * weights.For(def.kind);
            if (best.def && value <= bestScore) break;
            best = Choice{&def, rank};
            bestScore = value;
            break;
        }
    }
    // A kind a pilot refuses outright scores zero, which is what a line it
    // already has its fill of scores too -- neither is worth a trip.
    return bestScore > 0.f ? best : Choice{};
}

UpgradeCatalog::Choice UpgradeCatalog::PreferredUnlock(const TechUnlocks& unlocked,
                                                       std::uint32_t budget) const
{
    // Breadth before depth: a side that has learned nothing of shields gains
    // far more from the first rank of them than from a third cannon. `weight`
    // is what the draft used to roll by and now orders the research plan.
    Choice best;
    float bestScore = 0.f;
    for (const UpgradeDef& def : m_defs) {
        const std::uint8_t rank = static_cast<std::uint8_t>(UnlockedRank(def, unlocked) + 1);
        if (PermanentState(def, rank, unlocked, budget) != TechNodeState::Available) continue;

        const float value = def.weight / static_cast<float>(rank);
        if (best.def && value <= bestScore) continue;
        best = Choice{&def, rank};
        bestScore = value;
    }
    return best;
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
        weapon.guidance.wobble = (*guidance)["wobble"].value_or(0.0);
    }
    return weapon;
}

static UpgradeKind ParseKind(std::string_view name, bool& ok)
{
    if (name == "fire_rate")      return UpgradeKind::FireRate;
    if (name == "weapon_tier")    return UpgradeKind::WeaponTier;
    if (name == "cannon_tier")    return UpgradeKind::CannonTier;
    if (name == "missile_tier")   return UpgradeKind::MissileTier;
    if (name == "ammo_store")     return UpgradeKind::AmmoStore;
    if (name == "engine_tier")    return UpgradeKind::EngineTier;
    if (name == "shield")         return UpgradeKind::Shield;
    if (name == "boost")          return UpgradeKind::Boost;
    ok = false;
    return UpgradeKind::FireRate; // unused: the caller drops the entry on !ok
}

// A scalar charges the same for every rank; a list charges per rank, with its
// last entry standing for every rank past its end -- so a three-rank line does
// not have to spell out prices for ranks it can never reach.
static void ParseCostCurve(const toml::table& entry, const char* key,
                           std::uint16_t (&out)[MAX_UPGRADE_RANKS])
{
    if (const auto flat = entry[key].value<std::uint16_t>()) {
        for (std::uint16_t& cost : out) cost = *flat;
        return;
    }

    const toml::array* curve = entry[key].as_array();
    if (!curve || curve->empty()) return;

    std::uint16_t last = 0;
    for (std::size_t i = 0; i < MAX_UPGRADE_RANKS; ++i) {
        if (i < curve->size()) {
            if (const auto v = curve->get(i)->value<std::uint16_t>()) last = *v;
        }
        out[i] = last;
    }
}

static ShieldType ParseShieldType(std::string_view name, bool& ok)
{
    if (name == "bubble")  return ShieldType::Bubble;
    if (name == "plating") return ShieldType::Plating;
    ok = false;
    return ShieldType::None;
}

// Whether these two are alternatives rather than companions: one hull hole and
// one field to hold the rank, so fitting either takes the other off. The two
// shield emitters are the only pair in the pool that work this way -- the ammo
// lockers used to, and now have a stowage slot each.
static bool Exclusive(const UpgradeDef& a, const UpgradeDef& b)
{
    if (a.kind != b.kind) return false;
    return a.kind == UpgradeKind::Shield;
}

static AmmoPool ParseAmmoPool(std::string_view name, bool& ok)
{
    if (name == "cannon")  return AmmoPool::Cannon;
    if (name == "missile") return AmmoPool::Missile;
    ok = false;
    return AmmoPool::None;
}

} // namespace Gravitaris
