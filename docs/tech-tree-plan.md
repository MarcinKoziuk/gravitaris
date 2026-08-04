# Tech tree

Replaces the three-card research draft with a browsable tree in two tabs, and
the research queue with two currencies.

## The model

One pool of upgrade definitions, two rank tracks over it:

- **PERMANENT tab** spends **Tech** (technology points) to raise the *faction's*
  unlocked rank of a def. Shared across the side, produced by Labs, committable
  from the cockpit -- the labs do that work, not the hull.
- **SHIP tab** spends **Supplies** to fit a rank onto *this hull*, never above
  the faction's unlocked rank. Per pilot, global, banked across lives,
  committable only while landed at a friendly lab planet or docked at its High
  Port.

The two tabs work differently, and deliberately. The permanent tree is a
**ladder**: a faction unlocks rank I, then II, then III, because research is a
progression it makes. The ship tree is a **menu**: any unlocked rank can be
bought outright at that rank's own price, with no need to have owned the ranks
below it. A pilot who has III unlocked but only enough Supplies for II fits II
-- and after a death, refitting is one click at whatever rank they can afford
rather than a climb from the bottom.

The two trees are a 1:1 mirror: the same defs, the same prerequisite shape, the
same depth-derived layout. The permanent tree is what the side knows how to
build; the ship tree is what this airframe is carrying. Nothing new has to be
authored -- MISSILE BAY, FIELD PLATING, HEAVY ROUNDS and the rest each appear
twice, once as a thing to learn and once as a thing to fit.

`Structure`'s existing raw/finished materials are untouched: that is the
Colony -> Base freighter economy, a separate thing that pays for freighters and
site development. Tech and Supplies are their own currencies and do not draw on
it.

## What goes away

The research queue, entirely:

- `FactionState::upgradesReady`, `EconomyConfig::Research::stockCapacity`
- `UpgradeCatalog::ResearchStockCapacity`, `UpgradeKind::ResearchStock`,
  `UpgradeLevels::researchStock`
- `Structure::upgradesReady`, `Structure::researchStockLevel`
- the `research_queue` def in `upgrades.toml`

The draft:

- `UpgradeCatalog::Offers`, `OFFER_COUNT`, `RollOffers`, `PreferredOffer`
- `DraftSeed` in `research-system.cpp`
- `UpgradeDraft::offers`, `EntityState::upgradeOffers`

And two things the new model makes meaningless:

- **`UpgradeScope`** -- with the queue node gone, no def belongs to one scope.
  Every def has both a faction track and a ship track.
- **`UpgradeCatalog::Combined()`** -- a ceiling is not an overlay. Faction rank
  bounds ship rank; it never contributes to it.

## Two rank tracks, two different shapes

The ship's track stays `UpgradeLevels`: named `uint8_t` fields, because
`ResolveStats` and the sim rules read them by name.

The faction's track must **not** reuse it. `UpgradeLevels` carries a single
`shield` rank plus a `shieldType`, which encodes "one shield is fitted" -- but a
faction can perfectly well have learned both the bubble and the plating, and a
hull then chooses. So:

```cpp
// Fixed width, indexed by the def's index in UpgradeCatalog::Defs(). A file
// declaring more than this is clamped at load, the same way Body::AddPlates
// clamps against MAX_SHIELD_PLATES.
inline constexpr std::size_t MAX_UPGRADE_DEFS = 32;

struct TechUnlocks {
    std::uint8_t rank[MAX_UPGRADE_DEFS];
};
```

A def-indexed array is the wrong shape for the ship side (the sim needs named
reads) and exactly right here, since nothing reads unlock ranks except the
tree's own gating. Plain POD, fixed width, no allocation -- the same shape
`ShipLoadout::plates` already has, and for the same reason: one width serves the
component, the wire and the UI.

## Costs

Each def declares both, scalar for a flat charge or a list to charge per rank:

```toml
tech_cost   = [1, 2, 4]   # to unlock rank 1, then 2, then 3, for the faction
supply_cost = [2, 5, 9]   # the full price of fitting that rank to a hull
```

`supply_cost` is an **absolute price, not an increment**: fitting rank III costs
9 whether or not the hull ever carried I or II. That is what makes the ship tab
a menu rather than a ladder, and it means `UpgradeCatalog::Apply` sets the rank
outright instead of incrementing it.

