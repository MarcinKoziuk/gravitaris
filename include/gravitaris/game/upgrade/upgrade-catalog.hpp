#pragma once

#include <cstdint>
#include <vector>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/upgrade/upgrade-def.hpp>

namespace Gravitaris {

// The weapon table, the tech pool, and the ship stats they feed, parsed from
// data/upgrades.toml. One instance lives on Game; CGame reads the same one for
// the tech tree's names and the audio system's fire sounds.
//
// A missing or malformed file is not fatal: the built-in defaults below stand
// in, so the sim always has a base gun and an empty pool rather than a ship
// that cannot shoot.
// Why a rank cannot be taken right now, or that it can. Reported per rank
// rather than per node: the ship tree prices every rank separately and sells
// them individually, so "III is out of reach but II is not" is a thing one
// node has to be able to say.
enum class TechNodeState : std::uint8_t {
    Locked,       // the prerequisite is not unlocked (permanent) / not fitted (ship)
    Held,         // already unlocked, or already fitted at this rank or better
    NotUnlocked,  // ship only: above what the faction has learned
    Unaffordable, // eligible, but the pool is short
    NeedsLanding, // ship only: affordable, but the hull is not at a lab
    Available,
};

class UpgradeCatalog {
public:
    // Where a hull stands and what it can spend, which is everything the ship
    // tree needs beyond the loadout itself. Grouped so the query signatures
    // stay short rather than growing a parameter per rule.
    struct ShipContext {
        const ShipLoadout* loadout = nullptr;
        const TechUnlocks* unlocked = nullptr;
        std::uint32_t supplies = 0;
        bool atLab = false;
        // Which mount the question is about, for the lines that go into one.
        // "Already held" is a property of a mount, not of the ship: a hull with
        // light guns in the nose can still buy a second set for a wing, and
        // asking ship-wide would report every mount as already carrying them.
        // NO_MOUNT asks the ship-wide question, which is what the branch view
        // and the AI want.
        std::uint8_t mount = TechPick::NO_MOUNT;
    };

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

    // A def's own position in Defs(), which is what TechUnlocks is indexed by.
    // MAX_UPGRADE_DEFS for an id that isn't in the pool.
    [[nodiscard]] std::size_t IndexOf(id_t id) const;

    // How far this def can be taken. maxLevel 0 marks a repeatable restock,
    // which the faction still has to learn once and a hull can then buy over
    // and over -- so it is one rank on either track, not none.
    [[nodiscard]] static std::uint8_t RankCount(const UpgradeDef& def)
    { return def.maxLevel == 0 ? std::uint8_t{1} : def.maxLevel; }

    // What this rank costs on each track. Zero rank, or one past the end,
    // reads as free rather than as an error -- the state queries below are
    // what reject an out-of-range rank.
    [[nodiscard]] static std::uint16_t TechCostOf(const UpgradeDef& def, std::uint8_t rank);
    [[nodiscard]] static std::uint16_t SupplyCostOf(const UpgradeDef& def, std::uint8_t rank);

    // The rank a hull has fitted of `def`, for the tree's pips.
    [[nodiscard]] static std::uint8_t LevelOf(const UpgradeDef& def, const UpgradeLevels& levels);

    // The rank the faction has learned of `def`, which is the ceiling on what
    // any of its hulls may fit.
    [[nodiscard]] std::uint8_t UnlockedRank(const UpgradeDef& def, const TechUnlocks& unlocked) const;

    // Whether the faction may learn `rank` of `def` next, and why not. The
    // permanent tree is a ladder, so the only rank it ever offers is one past
    // what it holds.
    [[nodiscard]] TechNodeState PermanentState(const UpgradeDef& def, std::uint8_t rank,
                                               const TechUnlocks& unlocked,
                                               std::uint32_t techPoints) const;

    // Whether this hull may fit `rank` of `def`, and why not. The ship tree is
    // a menu: any unlocked rank above what is fitted can be bought outright at
    // its own price. A shield of the *other* type is the one exception -- every
    // unlocked rank of it is offered, since taking it swaps rather than stacks
    // and swapping down is the player's call to make.
    [[nodiscard]] TechNodeState ShipState(const UpgradeDef& def, std::uint8_t rank,
                                          const ShipContext& context) const;

    // Learns one rank for the faction, spending from `techPoints`. False when
    // the state above isn't Available, which leaves the pool untouched so a
    // stale click costs nothing.
    bool UnlockRank(const UpgradeDef& def, std::uint8_t rank, TechUnlocks& unlocked,
                    std::uint32_t& techPoints) const;

