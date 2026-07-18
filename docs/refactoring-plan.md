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

- [x] **R0.1 Fix FindHeaviestGravitySource.** Query `GravitySource`
  (mass × multiplier) like AIPilotSystem does, instead of `cpBodyGetMass`.
  Kills the NaN risk in autopilot Orbit and removes cgame's last direct
  Chipmunk-mass read for celestials. Manual check: engage Orbit autopilot
  near a sun; ship should circle, not vanish.
- [x] **R0.2 Silence Magnum C4910.** The warnings come from Magnum's own
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

- [x] **R1.1** New `include/gravitaris/cgame/fx/hit-flash-system.hpp` +
  `src/cgame/fx/hit-flash-system.cpp`: class `HitFlashSystem` holding the
  event cursor, constructed with `(flecs::world&, const GameEventQueue&,
  EntitySpawner&)` (spawner only for `EntityForNetId`). One method
  `Update(float dtSeconds)` containing exactly today's
  `CGame::UpdateHitFlashes` body. Mirrors AudioSystem's shape: a cgame
  consumer of the event stream with its own cursor (rule 3).
- [x] **R1.2** CGame: drop `UpdateHitFlashes`/`m_flashEventCursor`, add a
  `HitFlashSystem m_hitFlashSystem;` member (constructed after
  m_entitySpawner exists — constructor body or careful init order, same
  concern as the EntitySpawner::Init() precedent), call
  `m_hitFlashSystem.Update(dt)` from `Render()`.

## Phase R2 — Sim mutations out of the render/client path

- [x] **R2.1 Ship-weight multiplier into Game.** Move
  `m_shipWeightMultiplier` + its application into `Game`: a
  `m_shipWeightMultiplier` field applied at the top of `Game::Update()` on
  the player's PhysicsRef (same reapply-every-tick logic, now on the tick
  path instead of the render path — rule 2). CGame keeps only the
  getter/setter forwarding for the debug panel. Note in the field comment
  that it is a debug knob, replicated nowhere.
- [x] **R2.2 SpawnRandomAIShip into Game.** Move the method +
  `m_randomAIShipSpawnCount` to `Game` unchanged (it already seeds from
  `GetStep()`). Debug UI/keybind call it via the Game reference they already
  have. Under future netcode this becomes a server command handler; having
  it in game/ makes that a move-free change.

## Phase R3 — CameraDirector

The biggest extraction: all camera state and logic into
`include/gravitaris/cgame/camera-director.hpp` + `src/cgame/camera-director.cpp`.

- [x] **R3.1** Class `CameraDirector` owning: `Camera`, `CameraParams` (moved
  out of CGame wholesale), zoom state, framing state, the dead-zone
  constants, `SelectFramedEnemy`, `PlanetFramingGoal`, and `UpdateCamera`
  renamed to `Update(player, viewportSize, dtSeconds)` — plain parameters
  instead of a `CameraFrame` wrapper struct (3 params didn't warrant one).
  `NudgeManualZoom`, the follow-toggle, and the getters moved with it.
- [x] **R3.2** CGame shrinks to a `CameraDirector m_cameraDirector;` member,
  `GetCameraDirector()`, and thin forwarding wrappers (`GetCamera`,
  `GetCameraParams`, `NudgeManualZoom`, etc.) so external callers (app,
  WorldToUi, debug panels) needed no changes except `camera-panel.cpp`'s one
  now-relocated type name. `Render()` calls `m_cameraDirector.Update(...)`
  then reads pos/zoom off it for the renderers.
- [x] **R3.3 — deviated from the plan.** Kept `m_lastCameraTime`/
  `m_cameraTimeValid` in `CGame::Render()` rather than moving into
  CameraDirector: the same dt also feeds `HitFlashSystem::Update`, so
  computing it once in the caller and passing it to both consumers is
  simpler than duplicating the wall-clock bookkeeping.

## Phase R4 — IndicatorRenderer & Autopilot

- [x] **R4.1 IndicatorRenderer.** `UpdateIndicators` + `IndicatorParams` +
  `m_arrowModel` → `include/gravitaris/cgame/hud/indicator-renderer.hpp` +
  `src/cgame/hud/indicator-renderer.cpp`. Constructed with
  `(flecs::world&, ResourceLoader&, ModelRenderer2&)` (loads/holds the arrow
  model itself); `Update(player, cameraPos, zoom, viewportSize, pixelScale)`
  submits the overlays, doing its own Transform/Team try_get like
  CameraDirector does.
- [x] **R4.2 Autopilot.** `AutopilotMode` (now a free enum again, not nested
  in CGame — every `AutopilotMode::X` call site stayed unchanged),
  `SetAutopilotMode`/`ComputeAutopilotControls`/`FindHeaviestGravitySource`,
  anchor/goto/orbit state, `FlightControllerParams`/`GuidanceParams` →
  `include/gravitaris/cgame/autopilot.hpp` + `src/cgame/autopilot.cpp`,
  class `Autopilot`. CGame keeps `GetAutopilot()` + thin forwarders for
  every existing accessor, so the app's FeedInput and flight-panel.cpp
  needed no call-site changes.

## Phase R5 — game/ and client/ cleanups

- [x] **R5.1 Scenario builder.** Extracted the hardcoded solar system from
  `Game::Start()` into `src/game/scenario/classic-scenario.cpp` with
  `void BuildClassicScenario(EntitySpawner&)`. `Start()` is now just: spawn
  player, `BuildClassicScenario(*m_entitySpawner)`. Spawn order (and NetId
  assignment order) unchanged, so the sim-test checksum matched exactly.
- [x] **R5.2 ReplayController.** Moved record/replay state + logic into
  `src/client/replay-controller.hpp/.cpp` (client-local, not `include/`).
  `GravitarisApplication` kept identical method names
  (`ToggleRecording`/`StartReplay`/`StopReplay`) as thin wrappers, so the
  F5/F6/F7 key handlers needed no changes.
- [x] **R5.3 SpawnStar/SpawnPlanet dedupe.** `SpawnPlanet` turned out to be
  entirely unused (every planet spawn goes through `SpawnOrbitingPlanet`
  now) — deleted outright. `SpawnStar` stays as a semantic alias over
  `SpawnCelestial` since it's still the one call site classic-scenario.cpp
  uses.

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

**Verification status**: R0-R5 done. After every phase: both `GravitarisNG`
and `gravitaris-sim-test` built clean, `gravitaris-sim-test` matched the
pre-refactor checksum exactly (`0x1a3096e5f4b36217` state,
`0x2e16d87965684ca9` events, unchanged across all six phases as expected —
none of them touch sim behavior), and `GravitarisNG.exe` launched and ran a
few seconds with no errors/asserts beyond the expected headless-environment
sim-step-cap warning. Done from an unattended/autonomous session — **not yet
manually play-tested interactively** (camera framing/zoom feel, autopilot
modes, F5/F6/F7 record-replay, minimap, HUD arrows). Worth a hands-on pass
before considering R6.
