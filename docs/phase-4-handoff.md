# Hand-off: implementing AI ships phase 4 (first autonomous ship)

For a fresh session picking up where the 2026-07-13 sessions left off. Read
`docs/ai-ships.md` first (the plan; phases 0–3 are implemented and their
status blocks list exact files), plus `docs/adr/0001-netcode-model.md`
(constraints) and `CLAUDE.md` (style — note the comment removal test and
static-helpers-below convention).

## What exists (phases 0–3, all committed on main)

| Piece | Where | Note |
|---|---|---|
| Command queue | `component/input-queue.hpp`, `system/input-system.cpp` | tick-stamped `InputCommand` → `Controls`, drained by `InputSystem` each tick |
| Record/replay | `game/input/input-log.{hpp,cpp}` | F5 record / F6 replay / F7 stop, same-machine only |
| Navigation | `game/gnc/nav/trajectory-predictor.{hpp,cpp}` | test-particle symplectic Euler, owned by `Game` (`GetTrajectoryPredictor()`) |
| Control | `game/gnc/control/flight-controller.{hpp,cpp}` | `FlyToVelocity(Transform, desiredVel, params) -> ControlFlags`, pure |
| Guidance | `game/gnc/guidance/behaviors.{hpp,cpp}` | `GotoPoint`, `OrbitBody`, `InterceptEntity` (untested), `EvadeBody`; `Land` NOT implemented |
| Player harness | `CGame` autopilot (K/P/G/O), Flight + Trajectory tabs in F1 overlay | tuned defaults verified: kill-velocity, hover, goto-arrive, stable orbit |

Verified numbers to sanity-check against after changes: kill-velocity
arrests a 66 u/s fall in ~1 s; orbit holds r=306 ±2 for 15 s at ~40 u/s
(= √(G·M/r), G=20, planet mass 25000).

## Phase 4 goal

One autonomous enemy fighter that spawns, flies competently (doesn't crash
into the planet), pursues the player, and fires when aligned. Everything it
needs already exists as pure functions — this phase is assembly plus a small
utility selector.

## Design (agreed in prior sessions)

- **`AIPilot` component** (`include/gravitaris/game/component/ai-pilot.hpp`):
  server-only, never serialized (ADR 0001 §2). Contents: active behavior
  enum (Idle / Evade / Intercept / Orbit), target `flecs::entity` (switch to
  NetId when netcode lands), fire-cooldown counter, replan/decision-interval
  counter. Per-ship `FlightControllerParams` + `GuidanceParams` copies (so
  difficulty knobs can vary per ship later).
- **`AIPilotSystem`** (`src/game/system/ai-pilot-system.{hpp,cpp}` in the
  ECS-system convention, or `game/gnc/` if it feels more like tactics — your
  call, document it). Runs in `Game::Update()` **before**
  `InputSystem::Update()` so its command for tick N is consumed the same
  tick. New tick order: physics simulate → physics write-back → **AIPilot**
  → InputSystem → ShipControls → bullet lifetime.
- Each decision interval (~10–30 ticks, not every tick) the utility selector
  picks a behavior; every tick it runs the active behavior + `FlyToVelocity`
  and pushes an `InputCommand{step, flags}` onto its own `InputQueue` —
  identical seam as the human player, per ADR 0001 ("AI counts as a client").
