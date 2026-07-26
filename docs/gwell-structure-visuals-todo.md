# Gravity Well mode — structure visuals increment

Status: todo list written 2026-07-25, revised and then implemented same day
(see the Verification status note at the bottom); **largely superseded
2026-07-26 — see "Revision" at the bottom**. Follow-up to
`gravity-well-mode-plan.md` Phase 2, whose structure models were explicitly
placeholder ("Simple SVG box models ... visual polish later") — all seven
types were literally the same magenta 30–40 unit square, which is what made
a developed planet read as a pile of rectangles rather than a complex.

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

- [x] **Keep the current stroke widths.** They're consistent with the ships
  and read correctly through the bloom/CRT pipeline; this pass is about
  silhouette, not line weight.
- [x] Fix the rest of the shared vocabulary so seven models look like one
  family: unit grid, and which parts are team-coloured. `Model` already
  supports a team-colour placeholder on both stroke and fill
  (`TEAM_COLOR_PLACEHOLDER` in `src/cgame/resource/model.cpp`, `teamColor` /
  `fillTeamColor` in `include/gravitaris/cgame/resource/model.hpp`), so a
  neutral outline with a team-coloured filled core is available today —
  that's the original's Base.
- [x] **Planet/sun size stays as it is for this increment.** Growing them is
  the better long-term answer to "the complex doesn't fit inside the planet
  outline" — but it isn't a visual change: size comes from `scale = 0.2` in
  `data/models/planets/*/planet.toml`, and the collision shape, gravity
  source extent, safe-landing geometry, the classic scenario's orbit radii
  (2000–4800 in `src/game/scenario/classic-scenario.cpp`), and several
  hand-tuned sim-test distances all move with it. Deferred to its own pass
  so the art work isn't entangled with a flight-feel retune. (Done after all
  on 2026-07-26 — see the Revision at the bottom.)
- [x] Consequence: size the seven structures to fit the planet as it is
  today (~60-unit radius). The current 40-unit Base is too big — four
  planetside buildings plus spacing have to nest inside that outline, so
  budget roughly: Colony largest, Base slightly smaller, Lab and Comm Center
  clearly small satellites. Author them on a grid that rescales cleanly when
  planets do grow later.

## T2 — Author the four planetside models

Distinct silhouette per type; keep the physics `@body` layer roughly convex
and no larger than the visible model (Chipmunk shapes come from `@body`, see
`src/game/resource/`).

- [x] **Base** — squarish outline with a solid team-coloured core (reads as
  "the one that shoots"). Neutral outline, team fill.
- [x] **Colony** — tall narrow block, team-coloured outline, no fill (the
  original's tallest planetside shape; it's the producer).
- [x] **Lab** — small wide block, distinct from Colony by aspect ratio.
- [x] **Comm Center** — non-rectangular: dish/triangle + mast. This is the
  cheapest single win for "not all squares".

## T3 — Author the three orbital models

These are seen against black sky, not nested in the planet outline, so they
can be more detailed.

- [x] **High Port — do this one first.** Ring/hub silhouette, clearly the
  biggest orbital. It's the most-looked-at structure and the best test of
  whether T1's vocabulary works at orbital scale; the other two follow its
  lead.
- [x] **Space Dock** — open-jaw/cradle shape (a thing ships park in).
- [x] **Sensor Array** — thin mast + dish/antenna fan, visually light.
- [x] Orientation for orbital structures: **decided against**, no code
  change. All three silhouettes are rotationally symmetric enough (a ring, a
  cradle, a panel pair) that a fixed orientation reads fine, and leaving
  `StructureAttachmentSystem`'s `entity.has<Freighter>()` condition alone
  keeps the one case that genuinely needs facing (prograde freighters) the
  only special case in it.

## T4 — Placement pass

