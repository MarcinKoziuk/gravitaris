# AI ships — design and implementation plan

Status: draft plan (2026-07-13). Companion to `slice-components.md` (which names
`AIPilot`, `AIStrategy`, `InputQueue`) and `adr/0001-netcode-model.md` (whose
constraints apply to everything here — AI is a "client" on the same command
interface as networked players).

## Goal

Autonomous NPC ships (opponents, later freighters/colony ships per
`gravity-well-1997.md`) that fly competently in the n-body gravity field using
the same actuators as the player: torque-limited rotation + single rear
thruster + weapons. `slice-components.md` flags gravity-aware pilot AI as the
highest-risk item in the slice; this plan front-loads that risk (phases 1–3)
before any strategy AI.

## Prior art / architecture choice

The layered idea (reflexes → tactics → strategy, à la quake3 bots) is right,
but q3's concrete layers are built around navmesh pathfinding in static
geometry and don't map to continuous space + gravity. Better-fitting
decomposition: **GNC (Navigation / Guidance / Control)** from aerospace — also
how KSP's MechJeb autopilot is structured, which is the closest real prior art
("autopilot with realistic actuators in a gravity field"):

| Layer | Question | Output | This codebase |
|---|---|---|---|
| Navigation | Where am I / where will I be? | predicted trajectory samples | `TrajectoryPredictor` (new, `game/`) |
| Guidance | What velocity do I want now? | desired velocity (or heading + burn) | behaviors: KillVelocity, GotoPoint, Orbit, Intercept, Evade, Land |
| Control | How do I actuate that? | tick command (`Controls` bits) | `FlightController`: PD on heading + bang-bang thrust |
| Tactics | Which behavior, which target? | active guidance behavior | utility scorer in `AIPilotSystem` |
| Strategy | Build/attack decisions | goals for tactics | `AIStrategy` + `AIStrategySystem` (built in gravity-well-mode-plan.md Phase 5) |

Notes:
- Classic Reynolds steering behaviors assume omnidirectional thrust — our
  ships must rotate before burning, so seek/arrive don't transplant directly.
  The GNC split absorbs that: guidance outputs desired velocity *change*,
  control owns the rotate-then-burn reality (including flip time in stopping
  distance).
- For tactics, skip q3's fuzzy relations; a small utility scorer (score each
  candidate behavior, run the max) is simpler to write and debug.

## The 3+ body problem — prediction approach

Two simplifications make this cheap:

1. **Restricted problem, not full n-body.** Planets outweigh ships by orders
   of magnitude; predict each ship as a *test particle* against planets only
   (currently static; if they ever orbit, precompute their paths once per
   tick and share). Ship–ship attraction is negligible for planning even
   though the real sim includes it.
2. **Prediction is cheap enough to run synchronously.** 10 s horizon at the
   60 Hz sim step = 600 iterations of `F = G·m/d²` per planet — sub-µs per
   ship. Even ~20 AI ships replanning is well under a millisecond, and ships
   replan every N ticks, not every tick. Start synchronous with a time
   budget; move to a background thread (double-buffered results) only if
   profiling ever demands it (phase 7).

**Accept prediction error; don't fight it.** The hand-rolled predictor will
drift from Chipmunk (floats per ADR 0001; the sim applies gravity forces
*after* `cpSpaceStep` in `physics-system.cpp:~230`, i.e. forces land on the
*next* step; no softening term near `dist→0`). The fix is MPC-style:
**replan from actual state every 10–30 ticks** and give guidance tolerance
bands, rather than chasing an exact match. The predictor should mirror the
sim's force law (`G = 20`, `F = G·m₁m₂/d²`) and semi-implicit Euler so the
drift stays small over a few seconds.

## Integration points (as of 2026-07-13)

- Control seam: `Controls` bitfield (`include/gravitaris/game/component/controls.hpp`)
  consumed by `ShipControlsSystem` (`src/game/system/ship-controls-system.cpp:64`) —
  thrust force 140 local −Y, torque ±20 clamped to 15 rad/s, angular damping
  `−4·ω` every tick (the damping helps the PD controller).
- Tick order: `Game::Update()` (`src/game/game.cpp:26`). `AIPilotSystem` runs
  before `ShipControlsSystem`, matching slice-components tick steps 1–2.
- Spawning: AI ships are `Transform` + `RigidBodyDesc` (+ `AIPilot`); physics
  comes free via the `RigidBodyDesc` observer (ADR 0002).
- All AI code lives in `src/game/` (headless, server-only per ADR 0001); AI
  internals (predicted paths, behavior state) are server-only components,
  never serialized.
