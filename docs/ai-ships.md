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
| Strategy | Build/attack decisions | goals for tactics | `AIStrategy` (slice-one, deferred) |

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
toward target radius), `InterceptEntity` (dead-reckoned lead pursuit +
target velocity; untested until AI ships exist), `EvadeBody` (radial climb
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

**Phase 6 — strategy layer.** `AIStrategy` per slice-components (build/attack
decisions) issuing goals to pilots. Separate cadence (every ~1 s, not every
tick). Design when slice-one economy exists.

**Phase 7 (contingent) — background prediction.** Only if profiling shows
predictor cost matters: worker thread, double-buffered trajectory results,
main thread never blocks. The replan-from-actual-state loop already tolerates
stale predictions, so this is a drop-in optimization, not a redesign.

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
- **`Land` guidance** (phase 3 remainder): suicide-burn retro-thrust descent,
  the hardest behavior; predictor gives time-to-impact, controller holds
  retrograde, burn starts when stopping distance ≈ altitude, gentle terminal
  phase below some altitude.
- **Predictor-based drift compensation in `GotoPoint`**: reactive control has
  been sufficient; revisit if AI ships visibly miss in stronger wells.
- **Verification practice**: no test target exists yet (needs a game-lib
  CMake split); phases 0–4 were verified with temporary instrumented runs
  (`LOG(info)` + stdout redirect via `Start-Process`), since window
  automation doesn't reliably reach the app (Windows foreground lock).
