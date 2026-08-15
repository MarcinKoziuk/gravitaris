# Lasers — balance pass (windup, point aim, phaser note, point defence)

Status: written and implemented 2026-08-13, on branch `lasers`, on top of the
five commits that landed the beam weapon itself. **All four tasks are in**; the
tuning pass at the end of T1 is deliberately still open, and the three
deviations from this plan are recorded under "How it actually went" at the
bottom. Verified by `gravitaris-sim-test` (`TestLasers`,
`TestBeamsInterceptMissiles`) plus a client build; the feel itself wants
flying.

The beam as it was is a hold-the-trigger-and-it-is-there weapon: no commitment,
no telegraph, and its only cost is the bank. This pass gives it a commitment (a
windup you cannot take back), makes it aim at a *place* rather than a bearing,
replaces the electric buzz with a phaser note, and gives it the one job no gun
can do — killing a missile in flight.

Scope: `data/upgrades.toml` + `WeaponDef::Beam`, `Controls`,
`ShipControlsSystem::AdvanceCapacitor`, `DamageSystem::ResolveBeams`, the
client's aim path, `tools/gen-sfx.py`, `AudioSystem`, and one byte of the
snapshot (`SNAPSHOT_VERSION` 15 → 16).

Do them in order: T4 (missiles) is independent, but T1 changes when a beam
exists at all, so the interception rule wants the windup already in.

---

## T1 — Windup (~1 s, configurable, uncancellable)

- [x] `[weapon.beam] windup_seconds` in `data/upgrades.toml`, parsed into
  `WeaponDef::Beam::windupSeconds` (`include/gravitaris/game/upgrade/upgrade-def.hpp:40`,
  `src/game/upgrade/upgrade-catalog.cpp`). Seconds in the toml, ticks derived in
  code — matching `lifetime_seconds`, not `cooldown_ticks`.
  Start all three tiers at `1.0`; a rank that spins up faster is a tuning
  decision to make once it plays, not up front (see the open question below).
- [x] `Controls::laserWindup` — ticks left, a plain `std::uint16_t` field on the
  existing component (CLAUDE.md: a counter that changes every tick is a field,
  not a tag).
- [x] The rule goes in `ShipControlsSystem::AdvanceCapacitor`
  (`include/gravitaris/game/system/ship/ship-controls-system.hpp:94`) and
  nowhere else: it is the one function both the sim and `ClientPrediction` call
  exactly once per ship per tick, so it is the only place a windup timer can
  live without the two sides disagreeing about when a beam lights.
  - Rising edge of `flags.fireLaser`, hull has ≥1 `MountArm::Laser`, and the
    bank holds the start cost → `laserWindup = windupTicks`.
  - While winding: `laserFiring` stays **false** (no damage, no beam drawn),
    and the bank drains at the beam's own `energy_per_second`. Charging the
    emitter is what the energy is for; it also means a windup you waste costs
    you the same as a burn you don't.
  - Uncancellable: `flags.fireLaser` going false while `laserWindup > 0` is
    ignored entirely. The trigger is only read again once the counter reaches
    zero.
  - Completion → the beam burns while the trigger is held, with a floor of
    `MIN_BURST` (start at 0.35 s) so a released trigger still pays out a
    visible shot rather than a silent bill. See the open question.
  - Held through a burn: no re-windup. Release-and-press pays a new one, and so
    does a bank that ran dry mid-burn.
  - No separate cooldown after a burn: the capacitor's recharge is the pacing,
    and stacking a second timer on it would be pacing the same thing twice.
- [x] ~~Replicate it as one bit: `fireLaserWinding` in `ControlFlags` /
  `PackControlFlags`~~ — **changed during implementation** (deviation 1 below):
  it went out as `EntityState::laserWindup`, one `u8` of remaining ticks, with
  `SNAPSHOT_VERSION` bumped 15 → 16. A bit says a charge is running; the count
  says how far along it is, which is what the muzzle glow ramps over. Faking
  that from a local timer started on the bit's rising edge would have meant
  per-entity client state that a dropped packet or a late join gets wrong.
- [x] Check `ClientPrediction` replays the windup correctly
  (`src/game/net/client-prediction.cpp`): the counter is in `Controls`, which
  reconciliation already rolls back and re-steps, but `BoostEffectOf` exists
  precisely because some of that replay must *not* advance timers a second time
  — confirm the windup is on the advancing side of that split, not the
  read-only one.