- **Utility selector v1** (plain if/else is fine, it's 3 rules):
  1. Predicted to hit / get too close to the planet (use
     `TrajectoryPredictor` over ~2 s, or a cheap radius+falling check) →
     `EvadeBody(planet, safeRadius)`.
  2. Player exists and within engage range → `InterceptEntity(player)`, fire
     when aligned (below).
  3. Else → `OrbitBody` around the heaviest body at some patrol radius.
- **Firing**: bullets are gravity-immune, muzzle speed 200 + shooter
  velocity inherited (`GetBulletSpawnPosAndVel` in
  `ship-controls-system.cpp`). Lead solution: solve intercept of a 200 u/s
  bullet (relative to own vel) against target pos/vel; fire when heading
  error to the lead direction < ~5–10°. **Cooldown lives in AIPilot** — there
  is no weapon-cooldown system yet, and `firePrimary=true` every tick fires
  a bullet every tick (the one-shot clearing in `ShipControlsSystem` only
  helps humans; `InputSystem` overwrites `Controls` each tick from the
  queue).
- **Spawning**: `EntitySpawner::SpawnAIShip(modelId, pos)` mirroring
  `SpawnPlayer` (Transform + RigidBodyDesc + Controls + InputQueue) plus
  `AIPilot`. Add a "Spawn AI fighter" button to the debug Spawn panel
  (`src/cgame/ui/debug/spawn-panel.cpp`) and/or a debug key.
- **No damage yet**: there is no Health component — bullets physically
  collide but nothing dies. That's fine for phase 4; damage belongs to the
  slice work (`slice-components.md`).

## Verification recipe (pattern used by phases 0–3)

No test target exists yet, so verify with a temporary instrumented run:
auto-spawn an AI ship at tick ~60 via temp code in
`GravitarisApplication::FeedInput()` or `Game::Start()`, log per second:
distance AI→player, AI speed, AI distance to planet center. Assert by
eyeball from the log: distance to player shrinks and stabilizes (intercept
works), planet distance never goes below the planet radius (~30–40 world
units for the simple planet at scale 0.2 — check the SVG), no NaNs. Then
REMOVE the temp code (grep for `TEMP`), rebuild, quick 5 s launch check,
update the phase 4 status block in `docs/ai-ships.md`, commit.

Build (CLion dir is the fast iteration one; `out/` also configured):

```powershell
$cmake = "C:\Program Files\JetBrains\CLion 2025.3.3\bin\cmake\win\x64\bin\cmake.exe"
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && `"$cmake`" --build C:\Users\marcin\Projects\GravitarisNG\cmake-build-debug-visual-studio --target GravitarisNG"
```

New files must be added to the explicit source list in `CMakeLists.txt`.

## Gotchas learned the hard way

- Keyboard/window automation doesn't reach the app reliably (Windows
  foreground lock); instrumented runs with `LOG(info)` + stdout redirect are
  the reliable verification path. `Start-Process -RedirectStandardOutput`
  works (the exe is a console-subsystem build).
- Sim code (`src/game/`) must not include cgame/GL headers (ADR 0001 §1) —
  `AIPilotSystem` needs exactly nothing from cgame; if it seems to, the
  design is wrong.
- `Controls` bits are overwritten each tick by `InputSystem` for entities
  with an `InputQueue`; if no command matches the tick, `Controls` keeps its
  last value (repeat-last-command). AI should push a command every tick.
- flecs observers close over `this` (see `PhysicsSystem` dtor comment) —
  destruct order matters if `AIPilotSystem` uses observers (it shouldn't
  need any).

## Open questions parked (do NOT resolve silently — discuss first)

1. ~~`std::deque` in `InputQueue`~~ **RESOLVED 2026-07-14**: swapped to a
   fixed-capacity (64 = quake `CMD_BACKUP`) ring buffer in
   `component/input-queue.hpp` (`std::array` + head/count, `Push`/`Front`/
   `PopFront`); `static_assert(is_trivially_copyable_v<InputQueue>)` guards
   the flecs memcpy-relocation fast path. Verified: kill-velocity autopilot
   run reproduced the phase-2 numbers exactly across ~6 ring wraparounds.
2. **Per-entity vs per-controller/global input queue**: per-entity matches
   the current single-player shape; quake-likes hang the cmd ring off the
   *client/connection* object, not the entity; lockstep games use a global
   tick→commands log. Revisit when netcode starts (per-connection is the
   likely landing spot). Deferred.
3. `Land` guidance behavior (phase 3 remainder) — suicide-burn descent;
   design sketch in the plan doc. Not blocking phase 4.
4. Predictor-based drift compensation in `GotoPoint` — reactive control has
   been sufficient; revisit if AI ships visibly miss in stronger wells.

Delete this file (or fold anything still relevant into `docs/ai-ships.md`)
once phase 4 lands.