Ranks offered in the ship tab are those strictly above the hull's current fitted
rank, capped at the faction's unlocked rank -- with one exception already in the
code: a shield of the *other* type offers every unlocked rank, since taking it
swaps rather than stacks, and swapping down is the player's call to make (see
`IsEligible`).

## Tech

`FactionState::researchProgress` keeps its meaning: Labs pool into it, N labs
fill it N times as fast, and each fill pays `tech_per_fill` into
`FactionState::techPoints`. `economy.toml`'s `[research]` drops
`stock_capacity`, renames `seconds_per_upgrade` to `seconds_per_tech`, and gains
`tech_per_fill`.

There is no cap. The old one existed to stop a side banking a match's worth of
upgrades while its pilots never came home; that pressure now comes from
Supplies, which only bank on a landing.

Tech is a **team** resource: in multiplayer two pilots draw on one pool, and one
of them can spend it all from orbit. Deliberate, but worth knowing.

The two tabs are not two grinds, because they run at different rates and
different scopes: Tech is expensive and slow, off a shared lab bar, and buys a
handful of strategic decisions across a match; Supplies accrue continuously and
are spent at every landing. One genuine property falls out of unlocks being
permanent and shared -- late in a long match a side knows everything and the
permanent tab goes quiet. That makes it a **race**, where being first matters
and losing your labs early locks you out of a line for good. That is the 1997
design working rather than a problem to solve.

## Supplies, and where a pilot's bank lives

Nothing today outlives a ship on a per-pilot basis -- `NetServer::PeerState`
holds a name and a team, and `Callsign` sits on the hull itself. So:

**`PilotAccount`**, one entity per pilot, created lazily exactly the way
`FactionSystem` creates `FactionState` (see the note at the head of
`faction-state.hpp`). A ship carries a reference to its own. Human peers and AI
pilots both get one, which keeps the currency inside the sim -- deterministic,
replay-safe, ADR 0001 -- rather than parked in the net layer where a replay
couldn't see it.

```cpp
struct PilotAccount {
    std::uint32_t supplies = 0;
};
```

