# Gravity Well game mode — implementation plan

Status: plan written 2026-07-19, revised same day after review with Marcin
(freighter logistics confirmed in, freighters orbit rather than land, sector
generation pushed late, UI phases trimmed). No implementation started. This
is the "simple game mode" vertical slice from `IDEAS.md`, modeled on the
original *Gravity Well* — full mechanics + UI reference in
`gravity-well-1997.md` (read that first; it distills the vendored manual and
screenshots in `docs/gwell/`).

**How to use this doc**: phases are ordered by dependency and sized to be
individually completable and testable. Work one phase at a time. Each has a
"Done when" gate — do not start the next phase until it passes. Follow the
referenced existing code patterns closely; they encode house style and
netcode constraints that are easy to violate otherwise. Update the status
checkboxes and add a short "Verification status" note per phase as you go
(same convention as `networking-plan.md`).

## Scope decisions (settled with Marcin 2026-07-19/20 — don't relitigate)

- **Multiplayer is a first-class goal, not a later port** (clarified
  2026-07-20): an early version must support playing against other players,
  against AI, or any mix. A faction/team is not one pilot — **a team can
  field multiple fighters** (human co-op, or humans + AI leaders sharing a
  team). Concretely this means: never key a rule off "the player" — key it
  off `Team` (claiming, friendly sites, defeat) or off the individual ship
  (respawn site, landing state), whichever the rule is actually about. All
  sim code below runs in `game/` (headless, server-authoritative), so the
  existing netcode carries the mode; the multiplayer-specific wiring gap is
  small and tracked in its own phase below.
- **Full freighter logistics IS in scope** — the materials economy runs on
  real freighters doing real trips, like the original: colonies produce raw
  materials, freighters haul them, bases/high ports convert raw → finished,
  construction/repairs/production consume them. (An earlier draft abstracted
  this to timers; that was a misunderstanding — any "timer" in this mode is
  at most a rate constant, the flow itself is physical.)
- **Freighters do NOT land.** On arrival they enter a **kinematic 2-body
  orbit** around the target planet (exactly how planets orbit suns —
  `Orbit` component + `OrbitSystem`) and load/unload/build from orbit.
  Landing guidance is only ever needed for *fighters* (the player already;
  AI leaders in the AI phase, to claim planets).
- **Respawn rule**: your fighter is rebuilt at the **last friendly planet or
  high port you landed/docked at**. If that site is gone, fall back to any
  remaining friendly one; if nothing friendly is left — game over.
- **Sector generation comes late** (its own phase near the end). Until then
  the mode plays on the existing classic two-sun layout with hand-picked
  starting planets.
- **Rockets/missiles, tech upgrades, shields: deferred.** The HUD reserves
  space for them but nothing implements them in this slice. Ships have
  `Damageable` hull only.
