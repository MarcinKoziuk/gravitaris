# Converging the multiplayer and single-player client

Status: plan, written 2026-07-26. Step 1 is done; steps 2+ are not started.

Why: every camera/HUD feature has to be wired twice, and the multiplayer half
is the one that gets forgotten, because single-player is what you run while
developing. Two shipped bugs from exactly this, both found by playing rather
than by any test: the off-screen enemy arrows never ran in multiplayer at all
(`RenderNetClient` returned before the call, and the renderer they submitted
into wasn't the one drawing), and a joining peer spawned at a placeholder
world-origin offset because only the *respawn* path had been taught the
faction site rule.

## What the split actually is

On a multiplayer client:

- `m_registry` holds the own predicted ship (a real dynamic body) plus
  client-only kinematic **collision proxies** for things it must physically
  hit (`ClientPrediction::SyncCollisionProxies`).
- `m_mirrorWorld` holds a **rendered copy** of every replicated entity, fed by
  `SnapshotApplier` from `SnapshotInterpolator` output, drawn by
  `m_mirrorRenderer2`.

Single-player has neither: one world, one renderer, everything real.

## Step 1 — SceneView (done, 2026-07-26)

`SceneView { local, remote, overlays }` (`cgame/scene-view.hpp`) is what one
frame reads and draws into, built once per frame by `CGame::CurrentSceneView`.
`CameraDirector`, `MinimapRenderer`, `IndicatorRenderer` and the planet
ownership markers all take it instead of each growing its own optional
`flecs::world*` and trusting the call site to pass it.

This doesn't remove the split, it removes the *opt-in*: a consumer written
against a `SceneView` covers multiplayer whether or not its author thought
about multiplayer. It also deleted the registry references those consumers
held, which were only ever used for the sweeps.

## Step 2 — collapse the celestial duplication

**A planet exists twice on a multiplayer client** and the two copies are
genuinely redundant: the mirror entity and the collision proxy are both
positioned by `EvaluateOrbit` at the *same tick*. That is not incidental --
`SnapshotInterpolator` deliberately evaluates planets at `planetTick` rather
than the delayed `renderTick` precisely so the rendered surface and the
surface the predicted ship stands on cannot disagree. Structures (attachments)
got the same treatment on 2026-07-26.

So for planets and structures, one entity carrying Transform + Renderable +
kinematic body would do the job of both, and the analytic evaluation would run
once instead of twice. That is most of the entity count and all of the
"which world is this in" confusion for the static furniture of the world.

Work: `SnapshotApplier` creates celestials/structures with a `RigidBodyDesc`
instead of `ClientPrediction` making separate proxies for them; the analytic
positioning moves to whichever of the two owns them; `SyncCollisionProxies`
keeps only its remote-ship case. Est. a day, most of it in getting entity
lifetime right (a planet appearing in one snapshot and gone from the next).

## Step 3 — remote ships: NOT the same problem

I originally pitched "one world, remote entities *are* their proxies" for
everything. That is wrong for ships, and worth writing down so nobody tries
it twice.

A remote ship's two copies are at **two different ticks on purpose**:

- The **rendered** copy is interpolated at `renderTick`, deliberately delayed
  behind the estimated server tick (`m_interpDelaySeconds`), because smooth
  motion needs to sit *between* two known snapshots.
- The **proxy** is dead-reckoned forward to the tick being predicted, ahead of
  the server estimate by the input lead, because that is the tick the local
  ship's physics is actually stepping.

Merging them means picking one, and both choices are bad: render at the
predicted tick and remote ships jitter (that is what interpolation exists to
prevent); collide at the delayed tick and the local ship bounces off where an
enemy *was*, ~100ms ago.

The honest end state is therefore *not* one entity. It is either two entities
(what we have) or one entity with a physics transform and a separate render
transform -- which is more machinery, not less, and buys nothing beyond tidiness.
Unless the interpolation delay goes to zero (it won't), the split for ships
stays.

## Step 4 — one render path

With step 2 done, `RenderNetClient` and `Render` differ in: who advances the
sim (local `Game::Update` vs snapshot application + prediction), and the
mirror renderer. The rest -- camera, overlays, minimap, HUD, starfield,
post-process -- is already the same sequence written twice. Extracting the
common tail once the world split is down to "remote ships only" is mechanical,
and it is where the *next* forgotten-in-multiplayer bug would otherwise come
from.

Not worth doing before step 2: the two paths still differ in which renderer
draws, and that difference is exactly what the duplicated tail encodes.
