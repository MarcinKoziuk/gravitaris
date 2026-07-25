# Gravity Well mode — structure visuals increment

Status: todo list written 2026-07-25, revised same day after review with
Marcin. Follow-up to `gravity-well-mode-plan.md` Phase 2, whose structure
models were explicitly placeholder ("Simple SVG box models ... visual polish
later"). Today all seven types are literally the same magenta 30–40 unit
square (`data/models/structures/*/*.svg`), which is what makes a developed
planet read as a pile of rectangles rather than a complex.

Reference: `docs/gwell/screenshots/start-game.png` (labelled complex) and
`fight-near-base.png`. The original's planetside buildings *are* rectangles,
but they are differently-proportioned rectangles with a filled core on the
Base, and the orbital trio are chevron/arrow shapes — the silhouettes are
readable at a glance. **Match the rough shapes, not the pixels.** The goal is
"readable at a glance", better than the original, not a clone of it.

**Screenshot colours are not authoritative**: the original's vector display
flickers things white as they're drawn, so a structure that looks white in a
frame grab may well be team-coloured in motion. Take silhouette and
proportion from the screenshots; decide colour ourselves.

Scope: `cgame`/asset-side, plus placement constants in
`src/game/scenario/starting-complex.cpp`, `EconomySystem`'s
self-development placement, and (T1) celestial scale. No new components, no
snapshot bump.

## T1 — Model language + celestial scale (do this first, it constrains T2/T3)

- [ ] **Keep the current stroke widths.** They're consistent with the ships
  and read correctly through the bloom/CRT pipeline; this pass is about
  silhouette, not line weight.
- [ ] Fix the rest of the shared vocabulary so seven models look like one
  family: unit grid, and which parts are team-coloured. `Model` already
  supports a team-colour placeholder on both stroke and fill
  (`TEAM_COLOR_PLACEHOLDER` in `src/cgame/resource/model.cpp`, `teamColor` /
  `fillTeamColor` in `include/gravitaris/cgame/resource/model.hpp`), so a
  neutral outline with a team-coloured filled core is available today —
  that's the original's Base.
- [ ] **Planets and suns get bigger** rather than structures getting smaller
  — the complex should nest comfortably inside the planet outline like the
  original, and today a 40-unit Base against a ~60-unit planet radius can't.
  Size comes from `scale = 0.2` in `data/models/planets/*/planet.toml` and
  `data/models/stars/sun/`. This is **not** a pure visual change: the
  collision shape, the gravity source extent, and the safe-landing geometry
  all scale with it, and the classic scenario's orbit radii (2000–4800 in
  `src/game/scenario/classic-scenario.cpp`) are tuned against the current
  size. Change scale and orbit radii together, keep `mass`/`multiplier`
  fixed unless flight feel demands otherwise, then re-fly a landing before
  moving on.

## T2 — Author the four planetside models

Distinct silhouette per type; keep the physics `@body` layer roughly convex
and no larger than the visible model (Chipmunk shapes come from `@body`, see
`src/game/resource/`).

- [ ] **Base** — squarish outline with a solid team-coloured core (reads as
  "the one that shoots"). Neutral outline, team fill.
- [ ] **Colony** — tall narrow block, team-coloured outline, no fill (the
  original's tallest planetside shape; it's the producer).
- [ ] **Lab** — small wide block, distinct from Colony by aspect ratio.
- [ ] **Comm Center** — non-rectangular: dish/triangle + mast. This is the
  cheapest single win for "not all squares".

## T3 — Author the three orbital models

These are seen against black sky, not nested in the planet outline, so they
can be more detailed.

- [ ] **High Port — do this one first.** Ring/hub silhouette, clearly the
  biggest orbital. It's the most-looked-at structure and the best test of
  whether T1's vocabulary works at orbital scale; the other two follow its
  lead.
- [ ] **Space Dock** — open-jaw/cradle shape (a thing ships park in).
- [ ] **Sensor Array** — thin mast + dish/antenna fan, visually light.
- [ ] Orbital structures currently keep the fixed orientation they were
  spawned at (`StructureAttachmentSystem`, only `Freighter` gets a prograde
  `rot`). Decide per type whether to opt into an orientation — facing the
  planet suits High Port/Space Dock, and the hook is a one-line widening of
  that `entity.has<Freighter>()` condition into a flag on the attachment
  component.

## T4 — Placement pass

- [ ] `BuildStartingComplex` hardcodes ±15/±10 offsets and a 180-unit orbit
  radius. Derive both from the planet's actual radius so the layout survives
  both T1's rescale and Phase 6's generated sectors.
- [ ] Planetside structures sit at world-axis offsets and never rotate, so a
  complex reads as "upright" only by luck of the planet's position. Orient
  each planetside structure radially (feet toward planet centre) — the
  original's buildings always stand on the surface. Same one-line-ish change
  in `StructureAttachmentSystem` as T3, plus an angle in
  `PlanetSurfaceAttachment` (currently spawn-time data — keep it derived, not
  replicated, if that works out).
- [ ] Space out the four planetside buildings so none overlap at their new
  sizes; check against `EconomySystem`'s self-development placement, which
  spawns Lab/Comm Center independently of `BuildStartingComplex`'s layout —
  the two must agree or a grown complex will look different from a starting
  one.

## T5 — Verification

- [ ] Native single-player run: screenshot the starting complex at normal
  play zoom and at max zoom-out; all seven types identifiable without labels.
- [ ] Rough shape comparison against `docs/gwell/screenshots/start-game.png`
  — proportions and silhouettes, nothing pixel-exact.
- [ ] Re-fly a landing after T1's rescale (safe-landing geometry moved).
- [ ] wasm build clean; `gravitaris-sim-test` passes. Note there's nothing to
  re-baseline — the harness compares two runs of the *same* build against
  each other, not against a stored checksum, so a scale change moves both
  identically. What T1 does threaten is the hand-tuned geometry in the
  tests' own assertions: spawn clearances (Phase 1 records that under ~40
  units of surface clearance Chipmunk resolves the overlap by killing the
  ship), `StructureDefense::FIRE_RANGE` placements, and the structure
  offset/orbit-radius checks in `TestStructures`. Expect to retune those.

(Damage feedback is already handled: `Damageable::flashAmount` drives the hit
flash through `model-renderer2` for structures too.)

Done when: a developed planet reads as a settlement at a glance, and no two
structure types are confusable at play zoom.