- **Win/lose is per-faction (team), not per-pilot**: a team wins when every
  planet is claimed by it; a faction is defeated when all its colonies AND
  freighters are destroyed (can no longer expand, the original's rule). An
  individual pilot dying just respawns (see above) as long as their faction
  still has a facility to rebuild at; a faction whose facilities are all
  gone is out — for a team of human players, that's the whole team's game
  over together.
- **Round-based**: when it's over it's over, nothing persists.

## Invariants (apply to every phase — same list as networking-plan.md)

- All rules/simulation code in `game/` (headless). `cgame/` gets rendering
  and UI only. If `gravitaris-sim-test` stops linking, you pulled a client
  dependency into the sim — fix that, don't work around it.
- No wall clock, no `std::rand`, no iteration-order dependence. Randomness =
  splitmix seeded from tick + a counter; copy the pattern in
  `Game::SpawnRandomAIShip` (`src/game/game.cpp`).
- One-shot occurrences (claimed! built! destroyed!) are `GameEventQueue`
  events (`include/gravitaris/game/event/game-event.hpp`), never cgame-side
  observers. Extend `GameEventType` as needed.
- New components: POD, declare replication class (replicated / client-only /
  server-only) in a comment, reference other entities by `NetId` value —
  never store a raw `flecs::entity` in a replicated component.
- Prefer fields on existing components over add/remove tag churn
  (CLAUDE.md, "ECS component design").
- `gravitaris-sim-test` two-run checksum must stay stable after every phase.

## Unit mapping (original → Gravitaris)

| Original | Here | Existing base to build on |
|---|---|---|
| Fighter | player + AI leader ships | exists (`SpawnPlayer`, `SpawnAIShip`, `docs/ai-ships.md` phases 0–4) |
| Freighter | NPC ship; flies via GNC, **orbits kinematically at destination** | GNC guidance (`game/gnc/`), `Orbit`/`OrbitSystem` for the arrival orbit |
| Base / Colony / Lab / Comm Center | planetside structures | new `Structure` component; models as SVG+YAML like ships |
| High Port / Space Dock / Sensor Array | orbiting structures | `Orbit` component (kinematic, per IDEAS.md) |
| Planetary/orbital defenses | auto-firing turret on Base/High Port | reuse `SpawnBullet` + `DamageSystem`; aiming like `AIPilotSystem`'s firing solution |
| Claiming by landing | landed-state detection + `Team` on planet | `Team` exists; safe-landing detection is a gap (Phase 1) |
| Materials economy | real: raw/finished stores + freighter trips | new per-structure store fields; rules in one economy system |
| Sector (4–6 stars, 1–3 planets) | parametrized scenario — **late phase** | `classic-scenario.cpp`, `SpawnStar/SpawnOrbitingPlanet` |

## Phase 1 — Landing state + claiming

Goal: "safely landed" is a detectable sim state; landing on an unowned,
undeveloped planet claims it for your team. Arena: the existing classic
scenario, unchanged.

- [x] Safe-landing detection in `game/`: `LandingState` component
  (`include/gravitaris/game/component/landing-state.hpp`, server-only,
  emplaced by `SpawnPlayer`/`SpawnAIShip`) + `LandingStateSystem`
  (`src/game/system/landing-state-system.cpp`): landed = live contact with
  a Planet entity (new `PhysicsSystem::ForEachTouching`, which iterates
  Chipmunk's current arbiters rather than duplicating contact math) +
  relative speed under `SAFE_LANDING_SPEED` (20, comfortably below
  DamageSystem's damage thresholds) + upright (same legs-toward-surface
  cosine test the impact path uses).
- [x] "Last friendly landing site" tracked in the same component
  (`lastFriendlySiteNetId`), updated on landing at a friendly planet and on
  a successful claim.
- [x] Claiming: `ConquestSystem` (`src/game/system/conquest-system.cpp`) —
  fires once when `landedTicks == CLAIM_TICKS` (60, so a graze never
  claims); planet must have an `Orbit` (suns are not claimable) and a
  different/absent owner. Emits `GameEventType::PlanetClaimed` (param = new
  team id). Enemy-structure check is a marked TODO for Phase 2 (structures
  don't exist yet).
- [x] Replication came almost free: `EntityState.teamId` already travels
  for every entity, so planets just needed a `Team` component —
  `SpawnCelestial` now emplaces `Team{TeamId::None}` (= unowned) from
  birth. No `SNAPSHOT_VERSION` bump. One real client-side fix:
  `SnapshotApplier` only set `Team` at entity creation, never on update, so
  a mid-round claim would never reach the mirror world — the update path
  now `set<Team>`s too.
- [x] Debug readout in the Flight panel (landed / speed-vs-safe-threshold /
  claim-progress ticks); the real HUD aid is UI phase U2.
- [x] Sim-test `TestLandingAndClaiming`: scripted gentle descent ⇒ landed,
  claimed, `PlanetClaimed` emitted, friendly-site recorded; fast crash ⇒
  damage, no claim. Caught a real test-authoring gotcha: spawning a ship
  with < ~40 units of surface clearance overlaps its shape into the planet,
  and Chipmunk's overlap resolution kills it instantly (flecs then
  fatal-asserts on `.get()` of the dead entity — which on Windows shows as
  a silent hang, since MSVC's abort dialog has no console).

**Verification status** (2026-07-20): native `GravitarisNG`,
`gravitaris-sim-test`, `gravitaris-server` build clean; sim-test passes
with the new proof and the two-run determinism checksum stable. Not yet
done: wasm build verification, and the "claim a planet in a real
playthrough" half of the gate — needs a hand-flown landing on a planet in
the classic scenario (watch the Flight debug panel for the landed/claim
readout; there is no visual ownership feedback until UI phase U1).

Done when: you can claim a planet in a real playthrough and the sim-test
proof passes.

## Phase 2 — Structures

Goal: all seven structure types exist as spawnable, damageable, team-owned
entities; a hand-assembled starting complex spawns at the player's starting
planet.

- [x] `Structure` component (replicated): `{ StructureType type; float
  rawMaterials, finishedMaterials }` (unused until Phase 3, wired into the
  wire format now — `SNAPSHOT_VERSION` bumped to 4 — so that phase doesn't
  need a second bump). New `NetEntityType::Structure`; `SnapshotApplier`
  emplaces `Team`/`Damageable`/`Structure` for it, same shape as `Ship`.
- [x] Planetside structures (Base/Colony/Lab/Comm Center): kinematic bodies
  at a fixed offset from the planet's live position — genuinely *static*
  bodies would drift away as the planet orbits its sun, so this reuses the
  same kinematic-tracking idea `Orbit`/`OrbitSystem` already use for
  planets, just centered on a moving parent instead of a fixed point (new
  `PlanetSurfaceAttachment` component + `StructureAttachmentSystem`, since
  `Orbit::center` is intentionally a fixed `Vector2d` and changing that
  would complicate its existing replicated contract for planets-orbit-suns).
- [x] Orbital structures (High Port + its Space Dock/Sensor Array): same
  idea, real circular-orbit math around the live planet position (new
  `PlanetOrbitAttachment` component, same system). Attached at the same
  orbit radius, small phase offsets.
- [x] Simple SVG box models under `data/models/structures/{base,colony,lab,
  comm-center,high-port,space-dock,sensor-array}/`, team-colored stroke
  (`#ff00ff` placeholder), sized to nest inside a ~60-unit-radius planet
  (planetside) or orbit outside it (orbital radius 90). Visual polish later,
  as planned.
- [x] `EntitySpawner::SpawnStructure`/`SpawnOrbitingStructure`, following
  the existing `Spawn*` patterns; a shared `SpawnStructureBase` private
  helper does the common Team/Damageable/Structure/NetId setup.
- [x] Defenses: `StructureDefenseSystem` — Bases and High Ports (marked by
  a new server-only `StructureDefense{fireCooldown}` component) auto-fire
  at enemy ships within `FIRE_RANGE` (400) on a 90-tick cooldown, leading
  the target with the same intercept-time math `AIPilotSystem` uses (no
  rotation/aim-tolerance needed -- a turret doesn't visually aim).
- [x] Hand-assembled starting complex: new `BuildStartingComplex` (separate
  from `BuildClassicScenario`, which now returns a designated "home" planet)
  spawns the full complex there for `TeamId::Blue`, loosely matching
  `docs/gwell/screenshots/start-game.png`. Wired into both `Game::Start()`
  (single-player) and `gravitaris-server`'s startup (shared for now — real
  per-faction starting planets are Phase 6's job).
- [x] Sim-test `TestStructures`: spawns a full complex on a real orbiting
  planet (real mass, so it actually moves); asserts all seven types exist
  exactly once, a Base carries `StructureDefense`; runs 600 ticks and
  asserts both a planetside structure's offset-from-planet and an orbital
  structure's orbit-radius-from-planet stay put (i.e. they track the
  planet's motion rather than drifting in fixed world space, which was the
  whole point of the attachment system over reusing `Orbit` as-is); asserts
  an enemy ship placed within a Base's `FIRE_RANGE` (but clear of every
  structure's own collision shape, to avoid a spawn-overlap kill) takes
  damage from defense fire.

**Verification status** (2026-07-20): all four targets build clean (native
`GravitarisNG`, wasm `GravitarisNG`, `gravitaris-sim-test`,
`gravitaris-server`); sim-test passes with the new proof and the two-run
determinism checksum stable; a native single-player smoke run loads/renders
the new starting complex with no errors. **Not yet manually verified**: a
real multiplayer session with the complex actually visible/fightable (no
visual ownership feedback beyond the existing team-color rendering exists
yet — that's UI phase U1/U2), and whether the layout/sizing reads well at
actual camera zoom (visual polish explicitly deferred).

Done when: the starting complex renders, orbits, and shoots back in a real
playthrough; sim-test proof passes.

## Phase 3 — Freighters + materials economy + build loop

Goal: the original's automated economy, physically: colonies produce, 
freighters haul and build, bases convert. Claiming a fresh planet results —
hands-off — in a complex growing there. The heart of the mode.

- [x] Freighter model: `data/models/ships/freighter-0/` (hull + two visible
  cargo pods). Kinematic, slow.
- [x] Freighter movement: **scope simplification** — transit is a plain
  constant-speed kinematic seek toward the target planet's live position,
  not full GNC `GotoPoint`/`InterceptEntity` inertial flight. Freighters are
  background economy actors the player never pilots or dogfights (only
  shoots at, as a target); wiring up `FlightController`/`GuidanceParams`
  per freighter is real extra plumbing for no gameplay-visible benefit here,
  and a plain homing seek is far easier to keep deterministic. On arrival
  (within `FreighterSystem::ARRIVAL_RADIUS`), it gets a **real
  `PlanetOrbitAttachment`** — the exact mechanism High Port already uses —
  so `StructureAttachmentSystem` takes over its motion; no separate landing
  or departure state needed since it's consumed on build, never idles.
- [x] Economy rules, `src/game/system/economy-system.cpp`: Colony produces
  raw/tick (capped); supplies its own planet's Base and (independently)
  High Port, two separate draws against the same store; Base/High Port
  convert raw → finished/tick (capped). Lab/Space Dock dispatch a freighter
  when **the structure they accompany** (Base for Lab, High Port for Space
  Dock — matching gravity-well-1997.md's "constructs Freighter from
  finished materials of the Base/High Port it accompanies"; Lab/Space Dock
  hold no materials store of their own) can afford `FREIGHTER_COST`, to the
  nearest friendly claimed planet still missing Base/Colony/High Port (in
  that order) with no freighter already tasked to it — ties broken by
  NetId. New-unit rule re-checked fresh at build time (`FreighterSystem`),
  not trusted from dispatch time.
- [x] Freighter brain, `src/game/system/freighter-system.cpp`: **scope
  simplification** — two states, not four (Transit → Arrived-and-build),
  since freighters are pre-loaded at spawn (cost already paid by the
  funding structure at dispatch), so there's no separate "visit a colony to
  load cargo" trip for this construction role. On arrival, re-validates and
  builds the next missing structure (or is simply consumed without building
  if someone else already did — an accepted rare-race edge case), emitting
  the new `GameEventType::StructureBuilt`.
- [x] Freighter production: done as part of EconomySystem above.
- [x] Sim-test `TestFreighterEconomy`: seeds a fully developed home complex
  + one claimed-but-empty planet; confirms materials flow naturally over a
  short window (Colony → Base) before gifting funds to isolate the
  build-sequence timing from the (correctly slow) natural ramp; asserts
  Base → Colony → High Port appear in that order, at least one freighter
  was actually dispatched, and every dispatched freighter was consumed
  (none left idling). Two-run determinism checksum stable.

**Known gap, explicitly deferred**: resupplying an already-complete but
materials-starved planet (the original's *other* freighter role, for a
planet with no Colony of its own) isn't modeled — only the construction
case (missing structures) triggers a dispatch, and only a planet that's
also the freighter's *build target* gets any resupply benefit (see the
2026-07-21 update below) — a needy planet with everything already built
still gets nothing. Also: Lab/Space Dock's own `Structure::rawMaterials`/
`finishedMaterials` fields are simply never written to (by design, per the
manual's "the Base/High Port it accompanies" wording) — worth remembering
if a future phase reads them expecting non-zero values.

**Verification status** (2026-07-20): all four targets build clean; a
native single-player smoke run (starting complex + economy actually ticking
live) shows no errors; sim-test's new proof passes, determinism stable.
**Not yet manually verified**: watching a freighter actually fly and build
in a real playthrough (the sim-test proof gifts materials to keep runtime
sane — a real game would take a while at the tuned production rates before
the first freighter launches; that pacing itself hasn't been eyeballed for
feel).

**Update (2026-07-21) — staged two-cargo unload**: per playtesting feedback,
a freighter no longer builds instantly on arrival. It now unloads its two
cargo pods one at a time, `FreighterSystem::CARGO_UNLOAD_INTERVAL_TICKS`
(60 ticks / 1s) apart: cargo 1 tops up the target planet's *existing* Base
with `FreighterSystem::CARGO_ONE_RAW_MATERIALS` (25) raw materials if it has
one (a no-op if this freighter's own build order is to construct that Base
— nothing to top up yet); cargo 2 then resolves the build order exactly as
before and consumes the freighter. This is a partial step toward the
original's resupply role (above), scoped to "while already visiting to
build something" rather than a dedicated resupply dispatch. What happens to
a freighter once both cargo pods are gone (today: destructed, same as the
old instant-build behavior) is intentionally not revisited yet — pending
Marcin re-checking the original's behavior. Native + wasm build clean;
`TestFreighterEconomy`'s 3000-tick window comfortably absorbs the added
~2-tick-interval (120-tick) delay per freighter with no assertion changes
needed. Not yet manually playtested.

Done when: claim a fresh planet in a real playthrough and watch the complex
grow unattended; sim-test proof passes.

## Phase 4 — Fighter production, respawn, win/lose

Goal: complexes rebuild your fighter; a round can be won and lost.

- [x] Base self-development: a Base constructs Lab then Comm Center from its
  finished materials; a High Port constructs Space Dock then Sensor Array
  (one at a time, new-unit rule applies).
- [x] Fighter production + respawn: Labs and Space Docks rebuild a faction's
  fighter after death (delay: reuse `RESPAWN_DELAY_TICKS` in `game.hpp`),
  consuming host finished materials. Respawn site = the ship's **last
  friendly landing site** (Phase 1's field) if still alive and friendly;
  else any remaining friendly planet/high port; **none left → that faction
  is out** (for the player: game over).
- [x] Per-faction state (`FactionState`; one entity per team, created
  lazily): last-landing-site NetId, defeated flag. **Server-only for now,
  not yet replicated** (see verification note below).
- [x] Defeat rule: a faction with zero colonies AND zero freighters is
  defeated (nothing regrows; remaining structures stay). Win check: all
  planets claimed by one team. Emit `GameEventType::FactionDefeated` /
  `RoundOver`.
- Sim-test: kill a scripted faction's colonies+freighters ⇒ defeated flag;
  destroy the player fighter with a friendly lab alive ⇒ respawns at the
  last landing site; destroy everything ⇒ game over state.

Done when: a full round is winnable and losable against a do-nothing
opponent faction, verified by hand; sim-test proofs pass.

**Verification status — Base/High Port self-development** (2026-07-21):
`EconomySystem::SELF_DEVELOPMENT_COST` (40 finished materials, placeholder
pending tuning) funds a Base's own Lab then Comm Center, and a High Port's
own Space Dock then Sensor Array, spent from that structure's own
`finishedMaterials` (not delegated to an accompanying structure, unlike
freighter dispatch) — one at a time, new-unit rule re-checked live, same as
freighter construction. Same-planet and instant: no freighter trip, since
`EntitySpawner::SpawnStructure`/`SpawnOrbitingStructure` can place it
directly (Space Dock/Sensor Array reuse the High Port's own current orbit
radius/direction/theta, phase-offset ±0.4 rad, matching
`BuildStartingComplex`'s hand-placed layout). Also fixed a pre-existing bug
found while touching this code: `GameEventType::StructureBuilt`'s doc
comment says `param = StructureType`, but `FreighterSystem`'s emit was
actually passing the `BuildOrder` enum, whose values don't line up with
`StructureType` past index 1 (`BuildOrder::HighPort` = 2 vs.
`StructureType::HighPort` = 4) — harmless today since nothing reads the
param yet, but would have bitten whoever adds the first reader. New
sim-test `TestSelfDevelopment` (bare Base + High Port, no Lab/Comm
Center/Space Dock/Sensor Array, gifted funds) asserts both build-order
sequences. Native, wasm, and sim-test all build/run clean; determinism
checksum unchanged (the default scenario's own starting complex already has
all seven structures, so self-development never triggers there — expected,
not a sign the new code is dead). Not yet manually playtested in a live
game.

**Verification status — FactionState / defeat / win** (2026-07-21):
`FactionSystem` (new, `src/game/system/faction-system.cpp`) owns
`FactionState` entities, one per team, created lazily the first time
`GetOrCreate(team)` is called (from `LandingStateSystem`/`ConquestSystem`
when a ship of that team lands somewhere friendly, updating
`lastLandingSiteNetId`) — not pre-seeded for every `TeamId` value. Runs each
tick right after `DeathSystem`, so its counts reflect this tick's freshest
destructions. Defeat check iterates existing `FactionState` entities (not a
live Structure query) specifically so it still fires even after a team's
last structure of any kind is destroyed — deriving the team set fresh from
Structure ownership each tick would miss that final tick, since there'd be
nothing left to iterate. Win check compares every `Planet`-tagged entity's
`Team`; fires once when they're all the same non-`None` team (sticky, global
`m_roundOver` flag on `FactionSystem`, separate from the per-team `defeated`
flag). New sim-test `TestFactionDefeatAndWin`: two Blue-claimed planets ⇒
`RoundOver(Blue)` fires within 10 ticks; destroying every Blue Colony (with
no Blue freighters in flight either) ⇒ `FactionDefeated(Blue)` fires within
another 10 ticks. Native, wasm, sim-test all build/run clean, determinism
holds.

**Known gap, still deferred**: `FactionState` is server-only — not in
`EntityState`/`SNAPSHOT_VERSION`, so no client can see a faction's defeated
status yet (needed once UI phase U2's "opponent status" swatches are built).

**Verification status — fighter production + respawn** (2026-07-21):
`FactionSystem::TryRespawn(team)` (new) replaces the old unconditional
placeholder-position respawn in both `Game::HandlePlayerRespawn`
(single-player, always `TeamId::Blue`) and `NetServer::HandleRespawns`
(multiplayer, per-peer team). Site selection: the team's
`FactionState::lastLandingSiteNetId` planet if it's still alive and
friendly; else any remaining friendly `Planet`; else any remaining friendly
High Port. Funding: a Base with a co-located Lab, or a High Port with a
co-located Space Dock, belonging to the team, with `>=
FactionSystem::FIGHTER_COST` (30, placeholder pending tuning) finished
materials — spent on success. No site at all → `std::nullopt` permanently
(that faction is out; for the player, this reads as the ship simply never
coming back — no dedicated game-over screen yet, that's UI phase U4's job).
A site but no affordable funder yet → `std::nullopt` transiently; both
`HandlePlayerRespawn` and `HandleRespawns` just keep retrying every
subsequent tick past the original `RESPAWN_DELAY_TICKS` timer, rather than
resetting or giving up.

**A real bug found and fixed while wiring this in**: `FactionSystem::
GetOrCreate` can create a flecs entity (a fresh team's first `FactionState`)
— a structural change that isn't safe from inside an active
`m_registry.each()` iterator. It was being called nested like that from
three places (`FactionSystem::Update`'s own team-discovery loop,
`LandingStateSystem::Update`, `ConquestSystem::Update`), and reliably
segfaulted `gravitaris-sim-test` — but only in the long-running, many
-archetype shared `Game` instance `TestPeerRespawn`/`TestTeamAssignment`/etc.
use (1800+ ticks of accumulated state before these tests run), not in any of
the short-lived single-purpose `Game` instances other tests construct fresh
— which is why it didn't show up until this specific combination. Fixed with
a collect-then-create pattern in all three: gather the set of teams needing
a `FactionState` (or, for landing/claiming, the `(team, landedOnNetId)`
pairs to apply) during the read-only `.each()`, then call `GetOrCreate` in a
plain loop afterward, once no iterator is active. Also had to fix a
pre-existing sim-test, `TestTeamAssignment`: it reassigns a peer to
`TeamId::Cyan` (a team that owns nothing) and expected an unconditional
respawn — under the new site+funding rule Cyan legitimately has nowhere to
respawn (same as any other faction with no complex), so the test now gifts
Cyan a minimal Base+Lab first, preserving its original intent (a respawn
keeps the reassigned team, not the original request). Native, wasm, and
sim-test all build/run clean; determinism holds; the crash does not
reproduce after the fix.

Phase 4 is now fully implemented server-side; only the client-visibility gap
above remains.

## Phase 5 — AI opponents (strategy layer)

Goal: enemy leaders play the same game: scout, claim, defend, intercept.
This is `docs/ai-ships.md`'s deferred Strategy layer — read that doc's
architecture table first; tactics/guidance/control already exist.

- [ ] **Land guidance behavior** (the known GNC gap — fighters only;
  freighters orbit): add to `game/gnc/guidance/behaviors.hpp/.cpp` —
  approach the planet surface point, cap descent speed by flip-and-burn
  stopping distance against gravity (reuse `GotoPoint`'s solved-velocity
  idea), touch down below Phase 1's safe threshold. Riskiest item in the
  plan — budget for iteration; prove it in sim-test with a scripted descent
  before wiring it into strategy.
- [ ] `AIStrategy` (component on AI leader fighters + a system): utility
  scorer over goals — ClaimNearestUnowned, AttackWeakestEnemyComplex,
  InterceptEnemyFreighter, DefendOwnComplex, plus the existing dogfight
  tactics when engaged. Personality presets weight the scores
  (`AIPersonalityPreset` exists in `game/gnc/ai-personality-presets.hpp`;
  the original's preset names — Aggressive, Determined, Shrewd, Tenacious,
  Voracious, Defensive, Maniacal — can extend it).
- [ ] AI factions get a starting complex + fighter; claiming triggers the
  same freighter/complex machinery as the player (no special-casing — if a
  rule needs faction-specific branching, the rule is wrong).
- Sim-test: run a headless round a few thousand ticks with 2 AI factions;
  assert at least one new planet gets claimed by an AI; checksum stability
  across two runs (this doubles as the mode's determinism proof under full
  AI load).

Done when: a solitaire round against AI leaders is genuinely contested.

## Multiplayer wiring (parallel track — required before the first vs-humans round)

The sim side needs nothing special: every rule above is headless `game/`
code the server already runs, conquest state replicates via the existing
per-entity `teamId`, and one-shots ride the `GameEvent` stream. The real
gaps are the known netcode ones plus team assignment:

- [x] Per-peer team assignment — `ClientHelloPacket.requestedTeam` /
  `ServerWelcomePacket.yourTeam` (protocol bumped to v2); `NetServer`
  honors an explicit request or round-robins a `{Blue, Red}` default roster
  (free versus for the common 2-player case with zero setup), remembers
  each peer's team across respawns (`PeerState::team`), and exposes
  `SetPeerTeam` for explicit reassignment (wired to a `team <peer-id>
  <color>` `gravitaris-server` console command). Same team = co-op (already
  supported — multiple fighters per team was always fine, nothing gated on
  "exactly one ship per team"). AI leaders aren't wired to this yet (no AI
  -side team selection exists) but share the same `Team` component, so
  nothing blocks it. Verified live (two native clients round-robin to
  different teams) and in sim-test (`TestTeamAssignment`).
- [x] Client consumes replicated `SnapshotData::events` (net task #34) —
  done.
- [x] Peer respawn loop (net task #33) — done; still respawns at a fixed
  point rather than "the ship's last friendly landing site" (Gravity Well
  Phase 4's rule) since Phase 4 hasn't landed yet.
- [ ] Round setup over the network (which team each peer joins; AI fill) —
  UI side lives in U4's setup screen.

## Phase 6 — Sector generation (deliberately late)

Goal: replace the fixed two-sun arena with a deterministic, parametrized
sector once the mode is proven on the known layout.

- [ ] `src/game/scenario/sector-scenario.cpp`:
  `BuildSectorScenario(EntitySpawner&, const SectorParams&)` with
  `SectorParams { uint32 seed; int stars (2–6); int minPlanetsPerStar=1,
  maxPlanetsPerStar=3; }`. Splitmix on the seed for star placement (use the
  classic scenario's ±5600 spacing as the scale reference), per-star planet
  counts, orbit radii/phases/directions.
- [ ] Starting-planet selection: one per faction, far apart (max-min
  distance greedy pick), each seeded with the Phase 2 starting complex.
- [ ] Entry point: seed selectable at round start (U4's setup screen, or a
  CLI/debug option until then).
- Sim-test: same seed twice ⇒ identical checksum; different seed ⇒
  different layout; planet counts within bounds.

Done when: rounds play on generated sectors with all phases 1–5 intact.

## UI phases (parallel track — U1/U2 can start any time after Phase 1)

The information set matches the original (see the UI reference in
`gravity-well-1997.md`); the look should be *better*, not a clone. Real game
UI is RmlUi (`data/hud.rml` etc., drawn through the CRT/bloom pipeline when
`m_uiInWorld`); Dear ImGui is debug-only — don't build player-facing UI in
ImGui.

**Skipped by decision**: the original's directional dials (nearest
star/planet/enemy pointers) — `IndicatorRenderer`'s world-anchored arrows
already cover that job. Don't build a dial cluster.

### U1 — Sector map (minimap grown into the radar role)

Extend the existing `MinimapRenderer` (`src/cgame/renderer/minimap-renderer.
cpp` — note task #36's caveat about which world it reads in net-client
mode). Match the original's information, but **circles/points, not
squares**:

- [ ] Whole-sector extent; stars vs planets visually distinct (size/color).
- [ ] Claimed planets get a **team-colored ring** around their dot.
- [ ] Ships as small team-colored dots; freighters in one neutral color
  regardless of owner (the original's "commerce is neutral" rule — it reads
  well).
- [ ] Selected-unit highlight (ring/pulse) once U3's selection exists.

### U2 — Status cluster (fighter vitals; inspired, not copied)

RmlUi panel; data flows CGame → RML via a small per-frame update (keep the
cgame-side glue in one file, e.g. `src/cgame/ui/hud-model.cpp`).

- [ ] Hull/damage readout from the player's `Damageable`.
- [ ] Velocity readout with an explicit **"safe to land" state change**
  (color/glyph swap below Phase 1's threshold) — this is a gameplay aid,
  not decoration; it's how the original teaches landing.
- [ ] Opponent status: one swatch per faction, dimmed when defeated
  (Phase 4's flag).
- [ ] Reserved (empty for now): shields, munition counts.
- Design freedom: layout/style at implementer's discretion — aim for
  something that fits the vector/CRT look; take the original only as the
  checklist of *what* to show.

### U3 — Unit list + selection

- [ ] RmlUi scrolling list of your units (not the fighter): name + number
  (Space Dock/Sensor Array suffixed by their High Port's number, per the
  original), damage bar scaled by relative max HP, materials bar (raw for
  colonies/freighters, finished for bases/high ports — Phase 3's stores).
- [ ] Selection: click a list entry to make the camera follow that unit
  (`CameraDirector` already follows an entity); any flight input snaps back
  to your fighter; selected unit highlighted in list + minimap.
- [ ] **Deferred**: spectating enemy leaders (F2–F4 in the original) —
  later, it's trivial once selection exists and mostly useful for AI
  debugging.

### U4 — Round flow + main menu (later, but eventually required)

- [ ] Round setup screen: seed + per-AI personality preset (the original's
  Opponents dialog); RmlUi.
- [ ] Round-over screen on `RoundOver` (win/lose + restart).
- [ ] Eventually a proper main menu (new round / settings / quit) — out of
  slice scope, tracked here so it isn't forgotten.

## Suggested build order

Phase 1 → 2 → U2 (landing aid pays off immediately) → 3 → 4 → U1/U3 → 5 →
6 → U4. Freighter/economy (Phase 3) is the biggest phase; the Land guidance
behavior (Phase 5, fighters only) is the highest technical risk.
