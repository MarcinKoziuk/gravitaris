# Refactoring plan — CGame decomposition & module-boundary cleanup

Executable plan, same style as `docs/networking-plan.md`: each phase is
independently buildable and committable; no phase changes sim behavior unless
explicitly marked. After every phase: build `GravitarisNG` **and**
`gravitaris-sim-test`, run the sim-test (state + event checksums must match),
smoke-launch the app.

## The boundary rules being enforced

From CLAUDE.md / ADR 0001, restated as the invariants each item below serves:

1. `game/` is the authoritative, headless sim. `cgame/` is presentation +
   client-side input production. `src/client/` is the platform shell (window,
   GL context, OS input, main loop).
2. **The render path never mutates sim state.** Sim mutations flow through
   the InputQueue (player/autopilot/replay commands) or explicit sim-side
   calls made from the tick path — never from `Render()`.
3. One-shot sim→client information rides the GameEventQueue, each consumer
   with its own cursor (AudioSystem does this correctly; hit-flash currently
   does it *inside* CGame instead of its own system).
4. Anything that must be reproducible under replay (spawns, RNG) lives in
   `game/`, seeded from the tick.

## Current pain points (inventory)

- `CGame` is a god object: camera director, hit-flash consumer, HUD arrow
  overlay, autopilot, debug spawn command, debug physics knobs, renderer
  plumbing — 615-line .cpp / 394-line .hpp and growing with every feature.
- `CGame::Render()` mutates sim state (`SetMassMultiplier` every frame) —
  violates rule 2.
- `CGame::SpawnRandomAIShip()` mutates the sim from cgame — under a real
  client/server split this is a server command; belongs in `Game` (rule 4 is
  already honored via SplitMix, but the *location* is wrong).
- `Game::Start()` hardcodes the classic-mode solar system inline.
- `GravitarisApplication` mixes the main loop with input record/replay state.
- **Bug**: `CGame::FindHeaviestGravitySource()` reads `cpBodyGetMass`, which
  is +inf for the kinematic celestials since the solar-system change —
  autopilot Orbit mode gets `orbitMass = inf` → NaN guidance velocities.
  (AIPilotSystem/TrajectoryPredictor were migrated to `GravitySource`;
  this one was missed.)
- MSVC build emits ~30 C4910 warnings from Magnum headers (below).

## Phase R0 — Bug fix + warning silencing (do first, it's small)

- [ ] **R0.1 Fix FindHeaviestGravitySource.** Query `GravitySource`
  (mass × multiplier) like AIPilotSystem does, instead of `cpBodyGetMass`.
  Kills the NaN risk in autopilot Orbit and removes cgame's last direct
  Chipmunk-mass read for celestials. Manual check: engage Orbit autopilot
  near a sun; ship should circle, not vanish.
- [ ] **R0.2 Silence Magnum C4910.** The warnings come from Magnum's own
  `Math/ConfigurationValue.h` (`extern template` + `__declspec(dllexport)`
  explicit instantiations — a known, benign Magnum-on-MSVC-shared artifact;
  our code doesn't trigger it). Preferred: after
  `FetchContent_MakeAvailable(Magnum)`, for each Magnum target that exists
  (`Magnum`, `MagnumGL`, `MagnumShaders`, `MagnumAudio`,
  `MagnumSdl2Application`), `target_compile_options(<tgt> PRIVATE /wd4910)`
  under `if(MSVC)`. Do **not** switch to static Magnum/Corrade in this pass
  (it would also fix it, and the startup "corradeUtilityUniqueWindowsResource
  Globals" warning too, but it's a riskier link-model change — tracked as
  R6.1 instead).

## Phase R1 — HitFlashSystem (the UpdateHitFlashes extraction)

- [ ] **R1.1** New `include/gravitaris/cgame/fx/hit-flash-system.hpp` +
  `src/cgame/fx/hit-flash-system.cpp`: class `HitFlashSystem` holding the
  event cursor, constructed with `(flecs::world&, const GameEventQueue&,
  EntitySpawner&)` (spawner only for `EntityForNetId`). One method
  `Update(float dtSeconds)` containing exactly today's
  `CGame::UpdateHitFlashes` body. Mirrors AudioSystem's shape: a cgame
  consumer of the event stream with its own cursor (rule 3).
- [ ] **R1.2** CGame: drop `UpdateHitFlashes`/`m_flashEventCursor`, add a
  `HitFlashSystem m_hitFlashSystem;` member (constructed after
  m_entitySpawner exists — constructor body or careful init order, same
  concern as the EntitySpawner::Init() precedent), call
  `m_hitFlashSystem.Update(dt)` from `Render()`.

## Phase R2 — Sim mutations out of the render/client path

- [ ] **R2.1 Ship-weight multiplier into Game.** Move
  `m_shipWeightMultiplier` + its application into `Game`: a
  `m_shipWeightMultiplier` field applied at the top of `Game::Update()` on
  the player's PhysicsRef (same reapply-every-tick logic, now on the tick
  path instead of the render path — rule 2). CGame keeps only the
  getter/setter forwarding for the debug panel. Note in the field comment
  that it is a debug knob, replicated nowhere.
- [ ] **R2.2 SpawnRandomAIShip into Game.** Move the method +
  `m_randomAIShipSpawnCount` to `Game` unchanged (it already seeds from
  `GetStep()`). Debug UI/keybind call it via the Game reference they already
  have. Under future netcode this becomes a server command handler; having
  it in game/ makes that a move-free change.

## Phase R3 — CameraDirector

The biggest extraction: all camera state and logic into
`include/gravitaris/cgame/camera-director.hpp` + `src/cgame/camera-director.cpp`.

