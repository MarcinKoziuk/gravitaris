# Queued work

Three agreed changes and two known bugs, written down so a cold session can
pick any of them up without re-deriving the context. Decisions here are the
project owner's and are settled unless this document says otherwise; the open
questions are marked as such and are worth asking before writing code.

Nothing in here is started. The work that preceded it shipped in `155a876`,
which the last section summarises.

## How to build and check

The MSVC presets need a developer environment, and the `cmd /c "vcvars && cmake"`
one-liner silently builds nothing. Write a batch wrapper and call that:

```
@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --build C:\Users\marcin\Projects\GravitarisNG\out\windows-msvc-relwithdebinfo --target %*
exit /b %errorlevel%
```

Targets: `GravitarisNG`, `gravitaris-server`, `gravitaris-sim-test`. Confirm a
build actually ran by looking for `Linking CXX executable`, not by the absence
of the word "error".

`gravitaris-sim-test` is the headless suite — run its `.exe` by absolute path
from `out/windows-msvc-relwithdebinfo/RelWithDebInfo/bin`. It is silent on pass
and prints `FAILED:` with a nonzero exit on failure. It also checks the sim is
deterministic across 1800 ticks, so any change that reaches into sim state has
to keep that green.

The owner drives the GUI themselves. Launching the client when asked is fine;
screen capture is not. Verify UI work with numbers and synthetic events, not
screenshots.

## 1. Make FIELD PLATING worth taking

**The problem.** Plating is strictly riskier than the bubble today. It leaks a
share of every round that gets through (`leak_chance = 0.5`,
`leak_fraction = [0.25, 0.175, 0.10]` in `data/upgrades.toml`), and leaked
damage lands on the hull — which only regenerates by standing on one of your
own developed planets (`RepairSystem`). So the plating pilot accumulates
permanent damage the bubble pilot never takes, and the bigger reservoir and
faster regen do not pay for it.

**Two agreed changes, both small. No rebalance of the existing numbers yet.**

### 1a. Plates absorb collisions and hard landings

Ramming and bad landings currently bypass the shield entirely and hit hull.
In a game substantially about putting a ship down a gravity well onto a pad,
"plating makes a bad landing survivable" is a large practical perk and
thematically right — plates are physical armour, a bubble is a field.

The sites are in `src/game/system/combat/damage-system.cpp`:

- `LandingCrash` — emitted around line 114, off `m_physicsSystem.DrainImpacts()`.
- Ram/impact damage between two hulls — the `Impact` emits near lines 439 and 479.

Both need to route through `AbsorbWithShield` (line ~242) the way bullet damage
does, but **only for plating**: a bubble absorbing a planet is not the intent.
Note `AbsorbWithShield` takes an optional plate `element` — a collision has a
contact point, so resolving which plate took it via
`DamageSystem::ShieldElementFor` is the natural mapping, and it keeps the
per-facet model honest.

### 1b. A plate only leaks once it is damaged

Currently a plate at full charge leaks exactly as often as a plate about to
fail. Gate the leak roll on the struck plate's remaining charge: above some
fraction of its per-plate capacity it stops rounds whole, below it the existing
`leak_chance`/`leak_fraction` behaviour applies.

The decision is one condition at `damage-system.cpp:264`:

```cpp
const bool leaks = stats.shieldLeakChance > 0.f
        && LeakRoll(step, m_leakSeq++, *element) < stats.shieldLeakChance;
```

Per-plate capacity is `PerPlate(loadout, stats.shieldCapacity)` from
`component/ship-loadout.hpp`. The bubble path (`SHIELD_BUBBLE`) must be left
alone — it has `leak_chance = 0` anyway, but do not make that an accident.

**Start the threshold at 50% and author it in `data/upgrades.toml`** next to
`leak_chance`, so it can be tuned without a rebuild. Add it to
`UpgradeDef::Shield` and `ShipStats`, parsed in `UpgradeCatalog::Load`.

**Why this is worth more than it looks:** the HUD already draws one segment per
plate shaded by its own charge (`UI::SetShieldSegments`). Once leaking is tied
to plate damage, the player can *see* which facet is about to start failing and
turn a fresh side to the enemy. That is skill expression the bubble structurally
cannot have, and it is the real argument for the upgrade.

Cover both in `tools/sim-test`, near the existing shield tests (search for
`shieldLeakChance` and `leakFraction` in `tools/sim-test/main.cpp`).

## 2. Retire the research readouts behind a runtime flag

**Decision:** the research bar, its countdown and its completion chime all come
off by default. None of the code is deleted — all three sit behind **one
runtime flag**, off, so a better design can switch them back on without a
rebuild. The owner's reasoning: the bar is a timer you cannot influence and
cannot lose, so it does not earn permanent HUD space.