- Aiming is easy: bullets are gravity-immune (excluded in
  `physics-system.cpp:~210`), muzzle speed 200 + inherited ship velocity, so
  firing solutions are straight-line lead pursuit.

## Phases (each independently testable)

**Phase 0 — tick-stamped command queue** (prerequisite, from ADR 0001 §4).
*Status: implemented 2026-07-13.* Files: `ControlFlags` extracted in
`component/controls.hpp`; `InputCommand` (`game/input/input-command.hpp`) with
1-byte flag packing; `InputQueue` component (`component/input-queue.hpp`);
`InputSystem` (`system/input-system.cpp`) drains the current tick's command →
`Controls`, wired into `Game::Update()` before `ShipControlsSystem`;
`Game::GetStep()` added; player spawns with an `InputQueue`. Client
(`client/gravitaris.cpp`) keeps a live `ControlFlags`, and `FeedInput()` pushes
one command per tick before `Update()`. `InputLog` (`game/input/input-log.cpp`)
saves/loads a binary command stream; debug keys **F5** toggle record (writes
`input-replay.grinput` in the cwd on stop), **F6** load+replay, **F7** stop
replay. Behavior change: primary fire moved from Down-release to Down-press
(now a clean one-shot). NOT done: `while`-loop fix in `tickEvent`, headless
replay test, in-overlay record/replay UI, routing through a shared per-tick
command collection for multiple controlled ships (only the player has an
`InputQueue` today).

Introduce `InputQueue`: per-controlled-ship queue of tick-stamped command
structs (payload ≈ today's `Controls::actionFlags`, room for analog values
later). Keyboard handling in `client/gravitaris.cpp` enqueues commands
instead of writing `Controls` directly; a small system drains the current
tick's command into `Controls` before `ShipControlsSystem`. Add a debug
record/replay: dump commands to file, replay them into a fresh `Game`.
- Buys now: input replays for AI debugging, the seam AI pilots feed from day
  one, and it discharges ADR 0001 point 4 early.
- Explicitly NOT in scope: a general pub/sub event bus (deaths/explosions/
  hits). Useful later for sim→presentation decoupling and reliable network
  events; build it when there's a consumer.
- Replay caveat: input-log replay reproduces a run only on the same
  build/machine (float non-determinism, ADR 0001) and only while ADR 0001 §5
  holds (no wall clock / unseeded RNG in sim systems). Same-machine replay is
  the debugging use case, so this is acceptable. Cross-machine replays would
  need snapshot recording — out of scope.
- Watch item: `tickEvent` (`client/gravitaris.cpp:116`) steps at most one
  `Update()` per frame (`if`, not `while`), so sim time falls behind wall
  time under load. Harmless for replay (replay is tick-indexed) but worth
  fixing for its own sake.

**Phase 1 — trajectory predictor + debug draw.**
*Status: implemented 2026-07-13.* Files: `TrajectoryPredictor`
(`game/gnc/nav/trajectory-predictor.{hpp,cpp}` — game/gnc/ is the GNC
navigation/guidance/control code): symplectic Euler against all non-bullet bodies sampled once and held
static, force law shared with the sim via the now-public
`PhysicsSystem::GRAVITY_CONSTANT`; owned by `Game`
(`GetTrajectoryPredictor()`). Debug draw: "Trajectory" tab in the F1 overlay
(`cgame/ui/debug/trajectory-panel.{hpp,cpp}`) — predicted player path as an
ImGui background polyline (horizon/stride sliders), drawn whenever the
overlay is open, plus a **live drift meter** (prediction captured, then
compared against actual position as ticks elapse — the in-app stand-in for
the headless drift test). Verified visually: path anchors at the ship and
runs radially into the planet (correct for a from-rest spawn; predictor has
no collision, so it passes through — expected divergence at contact). NOT
done: headless drift regression test (needs a game-lib/test-target CMake
split first), path rendering outside the debug overlay (player-facing orbit
preview), moving-source support.