- [ ] **R3.1** Class `CameraDirector` owning: `Camera`, `CameraParams` (moves
  out of CGame wholesale), zoom state (`m_cameraZoom`, manual-zoom trio),
  framing state (`m_framingAmount`, `m_framedEnemy`, `m_framedEnemyOffset`,
  `m_framedReach`, planet/close amounts), the dead-zone constants,
  `SelectFramedEnemy`, `PlanetFramingGoal`, and `UpdateCamera` renamed to
  `Update(const CameraFrame&)` — where `CameraFrame` is a small input struct
  `{playerPos, playerVel, playerTeam, viewportSize, pixelScale, dtSeconds}`
  plus the registry/physics refs it queries. `NudgeManualZoom`, the
  follow-toggle, and the getters move with it.
- [ ] **R3.2** CGame shrinks to: `CameraDirector m_cameraDirector;`,
  `GetCameraDirector()` for the debug panel + app, and `Render()` calls
  `m_cameraDirector.Update(frame)` then reads pos/zoom off it for the
  renderers. `camera-panel.cpp` takes `CameraDirector&` instead of poking
  CGame accessors.
- [ ] **R3.3** The wall-clock dt bookkeeping (`m_lastCameraTime`,
  `m_cameraTimeValid`) moves into CameraDirector too (it exists only to feed
  camera smoothing; HitFlashSystem gets dt passed in from the same place it
  does today).

## Phase R4 — IndicatorRenderer & Autopilot

- [ ] **R4.1 IndicatorRenderer.** `UpdateIndicators` + `IndicatorParams` +
  `m_arrowModel` → `include/gravitaris/cgame/hud/indicator-renderer.hpp` +
  `src/cgame/hud/indicator-renderer.cpp`. Constructed with
  `(flecs::world&, ResourceLoader&, ModelRenderer2&)` (loads/holds the arrow
  model itself); `Update(playerPos, playerTeam, cameraPos, zoom,
  viewportSize, pixelScale)` submits the overlays. hud-panel takes it by
  reference.
- [ ] **R4.2 Autopilot.** `AutopilotMode`, `SetAutopilotMode`,
  `ComputeAutopilotControls`, `FindHeaviestGravitySource` (post-R0.1),
  anchor/goto/orbit state, `m_flightParams`, `m_guidanceParams` →
  `include/gravitaris/cgame/autopilot.hpp` + `src/cgame/autopilot.cpp`,
  class `Autopilot`. It is a client-side *command producer* (same seam as
  the keyboard — its output goes into FeedInput's InputCommand), so cgame is
  the right module; it just shouldn't be CGame itself. CGame keeps a member
  + accessor; the app's FeedInput and the autopilot debug panel go through
  it.

## Phase R5 — game/ and client/ cleanups

- [ ] **R5.1 Scenario builder.** Extract the hardcoded solar system from
  `Game::Start()` into `src/game/scenario/classic-scenario.cpp` with
  `void BuildClassicScenario(EntitySpawner&)` (+ tiny header). `Start()`
  becomes: spawn player, `BuildClassicScenario(*m_entitySpawner)`. Prepares
  for data-driven maps without inventing a format now.
- [ ] **R5.2 ReplayController.** Move record/replay state + logic
  (`m_recordLog`, `m_replayLog`, cursors, `ToggleRecording`, `StartReplay`,
  `StopReplay`, the replay branch of `FeedInput`) from
  `GravitarisApplication` into `src/client/replay-controller.hpp/.cpp`
  (client-local, not `include/` — nothing else needs it). The app keeps the
  keybinds and calls `m_replay.NextCommand(...)` / `m_replay.Record(...)`.
- [ ] **R5.3 SpawnStar/SpawnPlanet dedupe.** Both just call SpawnCelestial;
  either delete them in favor of public `SpawnCelestial` or keep as inline
  semantic aliases in the header. Cosmetic; do last.

## Phase R6 — Deferred / needs a decision

- [ ] **R6.1 Static Corrade+Magnum.** Would eliminate both the C4910 pile
  (R0.2 workaround becomes removable) and the startup Corrade
  "unique-global-symbol / app may misbehave" warning, and simplify
  deployment (no DLLs). Risk: link-order/static-init gotchas (the audio
  static-link issues in memory were of this family). Do it in isolation,
  nothing else in the same commit.
- [ ] **R6.2 RenderContext struct.** `SetViewportSize`/`SetPixelScale`/zoom/
  camera-pos are forwarded to each renderer individually; a shared
  `RenderContext` (viewport, pixelScale, zoom, cameraPos, lineWidth,
  zoomWidthFactor) passed to `Render()` would collapse ~6 forwarding
  setters. Only worth it when touching the renderers anyway.
- [ ] **R6.3 Debug-panel param ownership.** After R3/R4 the panels take the
  subsystem references directly; consider a `DebugPanels` registry so
  debug-ui.cpp doesn't need CGame at all. Low priority.

## Ordering & risk notes

- R0 first (bug + warning noise), then R1/R2 (small, unblock the boundary
  rules), then R3 (largest), R4, R5. Each phase = one commit.
- R2 moves *where* sim mutations run (render → tick path). The ship-weight
  multiplier applying on the tick instead of the frame is a real (desired)
  behavior change: it now applies even on frames that don't render and is
  identical across replay runs. Sim-test unaffected (it never set the knob).
- Everything else is mechanical relocation; determinism checksums must not
  change in R1, R3, R4, R5.2, R5.3. R5.1 must not change spawn order (NetId
  assignment order is part of the checksum).
