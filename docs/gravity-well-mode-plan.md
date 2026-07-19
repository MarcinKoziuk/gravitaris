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

## Scope decisions (settled with Marcin 2026-07-19 — don't relitigate)

- **Single-player first** vs AI opponents. Multiplayer conquest is out of
  scope, but every rule below runs in `game/` (headless,
  server-authoritative later) so it composes with the netcode for free.
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
- **Win/lose**: win when every planet is claimed by you (destroying enemy
  complexes as needed); the original's defeat rule applies to factions (all
  colonies + freighters destroyed = can no longer expand = defeated).
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

- [ ] Safe-landing detection in `game/`: a ship is *landed* on a planet when
  in contact with it, relative speed below a threshold, and surviving (the
  hard-contact damage path already exists — `LandingCrash` in
  `src/game/system/damage-system.cpp`; read it first, the new logic should
  live near/with the same contact processing rather than duplicating it).
  Track it as a field (e.g. on `Controls` or a new small component on
  ships), not an add/remove tag.
- [ ] Track per-ship "last friendly landing site" (NetId field, same
  component) — Phase 4's respawn rule reads it.
- [ ] Claiming: new system (e.g. `src/game/system/conquest-system.cpp`) —
  when a ship with a `Team` is landed on a planet whose `Team` is absent or
  different AND the planet has no living enemy structures, set the planet's
  `Team` to the lander's. Emit a new `GameEventType::PlanetClaimed`
  (param = team id) so UI/audio can react.
- [ ] `Team` on planets must replicate: extend `EntityState` +
  `GatherSnapshot`/`SerializeSnapshot`/`ReadSnapshot`
  (`src/game/net/snapshot.cpp`) with the owner team (bump
  `SNAPSHOT_VERSION`; follow the GravitySource-replication commit pattern,
  `git log --grep=GravitySource`).
- [ ] Landing aid: surface "slow enough to land" to the player — UI phase U2
  does it properly; until then a debug-panel readout is fine for verifying
  this phase.
- Sim-test: place a ship at rest on a planet (or script a descent) and
  assert landed=true, claim happens, event emitted; assert a fast contact
  still damages and does NOT claim.

Done when: you can claim a planet in a real playthrough and the sim-test
proof passes.

## Phase 2 — Structures

Goal: all seven structure types exist as spawnable, damageable, team-owned
entities; a hand-assembled starting complex spawns at the player's starting
planet.