Do not redesign research as part of this task. Several options were discussed
(salvage-fed research, directed targets, showing the enemy's bar) and none was
chosen; that is a separate conversation.

**One flag covers all three.** Put the toggle somewhere the debug UI can reach —
`src/cgame/ui/debug/hud-panel.cpp` is the natural home, next to the other HUD
switches. Runtime was chosen over compile-time deliberately: the point is being
able to try it again cheaply. The cost is that the markup stays in the document
and gets walked every frame; see the RmlUi note at the end of this file, and
prefer hiding the block via a class on an element that is already there over
leaving a second copy of the layout around.

What the flag has to gate:

- **The bar and countdown.** `data/ui/hud.rml` — the `div.stat` block around
  line 613 holding `#research_value` and `#research_fill`, plus the
  `div#research_fill` rules near line 219. `UI::SetResearchReadout` in
  `src/cgame/ui/ui.cpp` and the `m_researchFill` / `m_researchValue` pointers.
  `GravitarisApplication::RefreshResearchReadout` in
  `src/client/gravitaris.cpp` (~line 600), called from `RefreshHudReadout`.
- **The chime.** `GameEventType::ResearchComplete` in
  `AudioSystem::Update` (`src/cgame/audio/audio-system.cpp`). Gate the *play*,
  not the clip: `m_researchClip` must keep loading, because the clip pointers
  are dereferenced unconditionally and a default-constructed `ResourcePtr` is
  not safe there (see the constructor's own comment).

`CGame::GetResearchReadout` stays as it is either way — it is the data source,
it is cheap, and the flag should not reach into the sim.

**Also wanted, same task and NOT behind the flag:** the "x2 labs" multiplier
currently appended to the countdown text moves onto the small TECH counter, so
that information survives the bar being switched off. See
`RefreshResearchReadout`, which builds `" x" + std::to_string(research->labs)`.
The TECH and SUPPLIES counters themselves stay exactly as they are.

## 3. Make every AI ship addressable

**Scope was cut on 2026-08-09. Read this before reaching for the bird list at
the bottom of this file — the naming idea is shelved, not queued.**

**The problem.** AI fodder ships carry no `Callsign` at all. AI leaders fly as
`blue-leader` / `red-leader` (`Game::LeaderCallsign`), and `/players` walks
entities that have a `Callsign` — so the fodder is invisible in the listing and
there is no name for `/tp` to resolve. A wing of eight ships is, from the
console, eight things you cannot point at.

**Decision:** keep the naming scheme exactly as it is. Do *not* introduce the
bird pool. The only requirement is that every AI ship shows up in `/players`
and can be reached with `/tp`.

**The inference this rests on, worth confirming:** the obvious way to satisfy
that without inventing a naming scheme is to extend the existing one — fodder
gets `<color>-1`, `<color>-2`, … alongside `<color>-leader`. That is an
inference, not something the owner said. If they want something else, this is
the decision to get before writing code.

Both cheats already work off `Callsign` and need no changes once the component
is there:

- `CheatPlayers` in `src/game/cheat/cheat-console.cpp` lists everything with one.
- `FindCallsign` in the same file is what `/tp <name>` resolves through.

Worth offering, not agreed: `/players` currently cannot tell a human from an AI,
and once the fodder is listed the output gets long. Marking AI entries — and
leaders specifically — would help. `ship.has<AIPilot>()` is the test; a
`Callsign` with no `AIPilot` behind it is a person (`Game::FillEmptyTeamsWithAI`
already relies on exactly that).

### Where it plugs in

- `include/gravitaris/game/component/callsign.hpp` — the component.
- `EntitySpawner::SpawnAIShip` in `src/game/spawner/entity-spawner.cpp` — the
  fodder path, which emplaces no `Callsign`. Leaders get theirs in
  `Game::AddAIFaction` (`src/game/game.cpp`) via `Game::LeaderCallsign`.
- `Game::SpawnRandomAIShip` and the `/spawn` cheat both come through
  `SpawnAIWave`, so a wave has to number without colliding with what is already
  flying.

**Determinism matters.** The sim must stay reproducible (ADR 0001, and
sim-test's 1800-tick hash). A per-team counter that only ever increments is
both deterministic and collision-free, and is simpler than reusing numbers from
dead ships. If a numbering scheme ever does need to consult what is currently
flying, note the flecs gotcha in `CLAUDE.md`: a query run inside another query's
callback silently iterates nothing, so gather into a plain container first.

`/tp red-leader` keeps working unchanged, since leaders keep their names. That
was the risk in the shelved version of this task and it no longer applies.

### Shelved: the bird callsign pool

Kept because the list was curated and approved before the scope was cut, and it
may be wanted later — for joining players, for a round-setup screen, or if AI
naming comes back. **It is not queued work.**

```
BATELEUR   CARACARA   CONDOR     CORVUS     FULMAR
GOSHAWK    HARPY      HARRIER    IBIS       JAEGER
JUNCO      KEA        KESTREL    LANNER     MERLIN
NENE       OSPREY     PETREL     QUETZAL    SAKER
SHRIKE     SKUA       TERATORN   TUI        VIREO
WHIMBREL
```

Rules it was picked under, for whoever extends it: real birds only, no compound
`-bill`/`-tail`/`-wing` names, nothing that is a common English word first and a
bird second, nothing soft. `PEREGRINE` and `RAPTOR` were cut for reading as the
category rather than as a name next to `MERLIN` and `KESTREL`. Bench:
`AVOCET`, `SISKIN`, `GARGANEY`, `GRACKLE`, `NIGHTJAR`. The intended assignment
scheme was: pick at random from the names not currently in use, then append
`-0`, `-1`, … once the pool is exhausted.

**Left unresolved when the scope was cut:** whether a *joining player* who never
types a name should still draw from this pool instead of defaulting to
`"Pilot"` (`Game::SetPlayerName` / `Game::m_playerName`, and
`NetServer::PeerCallsign` at `src/game/net/net-server.cpp:278`). That was agreed
while the pool was in scope and was never explicitly withdrawn — the withdrawal
was worded as being about AIs. Ask before either building the pool for it or
quietly dropping it.

## 4. Bug: AI ships get stuck under the High Port

**Deferred deliberately** — the owner asked for it later, and it wants a fresh
session rather than being bolted onto UI work.

Reported symptom: AI ships wedge themselves *between the planet and the High
Port* and stay there. The guess in the report is that they are trying to restock.
Nothing has been investigated yet; treat that guess as unconfirmed.

Where to start looking: `AIPilotSystem` / `AIStrategySystem`, the `Rearm` goal
and the `Land` order it issues, and `LandOnBody` guidance. A High Port is a
station in orbit rather than a surface pad, and `docs/ai-ships.md` records that
`Rearm` lands a leader at its "nearest home planet" — a station parked above a
planet is a plausible way for "nearest" and "landable" to disagree. `git log`
also shows a recent fix titled *"a pad is a place, not a planet"*, which is
adjacent enough to be worth reading first.

## 5. Bug: remote structures replicate no maxHp

Found while fixing the own-ship hull bar, reported, not fixed, not scheduled.

`EntityState` carries `float hp` and no `maxHp`. The mirror world hardcodes it:
`entity.emplace<Damageable>(state.hp, 100.f)` at
`src/cgame/net/snapshot-applier.cpp:92` and `:104`. Ships get away with it
because they use `Damageable`'s own 100.f default, but structures are spawned
with `hull.maxHp *= STRUCTURE_HP_SCALE` (`entity-spawner.cpp:209`), so any
health fraction drawn for a spectated remote structure is wrong by that factor.

Either send `maxHp`, or have the applier resolve it from the model the way the
spawner does — the client already loads the `Body` for its `HitOutline`.

---

## Context a new session will want

**Where things live.** `src/game` is the sim (server-authoritative), `src/cgame`
is the client-side game layer, `src/client` is the application, `src/server` is
the dedicated server. Shared headers go in `include/gravitaris/...`; headers
only one module needs sit beside the `.cpp`.

**Recent shape changes worth knowing** (all in `155a876`):

- Ammo stowage is generic. A hull authors `ammo_N` bays and either locker fits
  either bay. `SlotFamily::AmmoBay`, placement on `ShipLoadout::ammoBays`, count
  on `UpgradeLevels::ammoStore`. `MAX_AMMO_BAYS` is 2 and deliberately tighter
  than `MAX_WEAPON_MOUNTS`.
- Snapshot version is **15**. Bump it for any wire change; client and server
  refuse to talk across versions.
- `ApplyEntityShipState` (was `ApplyEntityLoadout`) is the single place a
  client writes a ship off the wire, in both the mirror world and for its own
  predicted hull. Anything server-authoritative that a readout shows belongs in
  it — it has now been missed twice, once for the weapon mounts and once for
  `hp`.
- A dedicated server fields no AI leaders. `/ai` in chat, or `ai`/`ai fill` on
  the console, takes the sides nobody is flying.
- The overburn will not light below 10% of its tank
  (`BOOST_ENGAGE_SHARE`, `ship-controls-system.cpp`). It gates *starting* a
  burn; one already running still goes to empty.

**A UI performance trap, since two of these tasks touch the HUD.** RmlUi walks
every element of every document on every `Context::Update()`, hidden documents
included — `ElementDocument::Hide()` sets `visibility: hidden`, not
`display: none`, so a hidden document is laid out too. The tech window now
throws its generated markup away while closed (`UI::DiscardTechMarkup`). Any new
panel that is usually not on screen should do the same. Separately, never put a
value that changes every tick inside a `*View` struct's `operator==` — the
`Set*` functions rebuild markup on any difference, and one live ammo count in
`TechNodeView` once cost an order of magnitude of framerate.
