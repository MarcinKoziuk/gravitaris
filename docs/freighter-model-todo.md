# Freighter model (freighter-0) integration — technical todo

Status: model art updated by Marcin 2026-07-21 (hull + two cargo pods +
`_thrust` layer, nose-up). Item 1 done same day; the rest are open, ordered
so each builds on the previous. Written to be executable without extra
context — each item names the exact files, conventions, and pitfalls.

Engine conventions that matter here (verified against the code, don't
rediscover them):

- SVG layer labels are render-group tags. `src/cgame/resource/shape.cpp`
  groups paths by the *layer's* `inkscape:label` (nanosvg's `groupLabel`);
  labels starting with `@` are skipped by the renderer entirely (engine-side
  consumers only: `@body` → collision polygons in
  `src/game/resource/body.cpp`, `@origin` → transform origin). Per-*path*
  `inkscape:label`s (like the current `@cargo_l`/`@cargo_r` paths inside the
  "model" layer) are **ignored** — they render as plain "model" content
  today.
- `_thrust` is an existing tagged render group:
  `ModelRenderer2::Render` (`src/cgame/renderer/model-renderer2.cpp`) draws
  it only for entities whose `Controls::actionFlags.thrustForward` is set.
  `Controls` flags replicate (`EntityState::controlsFlags` in
  `include/gravitaris/game/net/snapshot.hpp`), and
  `src/cgame/net/snapshot-applier.cpp` emplaces `Controls` on every
  `NetEntityType::Ship` mirror entity — freighters classify as Ship
  (`src/game/net/snapshot.cpp`'s type ladder), so a server-side freighter
  with `thrustForward` set gets thrust visuals on every client for free.
  The thrust audio loop (`src/cgame/audio/audio-system.cpp`) keys off the
  same flag.
- Ship facing convention: thrust force is applied at local `(0,
  -THRUST_FORCE)` (`ShipControlsSystem::ApplyMovement`), i.e. the nose is
  local **-Y**; world nose direction at rotation `rot` is
  `(sin rot, -cos rot)`. So "face the direction of travel" for velocity `v`
  means `rot = atan2(v.x(), -v.y())`.
- Chipmunk **kinematic** bodies (the freighter is one, `freighter-0.toml`'s
  `physics.kinematic = true`) ignore applied forces/torques, so freighters
  passing through `ShipControlsSystem::ApplyMovement` is harmless —
  `FreighterSystem::SetKinematicMotion` stays the sole motion authority.

## 1. [x] Thrust visual while in transit (done 2026-07-21, tuned same day)

`EntitySpawner::SpawnFreighter` now emplaces `Controls`;
`FreighterSystem::Update`'s transit loop sets `controls.actionFlags.
thrustForward` and clears it on arrival. **Tuned** after playtesting feedback
("we're in space, only thrust when accelerating" -- it was lit for the
entire transit, snapping straight to `TRANSIT_SPEED` with no ramp at all):
added `FreighterSystem::TRANSIT_ACCELERATION` (20 units/s^2) so speed ramps
up from a standing start instead of snapping; `thrustForward` is now `speed
< TRANSIT_SPEED` (lit only while still accelerating, off once cruising --
coasting in vacuum needs no visible engine). No renderer change needed (see
conventions above).

## 2. [x] Freighter faces its direction of travel (transit) (done 2026-07-21)

`PhysicsSystem::SetKinematicMotion` grew an `std::optional<double> angle`
param (default `std::nullopt`, so every other caller — `OrbitSystem`,
`ClientPrediction`, `StructureAttachmentSystem`'s surface branch — is
unaffected); it calls `cpBodySetAngle` when set.
`FreighterSystem::Update`'s transit branch now passes
`atan2(vel.x(), -vel.y())` each tick. No slew-rate limiting added — untested
by eye yet, revisit if the initial snap or orbital-drift retargeting looks
twitchy in a real playthrough.

## 3. [x] Orientation while orbiting (arrived) (done 2026-07-21)

`StructureAttachmentSystem`'s orbit-attached branch did not write
`Transform::rot` at all — every orbiting entity (High Port, Space Dock,
Sensor Array, an arrived Freighter) kept whatever fixed orientation it
spawned with, forever. Deliberately did **not** change that for the three
station types (they may be authored assuming a fixed look; changing their
visual behavior wasn't asked for and wasn't verified against the models).
Only a `Freighter`-tagged entity now gets prograde facing
(`entity.has<Freighter>()` gate, same `atan2` convention) via the same new
`angle` param on `SetKinematicMotion`.

## 4. [ ] Cargo pods as toggleable render groups

Goal: when a cargo pod is unloaded (`Freighter::cargoRemaining` drops 2→1→0
in `FreighterSystem`), its half of the model disappears on every client.

a. [x] **SVG restructured** (done 2026-07-21). Resolved the "which path is
   which" question by bounding-box math on the path data: `path5` (was
   labeled `@cargo_r`) actually spans the *entire* hull nose-to-tail
   (x:18.9–34.2, y:8.8–63.1) — a stale/incorrect label, not a cargo pod at
   all; relabeled it `hull` and left it in the `model` layer. The real
   pods are `path6` (`@cargo_l`, x:18.3–25, a left-side strip) and the
   previously-unlabeled `path6-9` (x:27.3–34.1, its mirror on the right —
   confirmed as the right pod, not a stray duplicate). Moved each into its
   own new top-level layer, `_cargo_l` and `_cargo_r` respectively
   (underscore prefix, matching `_thrust`'s existing convention). Verified:
   XML well-formed, engine's own nanosvg/Body parser loads it without error
   (native smoke test + `gravitaris-sim-test`, which spawns real freighters
   in `TestFreighterEconomy`), and the sim-test state checksum is
   byte-identical to before the edit -- expected proof this was purely
   cosmetic, since `@body` (the collision layer) was untouched. Native+wasm
   both build clean.
   **Regression + fix (same day)**: this alone made both pods permanently
   invisible (worse than before -- previously they rendered as ordinary
   "model" content) -- moving them into their own tagged groups took them
   out of `ModelRenderer2`/`SimpleModelRenderer`'s only always-drawn call
   (`OVERLAY_TAG`/`"model"_id`), and nothing yet draws `_cargo_l`/`_cargo_r`
   at all. Fixed by adding unconditional `RenderTag("_cargo_l"_id, {})` /
   `RenderTag("_cargo_r"_id, {})` calls to `ModelRenderer2::Render` and the
   matching `RenderGroup` calls to `SimpleModelRenderer::Render` -- both
   pods always drawn again, restoring parity with pre-restructure
   visibility. This is intentionally a stopgap: real toggling still needs
   (b)+(c) below; until then both pods show regardless of
   `cargoRemaining`.
b. **Replicate cargo count**: add a small field to `EntityState`
   (`include/gravitaris/game/net/snapshot.hpp`) — e.g. reuse a spare byte or
   add `std::uint8_t cargoRemaining` — populate it in `GatherSnapshot` from
   `Freighter::cargoRemaining` (0 for non-freighters), serialize/deserialize
   it in `SerializeSnapshot`/`ReadSnapshot` (**bump `SNAPSHOT_VERSION`**),
   and store it client-side in the mirror world (simplest: emplace the real
   `Freighter` component on Ship-type mirror entities when the field is
   nonzero at spawn, and update its `cargoRemaining` each apply — see how
   `snapshot-applier.cpp` updates `Controls` flags in place).
c. **Render filter**: in `ModelRenderer2::Render`
   (`src/cgame/renderer/model-renderer2.cpp`), add two `RenderTag` calls
   modeled on the `_thrust` one:
   `_cargo_l` drawn when `cargoRemaining >= 2`, `_cargo_r` when
   `cargoRemaining >= 1` (first unload hides one pod, second hides the
   other; swap l/r if Marcin prefers the other order). Entities without the
   component (fighters, everything else) must still draw any `_cargo_*`
   groups they might have — return true when `try_get<Freighter>()` is
   null. Also add the same two calls to
   `src/cgame/renderer/simple-model-renderer.cpp` (it has its own `_thrust`
   call — keep the two renderers consistent).
d. **Single-player**: works automatically (same registry, real `Freighter`
   component present).

## 5. [ ] Separate collision bodies per cargo pod (marked LATER by Marcin)

Deliberately deferred — do not start without asking. The idea: pods become
child objects with their own `@body` polygons (own `Damageable`?) so
shooting a pod off is possible. Open questions to settle first: separate
entities vs. multi-shape body, what losing a pod means for the economy
(cargo destroyed?), and how attachment/detachment replicates.

## 6. [ ] Sanity checks after any of the above

- The new SVG has **no `width`/`height`/`viewBox`** on the `<svg>` element —
  body parsing worked (sim-test's `TestFreighterEconomy` loads it), but if
  anything downstream reads `NSVGimage::width/height`, verify it copes.
- The model is now ~19x55 world units at `scale: 1.0` (was 30x10):
  half-diagonal ~29. `CGame::CheckLocalBulletHits`'s `LOCAL_HIT_RADIUS`
  (22, `src/cgame/cgame.cpp`) and its sizing comment reference the OLD
  30x10 freighter — revisit the comment (and possibly the value, though
  Marcin tuned 22 by feel for fighters; ask before changing it).
- Build native + wasm (`tools/wasm/build.sh` — remember `EMSDK_DIR`, and
  that asset-only changes need a `.cpp` touch to re-embed `GravitarisNG.data`)
  and run `gravitaris-sim-test` (checksums must stay deterministic).
