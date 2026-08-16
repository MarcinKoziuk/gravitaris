# Lag compensation — what is done, and what is left

Status: beams are lag compensated as of 2026-08-15 (`1e9213b`). Bullets and
missiles are not. This is the context for picking that up later, written while
it was fresh rather than reconstructed afterwards.

## What exists now

- **`InputCommand::viewDelay`** (protocol v11): how many ticks behind its own
  tick the sender's picture of everyone else was. Filled by
  `CGame::ViewDelayTicks` from `m_lastRenderTick`; zero for anything composed
  inside the sim (single player, AI, replays), which is why it is a *delay*
  rather than a tick — one number means both "no delay" and "not applicable".
  `InputSystem` copies it onto `Controls::viewDelay` beside the flags, so the
  shot a tick resolves is resolved against the world that command was aimed at.
- **`LagCompensation`** (`system/combat/lag-compensation.{hpp,cpp}`): a trail of
  where each damageable hull has been, `Record(step)` once per tick from
  `Game::Update` (after physics, before `DamageSystem`, so a trail holds exactly
  what that tick's snapshot will carry), and a `Rewind` RAII scope that moves the
  real Chipmunk bodies *and* their Transforms back, then restores them.
  `MAX_REWIND_TICKS = 30` — half a second at 60Hz.
- **`DamageSystem::ResolveBeams`** takes one `Rewind` per burning shooter, around
  all of that shooter's mounts and bounce legs.

## Two constraints that are not obvious, and cost a day between them

**The trail must stay a side table.** It was a component first. Giving every
damageable entity one more component changes which archetype table it lives in,
which changes the order every unrelated query walks the world in — and float
accumulation is not associative, so *the physics changed*. A landing test with
nothing to do with netcode is what caught it (`highport`, a hull wedging under a
station). The side table costs a hash lookup per hull per tick and moves nothing.
The check that it is still clean: sim-test's determinism hash was
`0x0cbc6cffd4a6307a` before and after the feature.

**A rewind cannot be taken inside a query.** It walks every hull, and a flecs
query run inside another query's callback silently iterates nothing here (see
CLAUDE.md) — it would rewind precisely nobody, and every networked shot would go
quietly back to missing with nothing to show for it. `ResolveBeams` gathers who
is firing into a vector first, then resolves outside the walk. Anything added
below has to keep doing that.

## Missiles: the small, well-understood piece

`ResolveBeams` gathers the in-flight rounds (`InFlight`) **before** any rewind,
so an interception is resolved against present-tick missile positions while the
hulls around them have been moved back. A beam aimed at where a missile *was*
therefore still misses it, which is exactly the bug lag compensation exists to
fix, one layer down.

The fix is mechanical:

1. `Record` also walks `(Transform, Missile)` — the trail table is keyed by
   entity id, so it does not care what the entity is, but the current query asks
   for `Damageable` and a missile carries none (its airframe is `Missile::hp`).
2. `Rewind` moves them too — they have a `PhysicsRef`, so the existing body-move
   path already works.
3. Gather the rounds **inside** the shooter loop rather than once up front. That
   loop is a plain `for` over a vector now, so a query there is legal; it is only
   a query inside a query *callback* that silently does nothing.

## Bullets: decide the model before writing any of it

A bullet is not a beam, and the standard answer is not "rewind it the same way".

A beam is resolved at the instant it is fired, so rewinding the world to what the
pilot saw is exactly right. A bullet is spawned on one tick and resolves its hit
over its swept segment on some *later* tick, by which time the world has
legitimately moved — rewinding the hit test to the firing tick would let a laggy
pilot kill something that had been behind cover for half a second. Every shooter
that has solved this treats projectiles differently from hitscan:

- What lag compensation owes a projectile is **where and when it appears**, not
  where it lands: it should leave the muzzle where the client saw the muzzle, and
  then be simulated forward by the sender's delay so it is not born half a second
  behind the world it is flying through ("projectile catch-up").
- The alternative — rewinding the world on the tick the bullet's segment is
  tested — is what to avoid, for the cover case above.

Open questions to settle first, since they change the work:

- **Is the shooter's own view of its rounds already right?** The client spawns
  cosmetic bullets locally (`CosmeticBulletDespawner`), so what the pilot sees
  leaves their own muzzle immediately. The question is what the *server's*
  authoritative round does, and how far the two drift over a flight.
- **How much catch-up is honest?** Advancing a fresh round by the full view delay
  puts it up to half a second down range the instant it spawns, which is visible
  to everyone else as a round that starts away from the hull that fired it.
- **Does the gun even need it?** Rounds are slow enough (200–420 u/s) that a
  pilot leads a target far more than latency displaces it. Worth measuring how
  much of a miss the delay actually causes before building anything: a sim-test
  in the shape of `TestLagCompensation` (fly a target across, fire where it was,
  compare hits with and without) would answer it in an afternoon.

Turret fire (`StructureDefenseSystem`) needs none of this: it is resolved
server-side from server state, and carries no view delay.

## The test pattern that works

`TestLagCompensation` in `tools/sim-test/main.cpp` is the template: fly a target
across the line of fire, aim at where it *was* `VIEW_DELAY` ticks ago, and assert
the shot lands with the delay declared and misses with it zeroed. Verify any such
test by breaking the mechanism on purpose (force `ViewTickOf(step, 0)`) and
watching it fail — a lag-comp test that cannot fail is measuring nothing.

Two traps that wasted time in the shield tests next door, and will again:

- A shield fitted with `FitFree` comes up **empty** and fills at a few points a
  second; a test that fires immediately is testing an unshielded hull.
- Filling `plates` past capacity is clamped by `ShieldSystem` on the next tick and
  shows up as a colossal fake "loss". Fill to `ResolveStats(...).shieldCapacity`.

## Where the seams are

Both bugs that survived review in this area lived at the **client/mirror seam**,
and neither was reachable by a sim-side test — the sim was correct both times:

- beams drew through enemy hulls in multiplayer because beam targets were
  gathered from the shooter's own world, and in a networked game the own ship is
  alone in `m_registry` while everyone else is in the mirror;
- the aim was measured from the ship's current sim position while the cursor was
  mapped through the previous frame's camera, so it was wrong in proportion to
  speed.

Anything that resolves in one world and draws across two deserves that suspicion
first.
