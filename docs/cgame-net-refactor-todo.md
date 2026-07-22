# cgame.cpp net-client refactor — technical todo

Status: Stage 1 (all) and Stage 2 (2.1-2.3; 2.4 skipped as optional/low-value)
implemented and verified 2026-07-22. Stage 3 deliberately not started, per
Marcin's own call ("do the ones that make sense for sure, look at risky/
unsure ones later"). Originally written 2026-07-21 after Phase 4 landed,
when net-client handling in `CGame` had grown past the point where the
*sequence* of operations is readable at a glance.

`cgame.cpp` went from 576 to 407 lines, `cgame.hpp` from 393 to 343 -- not
just deletion, most of that moved into five new purpose-built classes/
structs (`RollingHistory`, `NetDiagnostics`, `PredictedTickClock`,
`OwnShipSync`, `CosmeticBulletReaper`, `RemoteEventApplier` -- six, actually)
across `include/gravitaris/{game,cgame}/net/`. Every stage below was
verified with native + wasm builds, `gravitaris-sim-test` (checksum stayed
byte-identical to the pre-refactor baseline through every single step --
confirms none of this leaked into the simulation), and live smoke tests.
One live multiplayer smoke test hit the same "client silently vanishes, zero
error output" artifact this session had already independently attributed to
environment/GPU contention on this dev machine (reproduced even with a
single client, no second GL process to contend with, and a genuine
pre-existing Corrade warning -- "Cannot query global symbol... App may
misbehave" -- present in the log) -- not attributed to this refactor.

## Why (measured, not vibes)

`src/cgame/cgame.cpp` is 576 lines. Net-client code is ~383 of them (66%):

| Member | Lines | Concern |
| --- | --- | --- |
| `ConnectToServer` | 6 | session setup |
| `TickNetClient` | 81 | own-ship lifecycle + tick clock + input send + step |
| `CheckLocalBulletHits` | 71 (30 code, 41 comment) | cosmetic bullet stop |
| `ReconcileOwnShipIfNeeded` | 39 | reconcile + visual correction + diagnostics |
| `ApplyRemoteEvents` | 57 | event dedup, re-emit, bullet match, hit flash |
| `RenderNetClient` | 129 | tick math, interpolation, camera, 2 renderers |

`cgame.hpp` carries ~20 net fields and ~15 net getters, plus a **public
nested `RollingHistory` struct that exists only so `net-panel.cpp` (a
different TU) can name the type** — a clear sign this state wants to live
somewhere else.

The single-player path, by contrast, is ~45 lines and reads top-to-bottom.

## Guard rails (read before touching anything)

Most of this code is **the written record of specific playtest bugs**, and
the comments are load-bearing. Refactoring must preserve them, not summarize
them away. In particular, these orderings are deliberate and each fixed a
real, reproduced bug:

1. `CheckLocalBulletHits()` runs in `RenderNetClient` **immediately after**
   `m_snapshotApplier.Apply(...)`, not in `TickNetClient`'s fixed-step loop
   — the loop can run up to 5 catch-up ticks per frame while the mirror
   world only refreshes once, so checking inside it compared an already
   -advanced bullet against a stale enemy position.
2. `m_visualCorrectionOffset` decays **before** `m_cameraDirector.Update`,
   because every framing rule reads player position once at the top of that
   call.
3. The own-ship draw-position override (save `Transform::pos`, overwrite,
   render, restore) must leave the real transform untouched — the next
   predicted tick builds on it.
4. `FreighterSystem`-style collect-then-mutate discipline applies to any
   flecs iteration that destructs or adds components (see the Phase 4
   `GetOrCreate`-inside-`each()` segfault).

**Verification for every stage below**: native + wasm build clean;
`gravitaris-sim-test` passes with an **unchanged** state checksum (this is
pure client-presentation code — if the sim checksum moves, something leaked
into the simulation); and a two-client live smoke test (fire at a peer, take
a hit, watch a freighter fly) since sim-test does **not** cover the cgame
render path at all.

## Stage 1 — mechanical extractions (no behavior change) [x] done 2026-07-22

Each item is a move + rename, verifiable by the checksum staying identical.
Do them one at a time, building between each.

### 1.1 `RollingHistory` → its own header

Move out of `CGame` into `include/gravitaris/cgame/net/rolling-history.hpp`
as a plain top-level struct. Removes the "public nested type so another TU
can spell it" wart, and unblocks 1.2.

### 1.2 `NetDiagnostics` struct

Group `m_resyncEventCount`, `m_lastResyncDriftTicks`, and the three
`RollingHistory` members into one struct (`cgame/net/net-diagnostics.hpp`).
Collapses ~5 fields and ~5 getters on `CGame` into one accessor;
`net-panel.cpp` reads the struct directly instead of 5 separate getters.
Pure instrumentation, so this is the safest possible first move.

### 1.3 Deduplicate the "find my ship in a snapshot" lookup

The same `std::find_if(... e.netId == GetYourShipNetId())` appears **three
times** (twice in `TickNetClient`, once in `ReconcileOwnShipIfNeeded`), once
as `any_of` and twice as `find_if`. One small free function
(`FindOwnShipState(const SnapshotData&, std::uint32_t)` returning
`const EntityState*`) replaces all three.

### 1.4 `PredictedTickClock`

Extract `m_nextPredictedTick` + the ~30-line drift guard from
`TickNetClient` into `cgame/net/predicted-tick-clock.hpp/.cpp`:

```
Advance(estimatedServerTick) -> tick   // resyncs internally if drifted
Current() / Reset(tick)
```

This is the most self-contained concept in the whole file (one counter, one
threshold, one log line, two diagnostics writes) and it's **pure integer
math — directly testable in sim-test**, which nothing in the current
net-client path is. Worth doing for the test coverage alone.

### 1.5 Tick arithmetic helpers

`RenderNetClient` computes `estimatedServerTick`, `interpDelayTicks`,
`renderTick` (with hand-rolled saturating subtraction), and `planetTick`
inline. Fold into small named helpers next to 1.4. The saturating
subtraction in particular is easy to get wrong twice.

## Stage 2 — concept consolidation (behavior-preserving, needs care) [x] 2.1-2.3 done 2026-07-22, 2.4 skipped

### 2.1 `OwnShipSync` — one owner for "my predicted ship"

Implemented in `include/gravitaris/game/net/own-ship-sync.hpp` (not
`cgame/net/` -- it has zero cgame/GL dependency, same precedent as
`ClientPrediction`/`NetClient`/`PredictedTickClock`). Ended up as
`DropIfStale()` / `SpawnIfConfirmed()` / `ReconcileIfNeeded()` /
`DecayCorrection()` + `GetCorrectionOffset()` rather than the
`SyncToSnapshot`/`Reconcile`/`RenderPosition` shape sketched below --
`CGame::m_player` (a `Game`-base-class field OwnShipSync has no access to)
still has to be set by the caller, so the methods return enough for `CGame`
to do that itself rather than trying to own it. Like `RemoteEventApplier`
(2.3), it needs a live `NetClient&`, so it's `std::optional<OwnShipSync>`,
constructed in `ConnectToServer` once `m_netClient` exists.

Currently split across three members: the staleness check + spawn gate
(`TickNetClient`), the reconcile + correction-offset accumulation
(`ReconcileOwnShipIfNeeded`), and the offset decay + draw override
(`RenderNetClient`). One class in `cgame/net/` should own the full
lifecycle:

```
SyncToSnapshot(snapshot, yourNetId, ...)   // stale-drop + spawn gate
Reconcile(snapshot, ...)                   // -> visual correction
DecayCorrection(dtSeconds)
RenderPosition(realPos) -> Vector2         // the smoothed override
```

`m_visualCorrectionOffset` becomes private to it. Biggest readability win
available, because it makes the *reason* those three fragments exist
(hiding a correction snap) visible in one place instead of three comments
in three functions.

### 2.2 `CosmeticBulletReaper` — one owner for "stop my own bullet"

There are deliberately **two independent** stop mechanisms — the local
proximity check (`CheckLocalBulletHits`) and the server `Impact`-event match
(inside `ApplyRemoteEvents`) — because `GameEventQueue`'s delivery is
fire-once with no real resend, so the event alone can't be relied on. That
rationale is currently explained across two long comments in two functions,
and the two radii (22 and 100) are tuned for different reasons in different
places. Put both in one small class so the "belt and braces, here's why"
story is told once.

### 2.3 `RemoteEventApplier`

`ApplyRemoteEvents` does four things: seq dedup, own-`BulletFired`
suppression, re-emit into the local queue, and hit-flash routing (plus
2.2's bullet match). Extracting it as its own type mostly buys a clean seam
between "decode the event stream" and "react to it".

### 2.4 `MirrorWorld` wrapper (optional)

`m_mirrorWorld` + `m_snapshotApplier` + `m_mirrorRenderer2` are always used
as a trio. Wrapping them would also tidy two API smells they cause: the
static `HitFlashSystem::Decay(world, dt)` call, and
`SubmitPlanetOwnershipMarkers(world, renderer)` taking the pair explicitly
because two different (world, renderer) combinations exist. Lower value than
2.1/2.2 — do it only if it falls out naturally.

### 2.5 Result: what `CGame` keeps

`RenderNetClient` now reads as: update client → apply events → reconcile →
interpolate + apply → reap bullets → decay correction → camera → renderers.
The renderer orchestration (zoom/camera/line width, the two `Render` calls)
legitimately stays in `CGame` — it owns the renderers. Landed at 407 lines
for `cgame.cpp`, not the 250-300 guessed here beforehand -- the guess
undercounted how much of the file is renderer setup/orchestration that was
never going to move (that's `CGame`'s actual job), not net-client logic.

## Stage 3 — riskier ideas, decide later

### 3.1 A generic "presentation offset" instead of transform mutation

The own-ship draw override works by mutating `Transform::pos`, rendering,
and restoring. It's honest and well-commented, but it's a hidden temporal
coupling: anything that reads `Transform` during that window sees a lie.
A `RenderTransform`/presentation-offset component the renderer adds in would
be cleaner and would generalize to any future smoothing — but it touches
`ModelRenderer2`'s hot instancing loop, so it's a real cost/benefit call,
not a free win. **Discuss before starting.**

### 3.2 Umbrella `NetClientSession`

Wrap transport + client + prediction + the Stage 2 pieces so `CGame` holds
one pointer instead of ~8 fields. Attractive on paper, but the render half
genuinely needs `CGame`'s renderers and camera, so the session can only own
*state and logic*, not the frame. Worth it only after Stage 2 shows what the
real seam is — doing it first would guess wrong.

## Explicitly NOT recommended

- **Don't** try to unify `m_registry` (own ship) and `m_mirrorWorld`
  (everyone else). The split is ADR 0001's deliberate design: the mirror
  world is presentation-only with no physics. Several comments already
  depend on that invariant.
- **Don't** collapse the two bullet-stop mechanisms into one "cleaner" path.
  Both exist because either alone was observed to fail. 2.2 consolidates
  where they *live*, not whether both run.
- **Don't** rewrite the load-bearing comments into shorter ones. They
  encode reproduced bugs and the reasoning that fixed them; several say
  things the code cannot.