- [x] `BuildStartingComplex` hardcodes ±15/±10 offsets and a 180-unit orbit
  radius. Derive both from the planet's actual radius — that's what makes
  the deferred planet rescale (and Phase 6's generated sectors, where
  planets won't all be one size) a data change rather than another layout
  pass.
- [x] Planetside structures sit at world-axis offsets and never rotate, so a
  complex reads as "upright" only by luck of the planet's position. Orient
  each planetside structure radially (feet toward planet centre) — the
  original's buildings always stand on the surface. Same one-line-ish change
  in `StructureAttachmentSystem` as T3, plus an angle in
  `PlanetSurfaceAttachment` (currently spawn-time data — keep it derived, not
  replicated, if that works out).
- [x] Space out the four planetside buildings so none overlap at their new
  sizes; check against `EconomySystem`'s self-development placement, which
  spawns Lab/Comm Center independently of `BuildStartingComplex`'s layout —
  the two must agree or a grown complex will look different from a starting
  one.

## T5 — Verification

- [x] Native `GravitarisNG`, `gravitaris-server`, `gravitaris-sim-test` and
  wasm `GravitarisNG` all build clean; sim-test passes with the two-run
  checksum stable. `TestStructures`' offset/orbit-radius assertions survived
  T4 unchanged (they compare against the spawn-time values, not literals).
- [x] Native single-player run: the orbital trio renders distinctly through
  the bloom/CRT pass — ring, cradle and panel pair are unmistakable at play
  zoom, and the neutral-grey detail reads separately from the team-coloured
  structure.
- [ ] The **planetside** cluster hasn't been eyeballed in a live game yet:
  it needs flying to the home planet, which the screenshot pass didn't get
  to. Geometry is proven by the sim-test, appearance is not.
- [ ] Rough shape comparison against `docs/gwell/screenshots/start-game.png`
  — proportions and silhouettes, nothing pixel-exact.

Note on the wasm build: the `gravitaris-sim-test` *wasm* target does not
link (`WebRtcServerTransport` is native-only, and `TestWebRtcRoundtrip`
references it unconditionally). Pre-existing — that target has never
produced an artifact in `out/wasm-RelWithDebInfo` — and unrelated to this
work; the wasm target that matters, `GravitarisNG`, builds fine.

(Damage feedback is already handled: `Damageable::flashAmount` drives the hit
flash through `model-renderer2` for structures too.)

Done when: a developed planet reads as a settlement at a glance, and no two
structure types are confusable at play zoom.

## Revision — 2026-07-26

T2/T3/T4's art and placement were rejected on sight; structures are being
hand-drawn from here on, so this pass is now scaffolding for that, not a
finished look.

- **Planetside four are plain boxes again**, axis-aligned inside the planet
  outline rather than standing radially on the surface. Kept from T2: the
  differing proportions and the Base's team-coloured core. Dropped: the roof/
  dish/mast detail, the radial orientation, and with it
  `PlanetSurfaceAttachment::rot` and `StructureLayout::SurfaceAngle`/
  `SurfaceRotation`/`SURFACE_INSET`. `SurfaceOffset` is now a per-type slot
  expressed as a fraction of the planet radius.
- **Space Dock and Sensor Array are gone**, folded into the High Port: one
  drawing (`high-port-0`, hand-authored), one entity, one `StructureType`.
  Consequences: no High Port self-development branch in `EconomySystem` (a
  High Port is now its own freighter producer *and* funder), and
  `FactionSystem`'s respawn funding only requires an accompanying producer
  for a Base (its Lab). Split them back out if they ever need to be built or
  destroyed independently.
- **`high-port-0` always shows its `_thrust` group** — station-keeping, it is
  permanently correcting. Implemented in `ModelRenderer2::Render` (and
  `SimpleModelRenderer` for parity): an entity with no `Controls` burns its
  thrust group if it is a `Structure`.
- Unlike the px-authored ships, `high-port-0.svg` is on Inkscape's mm page
  grid (~25 units wide), hence `scale = 1.34` in its toml.
- **The High Port keeps its floor toward the planet** all the way around its
  orbit (local +Y radially inward), rather than holding a fixed orientation.
  Both `StructureAttachmentSystem` (server) and `SnapshotInterpolator` (which
  re-derives attached entities' transforms client-side) have to agree on
  this, same as they already did for a freighter facing prograde.

### T1's deferred celestial rescale, done 2026-07-26

Planets and suns doubled (`scale` 0.2 → 0.4 and 0.5 → 1.0: ~120- and
~300-unit radii), and everything measured against them moved with them:

- Masses go up **4x**, not 2x — surface gravity is `M/r²`, and the pull at
  the surface is the one number the whole game is tuned around. Orbital speed
  at a given *fraction* of the planet radius therefore rises by √2.
- `classic-scenario.cpp`: sun separation and all five orbit radii doubled.
- `RESPAWN_OFFSET_RADIUS` 200 → 420 and `FreighterSystem::ARRIVAL_RADIUS`
  220 → 440, both of which have to clear the High Port's orbit
  (`ORBIT_RADIUS_FACTOR`, since dropped to 2x the planet radius = 240).
- AI `evadeRadius` doubled across the presets, *and* `AIPilotSystem` now
  floors it at `EVADE_SURFACE_CLEARANCE` x the body's own radius. The
  authored values were planet-sized; a 300-unit sun sat entirely inside even
  a Cautious pilot's evade radius, so flying into it never registered as
  danger at all (pre-existing at the old sizes, worse at these).
- Planetside structure models are ~1.8x bigger, keeping roughly the
  screenshot's box-to-planet proportions inside the larger outline.
- Sim-test: the two-planet fixtures were 300/500 units apart, which is inside
  the new arrival radius (a freighter would have "arrived" the moment it
  spawned); now 900/1600, with a longer tick budget for the extra transit.

Not scaled, deliberately: weapon/ship-scale numbers (`fireRange`,
`standoffDistance`, bullet speeds, `engageRange`) — nothing about ships got
bigger.

### Draw order — 2026-07-26

Nesting the planetside boxes *inside* the planet outline exposed a latent
bug: draws were iterated in `unordered_map` hash order, and a planet's fill
is opaque, so whether the complex (and the ownership marker at the planet's
centre) survived or got painted over was down to the hash layout of the
loaded model set — which changed the moment `high-port`/`space-dock`/
`sensor-array` were deleted. `Model`/`Shape` now carry a `render_order` read
from the model toml (planets and suns declare `-10`), and `ModelRenderer2`
draws in `(render_order, model id)` order.

### Stations as places — 2026-07-26 (later)

- **The station orbits at a third of circular speed**
  (`StructureLayout::ORBIT_SPEED_FACTOR`). It flies its ring under thrust --
  its `_thrust` group burns permanently -- so it is not obliged to be
  ballistic, and at true circular speed matching it closely enough to set
  down on the deck is unreasonable. The factor rides the replicated
  `attachAngularSpeed`, so client rendering and prediction follow for free.
- **A High Port is a landing site.** Its deck faces away from the planet, so
  the planet's own legs-toward-the-body uprightness test works unchanged with
  the station as the body. Landing on one records it as the faction's last
  visited site, and `FactionSystem::SpawnPosition` now prefers a High Port
  over a planet in its fallbacks. `ConquestSystem` needed no guard -- it
  already requires an `Orbit`, which a structure hasn't got, so landing on a
  station cannot claim anything.
- **A launch inherits the site's motion** (`FactionSystem::SpawnPoint` now
  carries velocity and facing, not just a position). This was an outright bug
  before, not a nicety: a ship left at rest in world space beside a home that
  moves at ~90 u/s along its own orbit is immediately run over by it. Ships
  stand on the model's `spawn` hardpoint when it has one -- `Body::Hardpoint`
  carries the Inkscape *label* now, so a model can name a point the game
  looks up by meaning.
- AI spawns (debug panel and the J shortcut) go through the same rule, so a
  spawned fighter launches from Red's own station rather than appearing next
  to the player.

### Predicting the station — 2026-07-26 (later)

The station judder in multiplayer was not missing prediction -- it was
already re-derived client-side, analytically, exactly like a planet. It was
*when*: `planetTick` was a whole tick, while the own ship is drawn at
`tickFraction` between its last two predicted ticks and remote entities run
on a continuous `renderTick`. Anything attached to a planet therefore stepped
at 60Hz against a continuously-moving camera -- judder proportional to the
body's speed, unnoticeable on a slow planet and obvious on a station you fly
alongside. `EvaluateOrbit`/`EvaluateAttachment` take a fractional `atTick`
now, and CGame passes `wholeTick + tickFraction`.

Separately, `ClientPrediction::SyncCollisionProxies` builds proxies for
structures too (positioned by `EvaluateAttachment` on the parent's
same-tick analytic position, and rotated floor-inward to match the server),
so the predicted ship collides with -- and can land on -- a High Port
locally instead of only server-side.