- [x] Muzzle visual for the windup — a charge that grows where the beam is
  about to leave (`LaserRenderer::Charge`, gathered in `CGame::GatherBeams`).
  A telegraph the *target* can see is half the point of a windup, so this is
  not optional polish. Reusing the beam quad at near-zero length was tried
  first and rejected on sight: a short wide wedge is a *square*, which reads as
  a texture nobody drew. It is a disc now — a triangle fan with the light spent
  at the rim, through the same falloff the length of a beam uses — swelling out
  of nothing over the charge and then **staying lit at the root of the beam for
  as long as it burns**, so a mount reads as the same light throughout.
- [x] Balance consequence, and the reason this whole file exists: the beam is
  already the weaker-per-second weapon that pays in capacitor, and a 1 s windup
  is a large nerf on top. Expect to raise `damage_per_second` and/or push
  `falloff_start` out once it plays. Do that **after** T1–T4 are in — retuning
  against a weapon whose feel is still changing wastes the pass.

## T2 — Aim at the world point, not the bearing

Today the client re-derives an angle from the live cursor every tick
(`CGame::AimAt`, `src/cgame/cgame.cpp:262`, called from
`GravitarisApplication::UpdateAim`, `src/client/gravitaris.cpp:749`). Because
the camera follows the hull, a cursor sitting still on screen holds a roughly
*constant bearing* — so a firing ship that flies sideways drags its beam across
the map instead of holding it on the thing it was pointed at.

- [x] Keep the wire as it is and hold an aim POINT client-side
  (`GravitarisApplication::m_aimPoint`), deriving `aim = PackAim(atan2(point -
  shipPos))` from it every tick.
- [x] ~~Freeze the point while the beam burns, re-latching only during the
  windup~~ — **superseded**: the mounts now *chase* the cursor with a lag
  (`AIM_SLEW_TAU`, 0.12 s), because a beam that arrived wherever the mouse was
  that instant felt like a laser pointer. The chase is exponential and runs off
  real frame time, so the drag feels the same in a local game (once a tick) and
  a networked one (once a frame). Chasing in **world** space is what keeps both
  behaviours a pilot can use: swing the cursor and the beam catches up, fly past
  a spot you are holding and it stays on the spot.
- [x] Touches: a new `CGame::AimPointAt` (or `AimAt` returning the point and
  the caller packing it), `GravitarisApplication::UpdateAim`, and the two
  pointer handlers at `src/client/gravitaris.cpp:1256` / `:1270`.
