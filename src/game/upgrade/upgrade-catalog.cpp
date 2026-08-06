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
static ShieldType ParseShieldType(std::string_view name, bool& ok);
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

    // No stock cannon either: the heavy mount is empty until one is fitted,
    // and the tier decides both the weapon and how deep its magazine is.
    if (const UpgradeDef* def = FindKind(UpgradeKind::CannonTier); def && levels.cannonTier > 0) {
        const std::size_t index = std::min<std::size_t>(levels.cannonTier, def->tiers.size()) - 1;
        stats.cannon = FindWeapon(def->tiers[index]);
        stats.cannonCapacity = def->rack.capacity * static_cast<int>(levels.cannonTier);
    }
    if (stats.cannon) stats.cannonCooldownTicks = stats.cannon->cooldownTicks;

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
    if (def.requiresId != 0) {
        const UpgradeDef* prereq = Find(def.requiresId);
        if (!prereq || LevelOf(*prereq, levels) == 0) return TechNodeState::Locked;
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
    // Swapping to the other shield emitter is always on offer, at every
    // unlocked rank: it replaces rather than stacks, and swapping down is the
    // player's call to make.
    const bool swapping = def.kind == UpgradeKind::Shield && levels.shieldType != def.shield.type;
    if (!swapping && HeldInMount(def, loadout, context.mount) && LevelOf(def, levels) >= rank) {
        return TechNodeState::Held;
    }

    // A rank the hull already carries is one it can carry again -- arming a
    // second mount with the guns already in the nose asks nothing new of the
    // faction. Only a rank above both gates is genuinely unresearched.
    if (rank > UnlockedRank(def, *context.unlocked) && rank > LevelOf(def, levels)) {
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
    return def.kind == UpgradeKind::MissileTier ? SlotFamily::MissileBay : SlotFamily::None;
}

bool UpgradeCatalog::HeldInMount(const UpgradeDef& def, const ShipLoadout& loadout,
                                 std::uint8_t mount)
{
    // Nothing that goes into a hole, or nobody asking about a particular one:
    // the ship-wide answer, which is what the branch view and the AI want.
    if (!IsMounted(def) || mount >= MAX_WEAPON_MOUNTS) return true;

    if (def.kind == UpgradeKind::MissileTier) return MissileBayFitted(loadout, mount);
    return loadout.mounts[mount] == ArmOf(def);
}

bool UpgradeCatalog::FitRank(const UpgradeDef& def, std::uint8_t rank, ShipLoadout& loadout,
                             const TechUnlocks& unlocked, std::uint32_t& supplies, bool atLab,
                             std::uint8_t mount) const
{
    const ShipContext context{&loadout, &unlocked, supplies, atLab, mount};
    if (ShipState(def, rank, context) != TechNodeState::Available) return false;

    UpgradeLevels& levels = loadout.levels;

    // Rounds are capped by the bay the ship has fitted, resolved rather than
    // authored on the restock: one number decides rack width, and it is the
    // one that also decides which round goes in it.
    const auto load = [&](int rounds) {
        loadout.missileAmmo = static_cast<std::uint8_t>(
                std::min(loadout.missileAmmo + rounds, ResolveStats(levels).missileCapacity));
    };

    // Where it goes, for the lines that go somewhere. A pick with no hole
    // named takes the first free one, and failing that the first already
    // holding this line -- re-ranking what is there rather than refusing.
    if (IsMounted(def)) {
        const MountArm arm = ArmOf(def);
        const auto occupied = [&](std::uint8_t i) {
            return arm != MountArm::None ? loadout.mounts[i] != MountArm::None
                                         : MissileBayFitted(loadout, i);
        };
        if (mount >= MAX_WEAPON_MOUNTS) {
            mount = TechPick::NO_MOUNT;
            for (std::uint8_t i = 0; i < MAX_WEAPON_MOUNTS; ++i) {
                if (!occupied(i)) { mount = i; break; }
            }
            for (std::uint8_t i = 0; mount == TechPick::NO_MOUNT && i < MAX_WEAPON_MOUNTS; ++i) {
                if (HeldInMount(def, loadout, i)) mount = i;
            }
        }
        if (mount < MAX_WEAPON_MOUNTS) {
            if (arm != MountArm::None) loadout.mounts[mount] = arm;
            else SetMissileBay(loadout, mount, true);
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
    case UpgradeKind::CannonTier: {
        levels.cannonTier = rank;
        // Arrives loaded: a cannon with an empty magazine is a purchase that
        // does nothing until the hull happens to sit at a yard.
        const int capacity = ResolveStats(levels).cannonCapacity;
        loadout.cannonAmmo = static_cast<std::uint8_t>(std::min(capacity, 255));
        break;
    }
    case UpgradeKind::MissileTier:
        levels.missileTier = rank;
        load(def.rack.perPickup); // the fitting comes with rounds in it
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
        // Nobody named a hole: the last one holding this line, so repeating
        // the call empties them one at a time from the outside in.
        for (std::uint8_t i = MAX_WEAPON_MOUNTS; i > 0 && mount >= MAX_WEAPON_MOUNTS; --i) {
            if (HeldInMount(def, loadout, static_cast<std::uint8_t>(i - 1))) {
                mount = static_cast<std::uint8_t>(i - 1);
            }
        }
        if (mount >= MAX_WEAPON_MOUNTS || !HeldInMount(def, loadout, mount)) return false;

        if (family == SlotFamily::Weapon) {
            loadout.mounts[mount] = MountArm::None;
            // The last heavy mount gone leaves nothing to fire the magazine
            // through, so its rounds go back to the yard with the gun.
            if (ArmOf(def) == MountArm::Heavy && MountsArmedWith(loadout, MountArm::Heavy) == 0) {
                loadout.cannonAmmo = 0;
            }
        }
        else {
            SetMissileBay(loadout, mount, false);
            if (MissileBaysFitted(loadout) == 0) loadout.missileAmmo = 0;
        }
        return true;
    }

    UpgradeLevels& levels = loadout.levels;
    if (LevelOf(def, levels) == 0) return false;

    switch (def.kind) {
    case UpgradeKind::FireRate:
        levels.fireRate = 0;
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
        return false;
    }
    return true;
}

// What each kind is worth to *this* hull right now. The shape of it is "cover
// a gap before deepening a strength": the first shield and the first rounds on
// an empty rack change what a ship can do at all, while a fourth cannon tier
// only makes it better at what it already does.
static int FitScore(const UpgradeDef& def, const ShipLoadout& loadout, int missileCapacity)
{
    const UpgradeLevels& levels = loadout.levels;
    const std::uint8_t level = UpgradeCatalog::LevelOf(def, levels);

    switch (def.kind) {
    case UpgradeKind::Shield:
        // Nothing keeps a fighter alive like the first shield; a swap to the
        // other type at the cost of the ranks already paid for is the one
        // thing a pilot will not do.
        if (levels.shieldType == ShieldType::None) return 100;
        if (levels.shieldType != def.shield.type) return 0;
        return 60 - 10 * static_cast<int>(level);
    case UpgradeKind::MissileTier:
        // The launcher is a weapon the hull does not otherwise have; the ranks
        // above it are only a better round.
        return level == 0 ? 75 : 45 - 5 * static_cast<int>(level);
    case UpgradeKind::Boost:
        // The first one is mobility it simply lacked; past that it is only a
        // longer burn.
        return level == 0 ? 70 : 30 - 5 * static_cast<int>(level);
    case UpgradeKind::WeaponTier:
        return 65 - 5 * static_cast<int>(level);
    case UpgradeKind::CannonTier:
        // The heavy mount is a weapon the hull does not otherwise have; the
        // ranks above it are a better round and a deeper magazine.
        return level == 0 ? 70 : 40 - 5 * static_cast<int>(level);
    case UpgradeKind::FireRate:
        return 50 - 5 * static_cast<int>(level);
    }
    return 0;
}

UpgradeCatalog::Choice UpgradeCatalog::PreferredFit(const ShipLoadout& loadout,
                                                    const ShipContext& context) const
{
    const int capacity = ResolveStats(loadout.levels).missileCapacity;

    Choice best;
    int bestScore = 0;
    for (const UpgradeDef& def : m_defs) {
        // Highest affordable rank of a line first: an AI that can reach III
        // has no reason to pay for II on the way, since the price is absolute.
        for (std::uint8_t rank = RankCount(def); rank >= 1; --rank) {
            if (ShipState(def, rank, context) != TechNodeState::Available) continue;

            // Ties break toward the earlier def and the higher rank, so the
            // same hull in the same position always buys the same thing.
            const int value = FitScore(def, loadout, capacity);
            if (best.def && value <= bestScore) break;
            best = Choice{&def, rank};
            bestScore = value;
            break;
        }
    }
    return best;
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
    }
    return weapon;
}

static UpgradeKind ParseKind(std::string_view name, bool& ok)
{
    if (name == "fire_rate")      return UpgradeKind::FireRate;
    if (name == "weapon_tier")    return UpgradeKind::WeaponTier;
    if (name == "cannon_tier")    return UpgradeKind::CannonTier;
    if (name == "missile_tier")   return UpgradeKind::MissileTier;
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

} // namespace Gravitaris