    // Fits one rank onto `loadout`, spending from `supplies`. Sets the rank
    // outright rather than incrementing it: a supply price buys the rank
    // named, not the next step up.
    //
    // `mount` is which weapon mount a gun or cannon is going into; the rank is
    // the ship's either way, but where it sits is not. Ignored by everything
    // that isn't mounted (a shield, the overburn), and TechPick::NO_MOUNT on a
    // weapon line arms the first free mount -- what the cheat console and the
    // AI want, neither of which has a schematic in front of it.
    bool FitRank(const UpgradeDef& def, std::uint8_t rank, ShipLoadout& loadout,
                 const TechUnlocks& unlocked, std::uint32_t& supplies, bool atLab,
                 std::uint8_t mount = TechPick::NO_MOUNT) const;

    // Takes a fitting back off the hull. No refund: a yard will pull a part
    // for you, it will not buy it back.
    //
    // What the hull has *paid* for stays on its UpgradeLevels, so the same
    // rank can be fitted again at its own price rather than needing the
    // faction to have researched it -- which matters most for the stock light
    // guns, fitted at a rank no side starts out knowing.
    //
    // `mount` names the hole for a mounted line, and NO_MOUNT empties the last
    // one holding it, so repeating the call strips them one at a time. False
    // when the hull is not at a yard, or when there is nothing there to pull,
    // which leaves a stale click costing nothing exactly as FitRank does.
    bool StripRank(const UpgradeDef& def, ShipLoadout& loadout, bool atLab,
                   std::uint8_t mount = TechPick::NO_MOUNT) const;

    // Which line a def arms a mount with, or None for one that isn't mounted.
    [[nodiscard]] static MountArm ArmOf(const UpgradeDef& def);

    // Which family of holes a def goes into, or None for one that belongs to
    // the ship as a whole (a shield, the overburn).
    [[nodiscard]] static SlotFamily FamilyOf(const UpgradeDef& def);

    // Whether a def goes into a numbered hole on the hull at all.
    [[nodiscard]] static bool IsMounted(const UpgradeDef& def)
    { return FamilyOf(def) != SlotFamily::None; }

    // Whether that hole already carries this line. True for anything that
    // isn't mounted, and for NO_MOUNT, which asks the ship-wide question.
    [[nodiscard]] static bool HeldInMount(const UpgradeDef& def, const ShipLoadout& loadout,
                                          std::uint8_t mount);

    // What an AI fits next, given what it can afford and reach: the def and
    // rank it should buy, or a null def when nothing is worth taking. A human
    // reads the tree and decides; a pilot scores it against what it is already
    // carrying, so a wing ends up spread across shields, racks and guns rather
    // than every ship buying the same first thing. Deterministic -- state in,
    // choice out, no randomness (ADR 0001).
    struct Choice {
        const UpgradeDef* def = nullptr;
        std::uint8_t rank = 0;
    };
    [[nodiscard]] Choice PreferredFit(const ShipLoadout& loadout, const ShipContext& context) const;

    // What an AI faction researches next, by the same rule. `budget` is how
    // much of its Tech it is willing to commit right now -- without one a side
    // sits on its points and its pilots fly stock hulls all match.
    [[nodiscard]] Choice PreferredUnlock(const TechUnlocks& unlocked, std::uint32_t budget) const;

    // Where a def sits when the pool is drawn as a tree: `col` is the length
    // of its prerequisite chain, `row` its order among the defs at that depth.
    // Derived at Load() rather than authored, so adding a node to the toml
    // places itself. Both trees are the same graph, so one layout serves both
    // tabs -- and a def's prerequisite is always exactly one column to its
    // left, which is what lets the UI draw a connector as two rectangles.
    struct TreeSlot {
        std::uint8_t col = 0;
        std::uint8_t row = 0;
    };

    [[nodiscard]] TreeSlot SlotOf(std::size_t defIndex) const;

    [[nodiscard]] int TreeColumns() const { return m_treeColumns; }

    [[nodiscard]] int TreeRows() const { return m_treeRows; }

private:
    // Fills m_layout from the prerequisite chains. Called at the end of Load().
    void BuildLayout();

    std::vector<WeaponDef> m_weapons;
    std::vector<UpgradeDef> m_defs;
    Fittings m_fittings;

    TreeSlot m_layout[MAX_UPGRADE_DEFS] = {};
    int m_treeColumns = 0;
    int m_treeRows = 0;
};

} // namespace Gravitaris