- [ ] `Structure` component (replicated): `{ StructureType type; }` enum
  Base/Colony/Lab/CommCenter/HighPort/SpaceDock/SensorArray, plus the
  materials-store fields Phase 3's economy reads (`float rawMaterials,
  finishedMaterials` — unused until then; keeping them here avoids a second
  snapshot-version bump).
- [ ] Planetside structures: static bodies placed at fixed offsets around
  the planet (the original draws them as boxes within the planet outline);
  `Team`, `Damageable`, `Transform`.
- [ ] Orbital structures (High Port, and its attached Space Dock / Sensor
  Array): kinematic `Orbit` around the planet, exactly like planets orbit
  suns (IDEAS.md explicitly wants orbiting bases kinematic). Attachment =
  same orbit at small phase offsets.
- [ ] Simple SVG models under `data/models/structures/…` (copy the authoring
  format of an existing model; visual polish later).
- [ ] `EntitySpawner::SpawnStructure(type, planet, …)` following the
  existing Spawn* patterns (NetId assignment etc.).
- [ ] Defenses: Bases and High Ports fire at enemy ships in range on a
  cooldown (reuse `SpawnBullet` sensor bullets + `DamageSystem`; lead the
  target the way `AIPilotSystem`'s firing solution does — read that first).
- [ ] Hand-assembled starting complex (Base, Colony, Lab, Comm Center, High
  Port + attachments) spawned at the player's starting planet in the
  scenario.
- Sim-test: spawn a full complex headless; assert structures exist with
  correct teams/orbits; a scripted enemy ship in range gets shot at;
  checksum stable.

Done when: the starting complex renders, orbits, and shoots back in a real
playthrough; sim-test proof passes.

## Phase 3 — Freighters + materials economy + build loop

Goal: the original's automated economy, physically: colonies produce, 
freighters haul and build, bases convert. Claiming a fresh planet results —
hands-off — in a complex growing there. The heart of the mode.

- [ ] Freighter model: new SVG+YAML under `data/models/ships/freighter-0/`
  (hull + two visible cargo pods, per the annotated screenshot). Slow: own
  `GuidanceParams` (low `maxSpeed`/`accel`).
- [ ] Freighter movement: GNC `GotoPoint`/`InterceptEntity` for the transit
  leg; **on arrival, snap into a kinematic `Orbit` around the target planet**
  (no landing, no Land behavior needed). Departure = leave the orbit, resume
  GNC flight. Keep transitions deterministic.
- [ ] Economy rules, one system (`src/game/system/economy-system.cpp`),
  mirroring the manual's model (see gravity-well-1997.md "Economic model"):
  - Colony produces raw per tick; supplies its Base and the High Port
    overhead.
  - Base/High Port convert raw → finished at a rate; repairs (structures,
    docked/landed fighters) consume finished from the host.
  - Freighters in orbit at a colony planet load raw; at a needy planet
    (no colony) unload; a **full** freighter at a friendly claimed planet
    missing a structure **builds it — Base → Colony → High Port order — and
    is consumed** (entity destroyed, structure spawned, emit
    `GameEventType::StructureBuilt`).
  - New-unit rule: only build if no living unit of that type is already
    on/above that planet.
- [ ] Freighter brain: small state machine component + system
  (`src/game/system/freighter-system.cpp`): Idle-in-orbit → LoadAtColony →
  Transit → OrbitTarget → (unload | build). Target selection: nearest
  friendly planet needing supply or construction, claimed-but-undeveloped
  first; break distance ties by NetId (determinism).
- [ ] Freighter production: Labs and Space Docks build freighters from their
  host's finished materials when a friendly planet needs one and no friendly
  freighter is already tasked to it.
- Sim-test: seed a developed complex + one claimed empty planet; run N
  ticks; assert Base then Colony then High Port appear in order, freighters
  were consumed, materials actually flowed (colony store drained into
  builds); two-run checksum stable.

Done when: claim a fresh planet in a real playthrough and watch the complex
grow unattended; sim-test proof passes.

## Phase 4 — Fighter production, respawn, win/lose

Goal: complexes rebuild your fighter; a round can be won and lost.

- [ ] Base self-development: a Base constructs Lab then Comm Center from its
  finished materials; a High Port constructs Space Dock then Sensor Array
  (one at a time, new-unit rule applies).
- [ ] Fighter production + respawn: Labs and Space Docks rebuild a faction's
  fighter after death (delay: reuse `RESPAWN_DELAY_TICKS` in `game.hpp`),
  consuming host finished materials. Respawn site = the ship's **last
  friendly landing site** (Phase 1's field) if still alive and friendly;
  else any remaining friendly planet/high port; **none left → that faction
  is out** (for the player: game over).
- [ ] Per-faction state (`FactionState`, replicated; one entity per team or
  a singleton array): last-landing-site NetId, defeated flag.
- [ ] Defeat rule: a faction with zero colonies AND zero freighters is
  defeated (nothing regrows; remaining structures stay). Win check: all
  planets claimed by one team. Emit `GameEventType::FactionDefeated` /
  `RoundOver`.
- Sim-test: kill a scripted faction's colonies+freighters ⇒ defeated flag;
  destroy the player fighter with a friendly lab alive ⇒ respawns at the
  last landing site; destroy everything ⇒ game over state.

Done when: a full round is winnable and losable against a do-nothing
opponent faction, verified by hand; sim-test proofs pass.

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