- [x] Deliberately **not** wiring a position: `ControlFlags::aim` stays one
  packed `u16` bearing, the sim and `DamageSystem::ResolveBeams` keep resolving
  from it, and the command format, the replay log and the prediction history are
  all untouched. Sending `aimX/aimY` instead would cost 4–8 bytes on every command
  and a format bump, and buys nothing unless the *sim* has to own the latch (an
  AI that tracks a point, or not trusting the client's arithmetic). Note it as
  the fallback if either of those turns up.
- [x] No gimbal work: `ClampAimToArc`
  (`include/gravitaris/game/system/ship/ship-controls-system.hpp:192`) already
  pins to the nearer arc edge, so a latched point that drifts behind the ship
  behaves the way a cursor there behaves today.
- [x] `tools/sim-test/main.cpp` case: a ship translating under a fixed aim
  point keeps hitting a target parked at that point, where a fixed bearing
  walks off it.

## T3 — Make it sound like a phaser, not an arc projector

**Settled 2026-08-14: `comb-metal`**, after three rounds of auditions. A comb
filter — a delay fed back into itself, pulling one pitch and its harmonics out of
plain noise, which is a beam ringing down a pipe. `BEAM_VOICE` in
`tools/gen-sfx.py` names it and it is written as the authored
`laser-loop-1.wav` / `laser-windup-1.wav`; the losing profiles stay in the table
(they cost nothing there and record what was tried) and `--variants` writes them
out again for another comparison. The in-game picker that ran those auditions is
gone with them — it was always meant to end in the toml.

**Revised twice before that.** The first pass was tonal (sine partials, a resonant band, a
slow warble) and read as a hum with a hiss on it; the target is *pshhhh*, so the
second pass went noise-led by description. The third was measured.

A ship-phaser reference was analysed (`scratchpad/analyse-sfx.py` — decode via
ffmpeg, then band energies, spectral centroid, tonality and envelope) and the
numbers settled every open question:

| | reference | ours |
|---|---|---|
| centroid | 3820 Hz | 3810 Hz |
| tonality (top 40 bins) | 0.033 | 0.032 |
| < 600 Hz | 1.2% | 1.5% |
| 600–1500 Hz | 30.5% | 34.2% |
| 1500–3000 Hz | 33.4% | 31.7% |
| 3000–6000 Hz | 31.8% | 29.1% |
| > 6000 Hz | 3.1% | 3.7% |

So: noise, banded 600 Hz–6 kHz with nothing outside it, no tonal core at all.
Synthesised as two `resonator()` bands (1.4 kHz tight, 4.3 kHz loose) under one
steep shared lowpass. The mix weights were **solved, not searched**: independent
noise sources add in power, so measuring each component alone through the shared
filter and least-squaring the result against the reference's band shares lands
the fit in one step (`scratchpad/solve-beam.py`). Two structural findings came
out of that:

- the naive two-pole resonator passes DC at a gain comparable to its own
  resonance, so a bank of them piles up a bass rumble no amount of highpassing
  removes — zeros at DC and Nyquist are load-bearing, not decoration;
- a bank of two-pole bands leaks a tenth of its energy above 6 kHz (skirts fall
  at only 6 dB/octave), which reads as a thin *tss*. One steep lowpass over the
  sum fixes every skirt at once, which is also what a grid search could not do.

The windup is the reference's own onset, which measures quite differently from
its sustain: nearly all the energy starts inside one narrow band near 3.2 kHz
(half tonal — a whistle, not a hiss) and spreads out into the wide band as it
comes up. So the charge is one high-Q resonance loosening while the held pair
rises underneath it, compressed into the 0.2 s the windup now lasts.

Nothing is sampled: the clips are synthesised from those aggregate figures.

**Still not right by ear** (2026-08-14): the fitted voice reads as dull and low —
which the numbers agree with, since matching a reference's *spectrum* says
nothing about whether that reference is the sound this game wants. So the voice
is now a table (`BEAM_PROFILES` in `tools/gen-sfx.py`): a low band for the body,
an upper band for the edge, an optional `air` band, a shared lowpass and an
optional tremolo. Nine profiles are written as nine charge/held pairs, and both
halves of a pair share the profile — a bright charge in front of a dull note is
two sounds rather than one weapon.

Pick one by flying with it: the **Audio debug tab** (F8) lists them with an
audition button each, and **F9** cycles hands-free while the trigger is held.
Neither persists anything — the answer belongs in `data/upgrades.toml`, and
`AudioSystem::BEAM_VOICES` is the list to keep in step with the generator's.
`scratchpad/audition-beams.py` plays every voice as charge + four loops for
comparing them without flying.

Two auditions later (`octave-up`, then `octave-higher-sharp`) the remaining
question stopped being about numbers: filtered noise was the only *idea* in the
table, and its brightest members were all the same sound at different heights.
So the current set keeps those two as anchors and adds six other mechanisms —
each one a technique the screen ray gun actually uses, none of them a buzz:

| # | voice | mechanism | centroid | >3 kHz | tonality |
|---|---|---|---|---|---|
| 1 | authored | band stack (the fitted one) | 3794 Hz | 32% | 0.015 |
| 2 | octave up | band stack | 6970 Hz | 73% | 0.009 |
| 3 | higher + sharp | band stack | 8421 Hz | 93% | 0.012 |
| 4 | comb: metal pipe | feedback comb pulls a pitch out of noise | 9729 Hz | 84% | 0.008 |
| 5 | ring mod: no fundamental | noise × sine, sidebands only | 6787 Hz | 52% | 0.007 |
| 6 | FM: inharmonic ray gun | non-integer FM ratio | 8993 Hz | 89% | 0.165 |
| 7 | formants: held vowel | three tight inharmonic resonances | 6890 Hz | 84% | 0.011 |
| 8 | string: tuned sizzle | Karplus-Strong, excited continuously | 9896 Hz | 51% | 0.019 |
| 9 | array: stacked emitters | six detuned high bands, none dominant | 9088 Hz | 99% | 0.008 |

Two changes to the clips themselves, both from "the repeating sound is bad":

- **No tremolo, and no knob for one.** Anything that pulses reads as repetition.
- **Loops are 2 s, not 0.9 s.** Nothing inside the sound repeats, but the clip
  does, and with noise this dense the ear starts hearing the same texture come
  round as a rhythm nobody authored. A few hundred KB per voice buys half as
  many recurrences.
- **Levelled to a common RMS** (`match_rms`), not a common peak. Crest factors
  across these run from 2.1 to 5.0, so peak-matched clips differ by several dB
  of loudness and an A/B just picks the loud one.

A bug worth remembering from wiring this up: **a clip list loaded in
`AudioSystem`'s constructor is silent unless it is also uploaded explicitly after
the backend is created.** The clips load before `m_backend` exists, so the
observer that normally uploads a buffer no-ops, and a voice with no buffer plays
silence rather than reporting anything. Every other list in that constructor is
uploaded by hand for exactly this reason.

All of this is `tools/gen-sfx.py` plus one clip reference in the toml; the
generator writes every sfx procedurally, so there is no art dependency.

- [x] Rewrite `beam()` (`tools/gen-sfx.py:260`). What makes the current clip
  read as electricity is exactly what has to go: two detuned 196 Hz *sawtooths*
  (buzz — every harmonic present) and a 24 Hz tremolo (a hum's flutter).
  Phaser character instead: a small stack of *sines* (f ≈ 330–420 Hz plus 2f
  and 3f at falling gain), a resonant band around 1.1–1.3 kHz for the sizzle,
  gentle AM at 6–8 Hz and ~0.2 depth for the warble, and only a whisper of
  high-passed hiss. Keep the head-into-tail crossfade, and choose partials that
  complete a whole number of cycles over the loop so the crossfade is
  phase-continuous (the trick `thrust_boost()` documents at
  `tools/gen-sfx.py:110`).
- [x] New `beam_windup()` → `data/sounds/laser-windup-1.wav`: ~1 s, amplitude
  ramping in under a fundamental sweeping up exponentially, plus a rising
  resonant whine. It must **end on the pitch `beam()` begins on** so the loop
  takes over without a seam — that hand-off is the whole effect.
- [x] Append both `write_wav` calls at the end of `main()`, for the reason the
  comment at `tools/gen-sfx.py:302` gives: the RNG is seeded once per run, so
  anything inserted above silently re-rolls every clip after it.
  `laser-loop-1.wav` itself will change bytes — that one is intended.
- [x] `windup_sound` / `windup_sound_gain` on the `[[weapon]]`, resolved to
  `WeaponDef::windupSoundId` in `src/game/upgrade/upgrade-catalog.cpp` — same
  shape as `sound`/`sound_gain`, so a rank that charges differently stays a
  data edit.
- [x] `AudioSystem`: play it once on the rising edge of the new winding bit,
  positioned at the ship like `SweepBeams` does
  (`src/cgame/audio/audio-system.cpp:262`). The per-entity loop state
  `HoldLoop` keeps is the natural place to remember last frame's bit. A
  `LoopKind::BeamWindup` held while the bit is up would also work and
  self-cancels, but a rising sweep is a one-shot and would restart if the clip
  and the windup ever disagree in length.
- [x] Leave `laser-1.wav` alone — despite the name it is the autocannon's and
  the missile's pew, not the beam's.

## T5 — Field plating deflects a beam; a bubble does not (2026-08-14)

The plates' one real advantage over a bubble, and the reason to fit either: a
plate is a **mirror**. `laser_absorb` per level (0.5 / 0.4 / 0.3) is the share
taken into the shield, and the rest leaves the hull as a live beam — one number,
because nothing is lost in the bounce, so a better plate is one that *swallows
less*. A bubble has no entry and absorbs a beam whole.

- The bounce is the same beam, not an effect: it burns what it meets, the
  falloff keeps measuring the **whole** path (so what comes back is the spent
  end of one beam, not a fresh one), and it can burn the ship that fired it
  **regardless of friendly fire** — `HitSearch::selfIsTarget`, the one rule in
  the game that ignores its owner's colours.
- `ShipControlsSystem::MAX_BEAM_BOUNCES` (8) bounds it: two plated hulls facing
  each other are a closed optical path, so something has to. Both the sim and
  the renderer walk to exactly that depth, and `ReflectHeading` is shared, or a
  beam would burn one way while being drawn another.
- Drawn as one kinked beam: `LaserRenderer::Beam::fadeStart` carries the fade
  across the joint, and a deflected leg is drawn at its own strength (additive,
  so half a beam is half the colour).

**Worth knowing before tuning it:** a plate is a *slanted facet of a real hull*,
so a beam striking one square on leaves at twice the facet angle rather than
returning down its own path — measured at about 45° back the way it came on
fighter-1. Deflection therefore scatters rather than mirrors neatly, and burning
yourself takes a near-perpendicular hit. Two test attempts assumed otherwise.

Two other things that cost time here and are worth not rediscovering:

- A shield fitted with `FitFree` comes up **empty** and fills at a few points a
  second, so a test that fires immediately is testing an empty emitter (which is
  no mirror at all). Charge the plates by hand — and to their real capacity, not
  past it, since `ShieldSystem` re-sums the pool from the plates every tick and
  clamps each, which turns an overcharged fill into a colossal fake "loss".
- On a hull whose model authors no `+plating` paths the field is *pooled*, so a
  beam hits the hull polygon with no shield element at all. The mirror test is
  therefore not "did it hit a plate" but "did it meet the field": element set, or
  no plate geometry and charge left. (fighter-1 does author plates; other hulls
  may not.)

## T4 — A beam can shoot down a missile

Two separate reasons this cannot happen today, both of which have to go:

1. Missile and bullet shapes are sensors filtered into
   `PhysicsSystem::BULLET_GROUP` (`src/game/system/core/physics-system.cpp:419`),
   and `DamageSystem::QueryFirstHit` sweeps with *that same group*
   (`src/game/system/combat/damage-system.cpp:210`) so a shot is not blocked by
   other shots. Chipmunk skips same-group shapes, so no sweep in the game can
   see a missile at all.
2. A missile is `Bullet` + `Missile` and nothing else
   (`src/game/system/ship/ship-controls-system.cpp:500-505`), and
   `QueryFirstHit` drops anything without a `Damageable`
   (`src/game/system/combat/damage-system.cpp:227`).

- [x] ~~`PhysicsSystem::MISSILE_GROUP = 2`~~ and ~~`bool hitsMissiles` on
  `HitSearch`~~ — **both abandoned during implementation** (deviation 2 below).
  Written, and they worked as far as the filter: the missile's shape did land in
  its own group and the sweep did stop rejecting it. What killed the approach is
  a layer further down — Chipmunk's broadphase prunes candidates against the
  **raw segment** and only applies the query radius afterwards
  (`cpSpaceQuery.c`'s `SegmentQuery`, over `cpSpatialIndexSegmentQuery`), so a
  shape whose bbox misses the line is never tested however generous the radius.
  A missile is a few units off the line by construction, so it was never found.
  Both changes came back out; the corridor is measured instead (below), which is
  also what keeps gunfire flying through a round — a bullet sweep has no such
  corridor and a missile still carries no `Damageable`.
- [x] Hit points on the missile as **`Missile::hp`**, seeded from a
  `missile_hp` (or `[weapon] hp`) in the toml — *not* a `Damageable`. A
  `Damageable` on a missile would enrol it in every system that walks one:
  `MissileSystem`'s own target candidates (`src/game/system/combat/missile-system.cpp:48`
  — missiles would home on missiles), `StructureDefenseSystem`'s turret
  targeting, `DeathSystem`, which explodes a zero-hp entity *and throws
  shrapnel* and files a kill-feed report
  (`src/game/system/combat/death-system.cpp:48,109`), plus the HUD indicators,
  the minimap and `CameraDirector`. The cost of keeping it out is one explicit
  branch in `QueryFirstHit` (`Damageable` **or** an intercept-eligible
  `Missile`) and an explicit destroy in `ResolveBeams`; that is much the cheaper
  side of the trade.
- [x] Intercept in `ResolveBeams` (`src/game/system/combat/damage-system.cpp:418`):
  the rounds in flight are gathered into a plain vector **before** the walk over
  the beams (a nested flecs query would silently find none — CLAUDE.md), and
  each beam measures every round against its own line: distance along, distance
  off, both compared against the hull sweep's `alpha` so a hull in front still
  shields the round behind it. At ≤0 hp it goes on a **deferred destroy list**
  and emits `GameEventType::Explosion` for the pop and the noise, since nothing
  may be destructed mid-walk (the pattern the bullet loop uses at `:135-191`).
- [x] `DamageSystem::BEAM_INTERCEPT_RADIUS` — the corridor a round has to be
  inside, 14 units. Not in the original plan and load-bearing: a missile is
  about two units across and a beam leaves from a mount buried in a wing, so on
  a tight line interception is a coin toss no pilot could aim. Hulls are still
  met on the tight line, so a beam still burns what it is visibly crossing.
- [x] Add missiles to `CGame::m_beamTargets` (`src/cgame/cgame.cpp:1252`): that
  walk gathers `Transform` + `Damageable` + hull, so with hp living on
  `Missile` the *drawn* beam would pass straight through the round it is
  killing. A second short walk over `Transform` + `Missile` fixes it, against
  the same corridor radius the sim burns inside of.
- [x] Pick the hp number as a time-to-kill, not a hit point count: with rank-I
  at 95 dps, hp 6 dies in ~0.06 s at the muzzle and ~0.3 s at half reach, since
  `BeamFalloff` applies to a missile exactly as to a hull. That falloff is the
  balance story worth keeping — point defence works close in, and a missile
  killed at the far end of the reach is not a thing that happens.
- [x] Deliberately out of scope, note as follow-ups: AI pilots learning to
  intercept (`docs/ai-ships.md`), beams stopping plain bullets, and whether an
  interception credits anyone.
- [x] `tools/sim-test/main.cpp` case: a missile flown down a held beam dies
  before it arrives at close range, and survives at long range.

---

## Decisions (2026-08-13)

1. **A windup always fires.** That is what uncancellable means: the trigger
   released mid-windup still pays out the burn, with a `MIN_BURST` floor of
   0.35 s, and holding it burns on past that as it does today.
2. ~~**The aim point re-targets during the windup only.**~~ **Revised**: the
   mounts follow the cursor all the time, with a lag (see T2). A frozen point
   was the wrong half of the idea — what matters is that the beam is aimed at a
   *place* and swings to hold it, not that the pilot loses the mouse mid-burn.
3. **One windup for all three ranks**, ~~1.0 s~~ **0.2 s** — cut after flying
   the first version: a full second reads as the weapon arguing with the
   trigger. A fifth is still audible, still visible at the muzzle, and still
   long enough to be beaten by a pilot who moves the moment they hear it. A
   faster-charging rank III is a later tuning decision (and wants its own clip).
4. **A bank that empties mid-windup fizzles.** The windup runs to its end and
   lights nothing; the charge sound and the muzzle glow are what tell the pilot
   why.

## How it actually went

Three departures from the plan above, all made while building it:

1. **The windup replicates as a count, not a bit** (`EntityState::laserWindup`,
   `u8`, `SNAPSHOT_VERSION` 15 → 16). The muzzle glow ramps over the remaining
   ticks, and a client reconstructing that from a locally-started timer would
   get it wrong on a dropped packet or a late join. One byte per ship.
2. **Interception is measured, not swept.** Chipmunk's segment query prunes by
   the raw line before applying its radius, so the filter-group work in T4 was
   written, proven insufficient, and reverted — see the struck item there. The
   corridor is now plain geometry inside `ResolveBeams`, which also removes the
   need for a `CollisionClass::Missile`.
3. **`BEAM_INTERCEPT_RADIUS` is a new tunable** the plan never anticipated, and
   the number most likely to want changing after flying: 14 units is what makes
   point defence aimable. Too tight and no pilot can do it; too loose and a beam
   waved in the general direction sweeps rounds out of the sky.

Still open, deliberately: the tuning pass at the end of T1 (the beam has not
been paid back for the second it now costs), a faster charge for the upper
ranks, AI pilots that intercept, and a HUD readout for the charge — today it is
audible and visible at the muzzle but the capacitor bar says nothing about it.

Multiplayer has one known gap: a *drawn* beam is truncated against missiles in
the local sim's world, but the mirror world a net client builds has no `Missile`
component (missiles replicate as plain bullets), so on a client a remote ship's
beam is drawn through the round the server is burning down. The interception
itself is unaffected — it is the server's, and the explosion replicates.