One balance, and no separate uncollected pool. An earlier draft had a `pending`
amount that only banked on touchdown, to make landing the collection ritual --
but that is a second lock on a door already bolted: Supplies cannot be *spent*
without landing, since every ship node requires `atLab`. The only thing the
split would buy is a risk mechanic (die deep in a sortie, lose the income you
hadn't banked), and death keeps Supplies, so there is nothing for it to do.

- `supplies` accrues at `economy.toml`'s `supplies_per_second`, and takes
  `supplies_per_kill` when a `DeathReport` names this pilot as the killer (the
  report already carries killer identity).
- The balance is global to the pilot and survives death untouched. Only the
  *fitted* upgrades are lost with the hull, as they always were -- which is what
  makes coming home to refit a routine act rather than a setback.

## Gating

`IsEligible` grows a sibling that reports *why*, per tab. The permanent tab asks
about one rank (the next one up); the ship tab asks about a *named* rank, since
any unlocked rank is buyable:

| state | permanent tab | ship tab |
| --- | --- | --- |
| `Locked` | prerequisite not unlocked | prerequisite not fitted |
| `Maxed` | unlocked to `maxLevel` | fitted at or above this rank |
| `NotUnlocked` | n/a | rank above the faction's unlocked ceiling |
| `Unaffordable` | not enough Tech | not enough Supplies |
| `NeedsLanding` | n/a -- commits anywhere | affordable, ship is not at a lab |
| `Available` | clickable | clickable |

`NotUnlocked` is the interesting one, and the UI should say so plainly ("III --
unlock in PERMANENT") rather than just greying the pip out. It is what makes the
two tabs visibly explain each other.

## Layout

A `TechTreeLayout` built once at catalog load: column = longest prerequisite
chain depth, row = pack order within a column with catalog order breaking ties.
One layout serves both tabs, since both trees are the same graph. A node's
prerequisite is always exactly one column left, which is what makes connectors
drawable as two or three rectangles.

## Wire

- **The pick** is no longer a 1..3 slot. It becomes
  `{ id_t node, uint8_t tab, uint8_t rank }` -- the tab byte is load-bearing
  (the same def id means "unlock" in one tree and "fit" in the other), and the
  rank byte is what lets a hull buy II when it has III unlocked. The permanent
  tab ignores it and always takes the next rank up. `WriteU8` -> `WriteU32` +
  two `WriteU8` in `protocol.cpp`; `REPLAY_VERSION` 2 -> 3 in `input-log.cpp`.
- **A faction block** in the snapshot, per team: `{ techPoints, TechUnlocks
  blob, defeated }`. Today the only faction state reaching a client is
  `researchStock`, smuggled through each Lab's `Structure` -- which breaks when
  a side has no lab standing, exactly when you want to read the tree.
  `faction-state.hpp` already anticipates this block for Phase 4's `defeated`.
- **The owning pilot's `supplies`** rides the own-ship state; no other client
  needs it.
- `Structure` keeps `researchProgress` alone -- all the lab glow reads.

## AI

`PreferredOffer(offers, ...)` scored three cards. It becomes two decisions:
which ship node to fit when landed with Supplies to spend, and whether to put
the faction's Tech into a line at all. Both deterministic (ADR 0001). Without an
explicit policy for the permanent tree the AI will never unlock anything and
will sit on a stock hull all match, so it needs one -- simplest is a per-
personality share of Tech it is willing to commit, and a preference order over
lines.

`AIPilot::upgradesWanted` changes from "queued upgrades to fly home for" to
"there is something I can afford and fit", which is what its `padWaitRemaining`
should key off.

## UI

New document `data/ui/tech-tree.rml` on the `vector-window` template, so it
drags and resizes for free. Hidden until toggled, by a button beside the
research bar in `hud.rml` and by a hotkey (`T`, suppressed while
`IsKeyboardCaptured()`).

`ui/` stays ignorant of the catalog, as `UpgradeOfferView` already is:

Because the ship tab sells a named rank, a node is not one button -- its rank
pips *are* the buttons. Clicking pip II buys rank II at II's price. So a view
carries its ranks:

```cpp
struct TechRankView {
    int cost;         // absolute price of this rank, in the tab's currency
    NodeState state;  // per rank, not per node
};

struct TechNodeView {
    std::uint32_t id;
    int tab;                  // 0 ship, 1 permanent
    int col, row;
    std::string name, description;
    int rank, maxRank;        // what this track holds / how far it goes
    int cap;                  // ship tab: the faction's unlocked ceiling
    TechRankView ranks[MAX_UPGRADE_RANKS];
};
```

The permanent tab lights only the next rank up, since it is a ladder; the ship
tab lights every affordable rank at or below `cap`.

- `SetTechTree(const std::vector<TechNodeView>&)`
- `SetCurrencies(int tech, int supplies)`
- `SetTechPickCallback(std::function<void(std::uint32_t id, int tab, int rank)>)`
- `SetTechTreeVisible(bool)` / `IsTechTreeVisible()`

Rebuild-on-change compare, as `SetChatLog` does. The node *set* only changes
when a rank is taken, so a later refinement is to rebuild markup on structure
changes and drive states through class flips -- start with the whole-panel
compare.

The HUD sidebar gets both readouts: TECH beside the research bar, SUP under it.

Removed: `#upgrade_draft` from `hud.rml`, the 1/2/3 cases in
`gravitaris.cpp`, and the "1 2 3 -- Take upgrade" binding in `main.rml`.

## Also in the blast radius

- `cheat-console.cpp`: `/research` stocks the queue; becomes `/tech [n]` and
  `/supplies [n]`. `/upgrade` still fits directly, bypassing both currencies and
  the faction gate.
- `tools/sim-test/main.cpp`: `TestUpgradeCatalog` asserts on rolled offers and a
  maxed tier falling off the table -- rewritten against the two tracks, the
  ceiling rule, costs and the depth-derived layout.

## Format

Staying on TOML. KDL was considered and deferred: with the layout derived from
prerequisite depth, `upgrades.toml` remains a flat list of nodes each naming a
`requires`, so there is no nesting for KDL to express and nothing to gain here.
Worth revisiting as its own change at the C++23 move, when `kdlpp`'s C++20 floor
stops being a cost.