Original phase description: `TrajectoryPredictor` in
`game/`: forward-integrate a test particle against planet point masses
(semi-implicit Euler, sim's G and force law), configurable horizon/step,
returns sampled positions. Render the *player's* predicted path in the F1
debug overlay (cgame reads the samples; doubles as a future player-facing
orbit preview). Eyeball predicted-vs-actual drift here before any AI exists;
add a headless test asserting bounded drift over ~2 s for a ballistic ship.

**Phase 2 — control layer.**
*Status: implemented 2026-07-13.* Files: `FlyToVelocity` +
`HoldPositionDesiredVelocity` (`game/gnc/control/flight-controller.{hpp,cpp}`):
pure functions (state, target, params) →
`ControlFlags`; PD on heading error (bang-bang rotate bits via turn
deadband), thrust gated on aim tolerance + velocity deadband. `Transform`
gained `angVel` (synced from Chipmunk) so the controller needs no physics
access. Player autopilot harness: `AutopilotMode` (Off/KillVelocity/
HoldPosition) on `CGame`, toggled with **K**/**P** in game (manual movement
input disengages), autopilot overrides movement bits but keyboard fire still
merges. "Flight" tab in the F1 overlay: mode radio + live sliders for all
gains + speed/angVel/anchor-distance telemetry. Verified by instrumented
run: engaged KillVelocity during a 66 u/s free-fall — arrested to 0.3 u/s
within 1 s, then hovered 14 s deep in the well with a 0.1–1.8 u/s bang-bang
limit cycle and ~1.5 units total drift. NOT done: flip-and-burn stopping-
distance awareness (deferred to GotoPoint in phase 3, where it matters),
desired-velocity vector overlay draw.

Original phase description: `FlightController` (plain function/struct, no
system yet): (current state, desired velocity) → command bits. PD on heading
error (tuned against the existing torque/damping model), thrust when heading
is within tolerance and velocity error exceeds a deadband. Includes
flip-and-burn awareness: effective stopping distance = turn-around time +
burn time. Test as a player autopilot on debug keys: "kill velocity" and
"hold position". This phase is where the feel gets tuned, isolated from AI
decisions.

**Phase 3 — guidance behaviors.**
*Status: implemented 2026-07-13 (except Land).* Files:
`game/gnc/guidance/behaviors.{hpp,cpp}`: `GotoPoint` (arrive with flip-and-burn
stopping distance solved from dist = v·flipTime + v²/2a), `OrbitBody`
(circular-orbit speed √(GM/r) at current radius + clamped radial correction
toward target radius), `InterceptEntity` (dead-reckoned lead pursuit + the
target's lateral velocity, see phase 5), `EvadeBody` (radial climb
preserving tangential motion). Gravity is otherwise handled reactively by
the control layer. `ShipControlsSystem::THRUST_FORCE` made public so
`CGame` sets `GuidanceParams.accel` from real ship mass on engage. Harness:
autopilot modes **G** (goto target, editable in Flight tab) and **O**
(orbit heaviest body at engage radius, keeping current rotation sense);
overlay markers (target cross, orbit ring, anchor circle) via a shared
`WorldToUi` helper; guidance sliders in the Flight tab. Verified by
instrumented run: GotoPoint engaged during a 66 u/s free-fall arrested,
cruised at the 80 u/s cap, decelerated and held 1.8–4.6 units from the
target; Orbit held radius 302–307 (target 306) for 15 s at the theoretical
40 u/s circular speed. NOT done: `Land` (next), predictor-based drift
compensation in GotoPoint (reactive control suffices so far), headless
scenario tests (still needs game-lib split).

Original phase description: Each ≈ a pure function
(world, self, target) → desired velocity: `KillVelocity`, `GotoPoint`
(gravity-compensated via predictor: aim at drift-corrected point, respect
stopping distance), `OrbitBody`, `InterceptEntity`, `Evade` (incl. "falling
into well" escape), later `Land` (retro-thrust descent — core Gravity Well
skill, hardest behavior; fine to defer past phase 5). Headless tests: scripted
scenarios asserting "reaches point within R by tick N", "doesn't crash into
planet".

**Phase 4 — first real AI ship.**
*Status: implemented 2026-07-14.* Files: `AIPilot` component
(`component/ai-pilot.hpp` — behavior enum, target entity, fire/decision
cooldowns, per-ship flight+guidance params) and `AIPilotSystem`
(`system/ai-pilot-system.{hpp,cpp}`), running in `Game::Update()` between
physics write-back and `InputSystem` so its tick-stamped command is consumed
the same tick. Utility selector on a 15-tick decision interval: predictor
lookahead (2 s) flags well danger → `EvadeBody`; target inside 500 units →
`InterceptEntity` (with a 50-unit standoff arrive radius); else `OrbitBody`
patrol at the engage radius; else idle. Firing: quadratic lead solution vs.
the 200 u/s muzzle speed, gated on ±0.12 rad alignment, 250-unit range and a
30-tick cooldown owned by the pilot. `EntitySpawner::SpawnAIShip` +
"Spawn AI fighter near player" button in the debug Spawn panel
(`Game::GetEntitySpawner()` exposed). Verified by instrumented 40 s run:
spawned 292 units out, intercepted at ~98 u/s, chased the free-falling
player into the well, broke off when the predictor flagged danger (planet
distance bottomed at 68, never crashed), re-engaged repeatedly closing to
16–20 units, and fired when aligned (bullet count pulsing 0→1 with the
3 s lifetime). NOT done: damage/health (bullets bounce — slice work),
`Orbit` patrol untested in-log (player always in engage range), difficulty
knobs (phase 5), routing AI target refs through NetId.

Original phase description: `AIPilot` component (behavior enum + state
+ target NetId) and `AIPilotSystem` in `Game::Update()` before the command
drain. Utility selector v1: crashing into well → Evade; player in range →
Intercept + lead-pursuit firing; else patrol/orbit. Feed commands through
`InputQueue` like any client. Debug overlay: draw active behavior, desired
velocity vector, predicted path per AI ship.

**Phase 5 — tactics polish.** Better target selection, weapon discipline
(don't fire through planets — segment-vs-circle check), evasive maneuvers
under fire, difficulty knobs (reaction delay, aim error — quake3-style
humanization).

*Status: done 2026-07-28.* Weapon discipline: `AIPilotSystem` tests the
firing solution (ship → lead point) against every gravity source's disc plus
`SHOT_CLEARANCE` before setting `firePrimary`; a blocked shot holds fire and
spends no cooldown, since bullets are gravity-immune and a body on the line
simply eats them. Evasion under fire: a hull drop between ticks is the
"being shot at" signal (no event needed — damage resolves before this system
runs), buying `underFireTicks` of jinking, during which `Intercept` adds a
lateral weave across the line of sight that reverses every `jinkPeriod`
ticks, phase-offset per entity so a wing doesn't weave in lockstep.
Breaking off: `AIBehavior::Flee` + `FleeThreat` guidance (full cruise away
from the threat, solved in its frame). A pilot under `fleeHealthFraction` of
max hull stops picking `Intercept` at all and runs from any threat inside
`fleeRange`; hysteresis is on range (`fleeMargin`), not hull, because
nothing repairs a ship — the hull fraction never climbs back, so opening the
gap is the only exit, after which it patrols rather than re-engaging. Evade
still overrides Flee (a well kills faster than a fighter does). All six
knobs are per-personality and in `data/ai-presets.toml`: Reckless sets
`flee_health_fraction = 0` (never breaks off), Sniper flees earliest and
jinks least. Difficulty knobs landed earlier as the preset library itself.
Verified by `TestAITactics` in `tools/sim-test`: fire held with a planet on
the solution vs. fire opened without it, straight run vs. weave after a hit,
range opened at 15% hull vs. closed at 100%, and the crowding case below.

*Multiplayer wiring + cruise speed (2026-07-28).* Three things made the AI
look inert in an MP session, none of them tactical:

- **No strategy layer existed there at all.** `m_aiFactions` was only ever
  populated by `Game::SpawnCombatants`, i.e. by `Game::Start()`, which a
  dedicated server deliberately skips — so no entity carried an `AIStrategy`
  and `AIStrategySystem` iterated nothing. Extracted `Game::AddAIFaction`
  from `SpawnCombatants`; the server calls it for Red at startup and exposes
  `ai <color> [preset]` for the rest.
- **Fodder never received orders.** `SpawnAIShip` ships have no `AIStrategy`,
  so `order.kind` stayed `None` for life and only the single leader acted on
  a goal. `AIPilotSystem` now propagates the team leader's order to pilots
  without an `AIStrategy` of their own (lowest NetId wins if a team fields
  two, so it doesn't ride on iteration order). A `Land` order is handed on as
  `Patrol` — a claim is one ship setting down, and the wing covers the
  descent rather than piling onto the rock.
- **Intercept crawled.** `InterceptEntity` clamped to `maxSpeed` (80), a
  dogfighting figure, while `transitSpeed` (400) only reached `GotoPoint` /
  `LandOnBody` — so a pilot engaging at the 6000-unit `engageRange` took 75
  seconds to arrive. The cap is now `maxSpeed` plus whatever a flip-and-burn
  can still bleed off before `mergeRange` (new `GuidanceParams` field, 500),
  solved like `GotoPoint`'s arrival speed with `maxSpeed` rather than zero as
  the arrival target. The lead point dead-reckons off that same figure.

*Ordered attacks vs. engageRange (2026-07-28, from a live session showing a
Red leader parked at goal `InterceptFreighter` doing nothing).* The tactical
pick gated its `Intercept` branch on `engageRange` even when an `AIOrder`
named the subject outright, so a leader ordered onto a freighter or an enemy
complex further off than 6000 units fell through to the `Orbit` patrol branch
and circled a well while still reporting the goal. `engageRange` is how a
pilot picks a dogfight opponent out of whatever is nearby; an ordered attack
now ignores it. Covered by `TestAITactics` (the raid case, which reproduced
the stall before the fix).

The throttle dwell was *not* a factor: `urgentFactor` abandons a coast above
9 u/s of velocity error, which holds for any transit.

*Phase 6 note (same date):* `AIStrategySystem` now scores a subject already
spoken for by n other leaders at `1/(1+n)` of its worth (Dogfight exempt —
converging on one enemy fighter is fine, two ships landing on one rock is
not), and leaders decide in NetId order with each pick landing in the
assignment map before the next reads it. The ordering is what makes that
sequence reproducible; a snapshot taken before the pass would let a whole
wing — all of whom spawn with a zero decision cooldown and so decide on the
same tick — claim the same planet.

*Somewhere to be (2026-08-02, from a live session where most AI ships read
`Dogfight` / `Orbit` for minutes at a time).* Three separate things all
ended at the same patrol ring, and it was the honest report of an idle
pilot, not a bug in the patrol:

- **`engageRange` was a cutoff.** The strategy layer scored a dogfight on a
  3000-unit falloff while the tactical layer refused to fly one past 6000,
  so between those figures a leader committed to a fight it would not
  travel to. The gate is gone: any named enemy is flown to, and
  `engageRange` is now the falloff the strategy layer scores dogfights with
  — how far a fight is worth travelling, weighed against everything else
  the pilot could be doing, rather than a wall. `Dogfight` also issues a
  real `Attack` order now, which is what makes the tactical layer treat it
  as a destination. That order is deliberately *not* propagated to the
  wing (nor is `Rearm`): the leader's pick is one enemy out of many and the
  wing has its own nearest.
- **`hurt` was permanent.** Nothing restored hull, so a pilot that fell
  under `fleeHealthFraction` could never pick `Intercept` again and
  patrolled out the round. `RepairSystem` gives hull back to any ship —
  player included — standing on one of its faction's developed planets (a
  Base *and* a Colony, the same pairing a respawn needs; `IsHomePlanet` is
  now the single definition of that, shared with `FactionSystem`).
- **Nothing sent it there.** `AIGoal::Rearm` lands a leader at its nearest
  home planet, scaled by damage and decisive below the flee threshold
  (`REARM_URGENCY` deliberately exceeds every other goal's ceiling of 1).
  Pilots without an `AIStrategy` reach the same trip from `AIPilotSystem`,
  which writes them the same `Land` order in place of the wing's.

The same trip carries the shopping list: a fresh pilot launches wanting
`AIPersonality::upgradeGreed` upgrades and holds its pad until
`ResearchSystem` has handed them over, `padWaitTicks` expires, or a threat
turns up overhead. `sniper`/`cautious` want two and arrive late and
overgunned; `reckless` has `pad_wait_ticks = 0` and `rearm = 0`, so it
never shops and never goes home to patch up — the ground-side reading of
the same temperament its `flee_health_fraction = 0` already states.

Covered by `TestRepairAndReachability`.

*Partly done 2026-07-26, fixing "AI runs away at full speed when chased":*
`InterceptEntity`'s velocity feedforward now matches only the target's
lateral motion, not its line-of-sight motion (mirroring a pursuer's charge
made the AI accelerate away along the pursuer's own velocity vector, above
its cruise cap since the sum was unclamped — it is clamped to `maxSpeed`
now). Targets are also re-picked on the decision cadence rather than only on
target death, with a hysteresis ratio, so a pilot no longer cruises past its
attacker toward a stale distant lock. Deliberate, disadvantage-driven
retreat (return to base to repair, fall back to defend home) is still
unimplemented — there is no Flee behavior at all, which is the point: every
retreat seen today is one of these two accidents. (Deliberate retreat exists
as of 2026-07-28 — see the phase-5 status above. There is somewhere to
retreat *to* as of 2026-08-02: `RepairSystem` and `AIGoal::Rearm`, below.)

*Thrust vs. gravity, and two silent guidance layers (2026-07-29), fixing "AI
ships are stuck on Depart".* The pilots were flying it correctly; nothing
could take off. Thrust was a global 140 against a standard planet's 155–162
units/s² of surface gravity, i.e. a thrust/weight of 0.9: a landed ship at
full throttle straight up did not move at all, measured over 15 s, player or
AI. Thrust is now the hull's own (`[physics] thrust` → `Body::GetThrust`,
default 220 for ~1.4 surface T/W) rather than `ShipControlsSystem::
THRUST_FORCE`, `AIStrategySystem` refuses a `Land` order on a body whose
surface gravity the hull cannot out-thrust (with a margin), and
`gravitaris-sim-test`'s `TestTakeoff` pins all of it.

Two guidance-level defects surfaced on the way, both of the same shape — a
layer that had *selected* a behavior while the behavior commanded nothing:

- `EvadeBody` returned "no correction" whenever the ship was still outside
  the safe radius, but the danger check that selects Evade fires off the
  *predicted* path. A pilot heading into a well therefore spent the entire
  window between prediction and arrival coasting under no guidance at all,
  building speed, and hit at 245–436 units/s. The early-out is gone: Evade
  and Depart each own their own hysteresis and decide when to climb, so the
  climb is now unconditional (and Depart flies its hysteresis band under
  power instead of coasting back into it).
- With Evade actually commanding a climb, it then fought the landing it had
  been ordered to make — a descent onto the well reads as danger every tick —
  and hovered a few tens of units short of touchdown forever. The reflex is
  now exempt for the body being landed on, and only that body.
- `LandOnBody` solved its flip-and-burn descent against *local* gravity,
  which grows the whole way down, so from altitude it planned a stopping
  distance it no longer had on arrival. It uses the surface figure now.

Together those turn a scripted descent from 2500 units (dead every time
before, at either thrust value) into a 0.3 units/s touchdown with no hull
damage and a claimed planet.

*A pad is a place, not a planet (2026-08-08), fixing "every AI is stuck on its
own High Port from the first second of the match".* Two independent defects,
either of which alone would have looked like the same symptom:

- **Shopping never ended, so `Rearm` never released.** The trip home was
  gated on `AnyRankUnlocked(faction.unlocked)` — "has this side learned
  anything" — but `IssueFreeUnlocks` grants every rank that costs no Tech to
  every faction on the first tick, so that flag was true for the whole match.
  `AIGoal::Rearm` scores at `REARM_URGENCY`, deliberately above every other
  goal's ceiling, so every pilot was permanently ordered home; and because
  the test short-circuited the `upgradesWanted && padWaitRemaining` branch,
  the `padWaitTicks` bound that exists to stop a wing parking never applied.
  The question is now about this pilot's own money and taste — something
  `UpgradeCatalog::PreferredFit` says is worth buying, affordable past
  `AIPersonality::supplyReserve` — reached through `PilotRef::account`, the
  resolved account handle `ResearchSystem::EnsureAccounts` now caches on the
  hull. `AIPersonality::fit` weights that choice per `UpgradeKind`, so
  `cautious` buys shields, `sniper` buys reach, and `reckless` buys neither.
- **Home was modelled as a planet, and the pad it was standing on wasn't
  one.** `FactionSystem::SpawnPosition` launches every AI *off its own High
  Port* — so a leader's first order was a descent to the surface, straight
  down through the station under its feet. Arriving was tested by comparing
  the ordered site's NetId against `LandingState::landedOnNetId`, which a
  deck landing never matches, so the pilot could neither hold nor give up:
  it wedged against the deck under power, or cycled Land/Depart forever.
  `HomeSite` (`system/gwell/home-site.hpp`) now names a landing site as a
  planet *or* a port over one, `SitePlanetNetId` is the single answer to
  "which planet does this landing count as" that `ResearchSystem` and
  `RepairSystem` each used to keep a copy of, and the AI prefers the port:
  it serves a pilot identically without the descent.

Three things fell out of taking a deck seriously as somewhere to land.
`LandOnBody` now takes a gravity *acceleration* rather than the site's mass
and radius, because what holds a ship on a station's deck is the planet
underneath at the station's orbital radius, not the station. What a descent
flares against comes from the model's own `spawn` hardpoint — already where
`FactionSystem` puts a launching fighter's feet — since a High Port's
bounding radius (70.8) is its longest arm and flaring against that left a
pilot hovering 90 units above the 9-unit-deep deck it was aiming at;
`ObstacleRadius` is the separate, much larger figure for flying *around* one.
And since a port is solid and rides a ring squarely across every descent onto
the planet it orbits, a blocked descent now enters the ring beside the
station — just past the arc it blocks, on the side it is travelling away
from. Deliberately not the far side: guidance arrives in a straight line, and
the straight line to the far side runs through the planet, which is a crater.

`ORBIT_SPEED_FACTOR` dropped from 1/3 to 1/5 in the same pass. Landing on the
deck is now the only way to refit at a port — the "hold station alongside and
it counts" envelope is gone, for players as well as AI — so how matchable
that deck is has to hold up by hand.

*A station is solid from every side (2026-08-09), fixing "AI ships wedge
themselves between the planet and the High Port and stay there".* The station
was an obstacle in exactly one place — `ringEntryPoint`, on a descent onto the
planet beneath it — and that helper declines the two cases that matter: it
skips the ring when the ring **is** the destination, and it skips a ship
already inside the ring. Everything else in the pilot, `EvadeBody` included,
sees only gravity sources. Two permanent wedges came out of that, both
reproduced in the harness before anything was changed:

- **Leaving.** A departure climb is radial and a High Port covers about 9% of
  the ring it rides. A pilot lifting off the surface under one flew into its
  underside and burned outward against it for the rest of the match —
  `LandingState` reads deck contact at matched velocity as landed (a deck
  skips the uprightness test), so it simultaneously believed it was parked and
  was trying to leave. No hull damage, so nothing ever broke the deadlock.
- **Arriving.** `SelectHomeSite` prefers the port, `LandOnBody` closes on the
  station's *centre* in a straight line, and the deck is its outward face nine
  units out against a hull that reaches seventy. A pilot below the ring
  therefore drove into the belly and pressed there: never landed, so never
  `atLab`, so the refit never completed and `Rearm` never released. A 20-minute
  match with every side fielded had a leader stuck like this for 40575
  consecutive ticks — 11 minutes, and still there when the run ended.

Both are now expressed in terms of the station's *column* — the bearings under
it, at any radius below its ring. Nothing may climb through one and no descent
may come out of one, so `Evade`, `Depart` and a deck approach all slide
tangentially out first, at the altitude already held: a straight chord across
the arc cuts back toward the surface, and from low down the surface is the
planet. The nearer edge wins, since a fighter crosses the arc several times
faster than the station sweeps it.

Docking is then a sequence of legs — clear the column, radial to a hold
altitude outside the station's own envelope, arc round to sit over the deck,
descend — and which leg is being flown is *remembered* (`AIPilot::dockStage`)
rather than re-derived. Both intermediate versions of this that tested the
geometry afresh each tick dithered on a seam instead of crossing it: first on
the hold radius, where the climb's target and the test against it were the same
number, then on the quarter-turn bearing, where a pilot orbited the ring for
40 seconds without ever closing on it. The legs only advance; falling back
inside the station's envelope is the one thing that starts them over.

Behind all of it, a progress watchdog: a `Land` or `Depart` that has not closed
its objective by `WEDGE_PROGRESS` in `WEDGE_TICKS` is leaning on something, and
slides tangentially for two seconds before trying again. It is a net, not a
mechanism — over the same 20-minute match it fired 14 times across six sides —
but the failure mode it catches is *permanent*, which is the only reason
either of the bugs above was visible at all.

The same match now tops out at 365 consecutive ticks in the gap (6.1 s, i.e.
traffic passing through). Still open, and pre-existing: a descent onto a *planet*
from the far side of it flies a straight line at the site with the well
-avoidance reflex deliberately exempt, so the line runs through the planet;
and deck contact from underneath still reads as a landing (`LandingStateSystem`
asks a deck for velocity match and nothing else), which the column rule now
prevents rather than corrects.

**Phase 6 — strategy layer.** `AIStrategy` per slice-components (build/attack
decisions) issuing goals to pilots. Separate cadence (every ~1 s, not every
tick). Design when slice-one economy exists.

**Phase 7 (contingent) — background prediction.** Only if profiling shows
predictor cost matters: worker thread, double-buffered trajectory results,
main thread never blocks. The replan-from-actual-state loop already tolerates
stale predictions, so this is a drop-in optimization, not a redesign.

*Empty sides are slots, not bots (2026-08-09).* A dedicated server used to
call `AddAIFaction` for every colour in the roster at startup, so a joining
player always landed alongside a leader they had not asked for and no side was
ever genuinely free. It now fields none, and `Game::FillEmptyTeamsWithAI`
takes only the sides with neither a leader nor a human on them — reachable as
the console's bare `ai` (or `ai fill`) and as `/ai` in chat. "Human" is a
`Callsign` with no `AIPilot` behind it; a leader carries one of those too, so
the name alone cannot tell a pilot from a bot. `AddAIFaction` is now
idempotent per team, which is what lets both entry points be typed twice.
Single-player is unchanged: `SpawnCombatants` still fields a leader on every
side the player is not flying, because there nobody else is coming.

*An AI could not shoot (2026-08-22), fixing "they are too stupid to kill me".*
Three separate reasons a pilot's guns were nearly ornamental, none of them
about aim:

- **The trigger was tapped, not held.** `firePrimary` is a held control: a
  hull's gun mounts are phased across the weapon's own cooldown and only the
  first of them answers a fresh pull (`ShipControlsSystem::AdvancePrimary`,
  `SeedPhasesAt`). A pilot pulsing the flag for one tick every `fireInterval`
  therefore fired one barrel of `fighter-1`'s three. The AI now holds a burst
  long enough for every barrel to get `burstCount` rounds out, and
  `burstShotInterval` is gone — the weapon's own cadence does that spacing.
  Losing the solution mid-burst releases the trigger and costs the same
  silence as finishing it, so a wavering aim cannot be tapped into a rate of
  fire the gun does not have (a re-pull re-seeds the phases, and tapping every
  tick would otherwise fire mount 0 every tick).
- **The nose was never the gun's.** `aimPriorityError` — the velocity
  correction above which the burn takes the heading back from the firing
  solution — was 45 units/s, which routine station-keeping exceeds. Measured
  over a fixed duel, a pilot's guns bore on its target for **15%** of the
  fight. At 180 it is **84%**, and that duel -- everything else held, only
  this threshold moved -- goes from 230 points of damage to 930. It sits just under `boostVelError` on purpose: anything worth
  breaking the aim for is worth burning the capacitor on.
  `STANDOFF_AIM_SHARE` is the second half of that — the old test was
  `range > standoffDistance`, so a pilot holding station sat exactly on the
  boundary and gave the nose up half the time, which is most of a sniper's
  fight.
- **Gunnery was wired to `AIBehavior::Intercept`.** The lead solution was only
  computed while intercepting, so a pilot climbing out of a well, holding a
  patrol ring or breaking off flew past an enemy inside its own firing range
  without loosing a round. Firing is now unconditional; which way the ship is
  *pointed* stays the manoeuvre's business (the nose contest above only opens
  for `Intercept`/`Orbit`/`Idle`), which is what keeps this from turning a
  climb or a descent into a gunfight.

**Tried and dropped:** a range-aware firing aperture (fire when the round is
predicted to pass within a hull's width, rather than within a fixed angle).
It reads better than a flat tolerance and is the right *model*, but measured
against the flat angle it was noise — +19% damage per round at 300 units,
−8% at 500 — which does not pay for a personality knob and a toml key. The
two fixes above are where the difference actually lives.

*A faction is a wing (2026-08-22).* `AddAIFaction` now sizes a wing behind
each leader (`[ai] wing_size` in `data/economy.toml`, default 2). Wingmen
carry no `AIStrategy`, so they fly the leader's objective through the wing
orders that already existed, and each holds its own respawn slot funded by
`FactionSystem::TryRespawn` — a side that has lost its production flies fewer
of them rather than the same three for free. Their presets are picked from
the library rather than inherited, so a wing is a mix of temperaments.

Every AI ship is named in the same pass (queued-work item 3): `SpawnAIShip`
gives one `<colour>-<n>` off a per-side ordinal that only ever increments, and
`SpawnAILeader` gives its one `<colour>-leader` without spending an ordinal, so
`red-1` is always the first wingman. A wing of ships nobody can name is, from
the console, several things that cannot be listed or flown to. `TestAIWing`
covers both halves.

## Risks

- **Guidance in strong wells is the hard part** (Lunar Lander with a
  turn-rate limit) — expect tuning iteration in phases 2–3; that's why they
  come before any tactics/strategy. Prediction (the seemingly scary 3-body
  part) is comparatively cheap.
- Predictor/sim drift: bounded by frequent replanning; keep the headless
  drift test from phase 1 as a regression guard.
- No softening in `ApplyGravity` — near-zero distances blow up forces in sim
  *and* predictor; consider adding an epsilon in both when it first bites.
- Tuning constants (PD gains, deadbands, replan interval) should live in one
  struct, tweakable via the F1 overlay, or iteration will be miserable.

## Parked decisions (from the phase-4 hand-off, folded back 2026-07-14)

- **Per-entity vs per-controller/global input queue**: per-entity matches the
  single-player shape; quake-likes hang the cmd ring off the
  client/connection object, lockstep games use a global tick→commands log.
  Revisit when netcode starts (per-connection is the likely landing spot).
  Note: AI pilots are pure functions of sim state, so replays never need to
  record AI commands — only human input.
- **`Land` guidance** (phase 3 remainder): built as `LandOnBody` in
  gravity-well-mode-plan.md Phase 5. It went the analytic route rather than
  the predictor one — the descent speed is solved directly from altitude and
  the thrust left over after gravity, so no time-to-impact prediction is
  involved — plus the gentle terminal phase anticipated here, which turned
  out to be load-bearing for uprightness, not just for gentleness.
- **Predictor-based drift compensation in `GotoPoint`**: reactive control has
  been sufficient; revisit if AI ships visibly miss in stronger wells.
- **Verification practice**: no test target exists yet (needs a game-lib
  CMake split); phases 0–4 were verified with temporary instrumented runs
  (`LOG(info)` + stdout redirect via `Start-Process`), since window
  automation doesn't reliably reach the app (Windows foreground lock).
