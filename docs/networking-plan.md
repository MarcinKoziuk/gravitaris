# Networking: preparation & execution plan

Companion to `docs/adr/0001-netcode-model.md` (authoritative server +
client-side prediction + snapshot interpolation; **read it first — its 8
constraints are binding for every task below**). This document turns that ADR
into ordered, independently-testable phases, written so that a session with no
context (or a smaller model) can execute one phase at a time.

Rules for executing this plan:

- **One phase per session/PR.** Each phase ends with a "Done when" gate; stop
  there and let the user playtest before starting the next.
- Never start phase N+1 with phase N's gate unmet.
- When a task says "sketch", the exact fields/names are a starting point —
  match surrounding code style (CLAUDE.md), but do not change the *semantics*
  without noting it here.
- Update the status block of the phase you worked on (mark tasks done, note
  deviations) — the same convention `docs/ai-ships.md` used.

## Current-state audit (2026-07-17)

What already aligns with ADR 0001:

- `game/` is headless-pure: no cgame/GL/Audio includes (verified by grep);
  wall clock only in logging timestamps. The Game/CGame split mirrors quake3's
  server/cgame.
- Input is already the right seam: tick-stamped `InputCommand` (with 1-byte
  wire packing) → per-entity `InputQueue` (64-deep ring, explicitly sized as
  "quake3 CMD_BACKUP") → `InputSystem` consumes at matching tick, repeats last
  command on gaps. Keyboard, AI pilots and replay all feed this seam;
  a network peer will be just another producer.
- Record/replay exists (`InputLog`, F5/F6/F7) and gameplay RNG is already
  deterministic (SplitMix64 seeded per (tick, entity id) in DeathSystem and
  AIPilotSystem) — replays are the determinism harness netcode needs.
- AI is server-side only (`AIPilot` is documented server-only, drives ships
  through the same InputQueue seam).

Known violations / gaps (fixed by Phase 0-1):

- **Tick loop**: `tickEvent()` steps the sim at most once per frame
  (`if (accumulator >= PHYSICS_DELTA)`, not `while`). Below 60fps the sim
  slows down instead of catching up. A server must never do this.
- **`std::rand()`** in `CGame::SpawnRandomAIShip` (cgame debug helper, but it
  mutates the sim — must become deterministic or a server-side command).
- **Gameplay one-shots are inferred client-side** instead of emitted as
  events: AudioSystem plays gunshots from a flecs observer on `Bullet`
  creation and hit-thuds from edge-detecting `Damageable::flashAmount == 1`.
  Both break under replication (a remote client sees snapshots, not component
  -creation order; a dropped/skipped snapshot misses the one-tick flash edge
  entirely). This is exactly what the centralized event queue replaces.
- **`Damageable::flashAmount` mixes replication classes**: presentation state
  (render flash) living in a replicated gameplay component (ADR constraint 2).
- **No `NetId`** yet (ADR constraint 3); `AIPilot::target` already documents
  "becomes NetId when netcode lands".

## quake3 → Gravitaris mapping (orientation for executors)

| quake3                       | Gravitaris equivalent                          |
|------------------------------|------------------------------------------------|
| `usercmd_t`                  | `InputCommand` (exists)                        |
| `CMD_BACKUP` ring            | `InputQueue` (exists, 64)                      |
| server game / cgame split    | `game/` / `cgame/` (exists)                    |
| `entityState_t`              | `EntityState` (Phase 2)                        |
| `svc_snapshot`               | `SnapshotPacket` (Phase 3)                     |
| `entityState_t.event` + `eventParm` + sequence | `GameEvent` stream with `seq` (Phase 1) |
| temp entities (EV_*)         | folded into the same `GameEvent` stream        |
| PVS / interest management    | distance-based relevance (Phase 6, deferred)   |

One deliberate difference: quake3 attaches events to entities inside
snapshots; we use a single global, sequence-numbered event stream (simpler to
reason about, trivially delta-able by "events since your last acked seq", and
it works identically offline). Spatial filtering can be added at Phase 6
without changing emitters.

---

## Phase 0 — Sim hygiene (no networking code yet) — DONE (2026-07-17)

Goal: make the sim loop and its inputs server-grade. Everything here pays off
even if netplay never ships.

- [x] **0.1 Fix the tick loop.** `GravitarisApplication::tickEvent` now steps
  the sim in a `while (m_frameTimeAccumulator >= PHYSICS_DELTA)` loop, capped
  at 5 steps/frame (drops the remainder past the cap and logs once) so a
  debugger pause doesn't spiral. `FeedInput()` runs once per *step* inside the
  loop, not once per frame.
- [x] **0.2 `NetId`.** `include/gravitaris/game/component/net-id.hpp` as
  specified. `EntitySpawner` owns the counter + `unordered_dense::map<u32,
  flecs::entity>` (matching the project's existing hash-map choice, see
  `ankerl` in CLAUDE.md) and an OnRemove observer, assigned on every spawn
  (player, AI, planet, bullet). **Deviation from the sketch**: the observer
  registration couldn't live in the constructor as planned — `EntitySpawner`
  is built via `CreateEntitySpawner()`, called as an *argument* to `Game`'s
  own (delegating/base-class) constructor, which runs *before* `Game::
  m_registry` is constructed; calling `m_registry.observer<NetId>()` at that
  point is UB (segfaulted the moment something in Init actually touched it
  substantively). Fixed with `EntitySpawner::Init()`, called from `Game`'s
  constructor *body* (after every member, including `m_registry`, is fully
  built) — this was a **latent bug in the pre-existing `Game`/`CGame`
  constructor pattern**, not something specific to NetId; anything future that
  needs a live `m_registry` at spawner-construction time must go through
  `Init()`, not the constructor.
- [x] **0.3 Kill `std::rand`.** `SpawnRandomAIShip` seeds via
  `SplitMix64Seed(GetStep(), spawnCounter++)` — same pattern as DeathSystem's
  frag scatter. The two pre-existing inline SplitMix64 copies (DeathSystem,
  AIPilotSystem) were consolidated into `game/util/splitmix.hpp` while at it
  (byte-identical algorithm, so replays are unaffected).
- [x] **0.4 State checksum + headless harness.** `Game::ComputeStateChecksum()`
  — FNV-1a over each NetId-bearing entity's (NetId, quantized pos/rot/vel),
  sorted by NetId first. New CMake target `gravitaris-sim-test`
  (`tools/sim-test/main.cpp`): links only the `game/` sources the main target
  itself lists (no `cgame/`, `Magnum::GL`, `Sdl2Application`, `Audio`, `RmlUi`,
  `ImGui`) — confirms ADR 0001 constraint 1 by construction: it either links
  clean or something pulled in a rendering/window/audio dependency. Spawns 2
  AI ships + `Game::Start()`'s planet, steps 1800 ticks (30s), runs twice from
  a fresh `Game`, asserts identical checksums. **This harness is what caught
  0.2's constructor-ordering bug** — first real payoff of building it.

**Done when:** sim catches up after a stall (drop a breakpoint, resume, game
fast-forwards); `gravitaris-sim-test` builds without cgame and prints a stable
checksum; replays still work.

**Verification status**: build clean, app launches/exits without error,
`gravitaris-sim-test` passes (two full runs, identical checksum) — done from
an unattended session, no interactive display available. **Not yet manually
verified**: the tick-loop catch-up under an actual debugger-pause stall, and
F5/F6/F7 record/replay still producing matching input. Do an interactive pass
on these before starting Phase 1 if they haven't been checked by hand yet.

## Phase 1 — Centralized event queue (the quake3 part)

Goal: every gameplay one-shot flows through one serializable stream. This is
the "network boundary seam" — and it immediately fixes the audio observer
hacks, so it's valuable stand-alone.

- [x] **1.1 The event types.** `include/gravitaris/game/event/game-event.hpp`:

  ```cpp
  enum class GameEventType : std::uint8_t {
      BulletFired,   // source = shooter,  pos = muzzle
      Impact,        // source = victim,   pos = hit point, param = damage*10
      Explosion,     // source = the ship that died, pos = its position
      LandingCrash,  // source = the ship,  pos = contact, param = damage*10
  };

  struct GameEvent {
      std::uint32_t seq = 0;     // globally monotonic, assigned by Game
      std::uint64_t tick = 0;
      GameEventType type{};
      std::uint32_t sourceNetId = 0;
      Magnum::Vector2 pos{};
      std::uint32_t param = 0;
  };
  ```

  Continuous states (thruster on, flash decay) are NOT events — they stay
  replicated component state. Events are strictly things that *happen*.
- [x] **1.2 The queue.** Game owns a fixed ring (`std::array<GameEvent, 256>`
  + head/count, like InputQueue) plus the seq counter. `Game::EmitEvent(type,
  sourceEntity, pos, param)` resolves the entity's NetId and stamps seq/tick.
  Consumers read via `Game::EventsSince(u32 seq)` (returns a span/pair of
  ranges) — multiple independent consumers, each tracking its own cursor,
  exactly how a per-client "events since last ack" will work in Phase 3.
- [x] **1.3 Emit.** ShipControlsSystem → BulletFired (both weapons);
  DamageSystem → Impact (bullet hits) and LandingCrash (impact events over
  the damage threshold); DeathSystem → Explosion. All emitted server-side,
  same tick the thing happened.
- [x] **1.4 Consume — AudioSystem.** Delete the Bullet OnSet observer and the
  `m_lastFlash` edge-detection maps; AudioSystem keeps an event cursor and
  plays laser/hit sounds from BulletFired/Impact. Thruster loops stay
  state-driven (correct — continuous). LandingCrash/Explosion get sounds when
  assets exist; wire them to the hit sound for now.
- [x] **1.5 Move the hit flash client-side.** Remove `flashAmount` from
  `Damageable`; new cgame-side flash state (map NetId → amount inside
  ModelRenderer2's instance-color path, or a small `CGame` map consumed by
  it) set to 1 on Impact/LandingCrash events, decayed client-side. This fixes
  the replication-class violation and the missed-edge fragility in one move.
- [x] **1.6 Replay check.** A recorded replay must produce the identical
  event stream (compare final seq + a checksum over (seq,type,tick) in the
  sim-test harness).

**Done when:** audio behaves identically by ear; AudioSystem contains no flecs
observers and no flash-edge maps; `Damageable` has no presentation fields;
replaying a recording yields the same event-stream checksum.

**Verification status**: build clean for both `GravitarisNG` and
`gravitaris-sim-test`; `gravitaris-sim-test` passes (two full runs, identical
state checksum *and* identical event-stream checksum — 21 events emitted both
times over 1800 ticks); `GravitarisNG.exe` launches, loads all resources
(fonts, audio backend, RmlUi), and runs several seconds with no errors or
warnings beyond the expected sim-step-cap message from running headless.
Done from an unattended session, no interactive display available. **Not yet
manually verified**: audio "sounds right" by ear, hit-flash visuals render
correctly on-screen, and a real F5/F6/F7 replay reproduces the same
event-stream checksum interactively (only the scripted two-run sim-test
comparison was exercised). Do an interactive play-test pass on these before
starting Phase 2 if they haven't been checked by hand yet.

## Phase 2 — Snapshot serialization (still zero sockets)

Goal: world → bytes → world, proven inside one process. This is the highest
-value/lowest-risk replication step and needs no networking knowledge.

- [x] **2.1 Byte IO.** `include/gravitaris/game/net/byte-stream.hpp`:
  `ByteWriter`/`ByteReader` (little-endian u8/u16/u32/u64/float, plus
  `WriteQuantizedFloat(v, min, max, bits)`), explicit byte-by-byte so the
  layout is host-endianness/padding-independent (native and wasm must
  interoperate). Roundtrip + overrun-latch tests run in the sim-test target.
- [x] **2.2 EntityState.** As specced, plus one deviation: **`scale (2×f32)`
  added** — bullets spawn with Transform scale {3,3}, so a remote client
  rendering them without it would draw them 3x too small. Quantized floats
  exist in ByteWriter but v1 uses plain f32 everywhere, per the sketch.
- [x] **2.3 SnapshotWriter (game/).** Split into `GatherSnapshot` (world ->
  `SnapshotData`) + `SerializeSnapshot` (`SnapshotData` -> bytes), with
  `WriteSnapshot` as the combined convenience — the split is what lets the
  sim-test prove gather -> serialize -> parse -> re-serialize is
  **byte-identical** without linking cgame. Version byte + count sanity caps
  on read. Entities NetId-ascending (canonical order, delta-able later).
- [x] **2.4 SnapshotApplier (cgame/net/).** As specced: creates by NetId with
  Transform/Team/Renderable/HitFlash and **no RigidBodyDesc**, updates,
  destroys absent. Deviations: it owns its own NetId->entity map (no
  CEntitySpawner involvement — server NetIds are applied as-is, and
  EntitySpawner's job is *assigning* ids, which the client must never do);
  `Controls` is also emplaced on Ship-type entities (unpacked from
  controlsFlags) because ModelRenderer2 gates the `_thrust` tag group on
  `Controls::actionFlags.thrustForward` — exactly the "drives remote thruster
  visuals" the field was specced for. hp is parsed but not applied (nothing
  renders health), events are parsed but left to the caller.
- [x] **2.5 The mirror test.** Implemented as a third entry in the existing
  renderer toggle (`RendererKind::Mirror`, Renderer debug tab): while active,
  every rendered frame serializes the live Game, parses it back, applies into
  `CGame::m_mirrorWorld`, and draws that world via a second ModelRenderer2
  instance (constructed before any model loads so ResourceLoader's
  OnCreate<Model> bakes into both; debug-only duplicate GL memory).
  Serialize+apply cost shows as "Snapshot Mirror" in the perf panel. Known
  mirror-mode limits: HUD arrows hidden (overlays ride the real renderer's
  draw), hit-flash doesn't flash in the mirror (flash events apply to the
  real world's entities), audio still plays from the real sim's queue.

**Done when:** the mirror view plays identically; serialize+apply cost shows
up acceptable in the perf panel (< 0.5ms for a typical scene).

**Verification status**: both targets build clean; `gravitaris-sim-test`
passes with the new gates — byte-stream roundtrips (including quantized-float
step tolerance and overrun latching), snapshot gather -> serialize -> parse ->
re-serialize byte-identical, entities strictly NetId-ascending — and the
determinism checksums are **unchanged from pre-Phase-2** (state
0x1a3096e5f4b36217, events 0x2e16d87965684ca9), proving GatherSnapshot reads
without perturbing the sim. Done from an unattended session. **Not yet
manually verified**: the mirror renderer toggle itself (visual identity, the
< 0.5ms perf-panel gate) — needs an interactive pass: F1 -> Renderer tab ->
"Snapshot mirror (net debug)", fly/fight, compare against ModelRenderer2 and
check the "Snapshot Mirror" perf section.

## Phase 3 — Transport & protocol (first real netplay)

Goal: two processes on localhost, second player visible and flying.

- [ ] **3.1 Transport choice — constrained by the wasm port (2026-07-19).**
  The browser build is a supported target, and browsers cannot open raw UDP
  sockets: ENet only works native. So `INetTransport` (game/net/) is the
  load-bearing decision, not ENet: `Send(peer, channel, bytes, reliable)`,
  `Poll() -> {Connected, Disconnected, Packet}` events, implementations
  per platform. Plan: `LoopbackTransport` first (in-process pair, protocol
  tests in sim-test without sockets), then **WebSockets as the first real
  transport** — Emscripten maps POSIX TCP sockets to WebSockets out of the
  box, a native server can terminate them with a small library, and one
  transport that works for BOTH native and browser beats maintaining two
  from day one. TCP head-of-line blocking is acceptable at this stage (the
  protocol's unreliable-by-redundancy design still applies; it just rides a
  reliable pipe). ENet (or WebRTC datachannels for true unreliable-to-
  browser) becomes a second `INetTransport` impl later **only if** HOL
  blocking measurably hurts.
- [ ] **3.2 Protocol v1** (all little-endian, first byte = packet type):
  - `ClientHello {protocolVersion, name}` (reliable)
  - `ServerWelcome {clientId, yourShipNetId, tickRate}` (reliable)
  - `ClientInput {lastAckedSnapshotTick, count, InputCommand[count]}` —
    unreliable, sent every client frame, containing the last ~8 commands
    (redundancy instead of reliability, quake3-style; InputQueue dedupes by
    tick on the server).
  - `Snapshot {serverTick, eventSeqBase, payload}` — unreliable, 20Hz, full
    snapshots v1 (ADR constraint 8 explicitly allows this; the EntityState
    split is what makes deltas possible later). Events ride in every snapshot
    from the client's acked seq; duplicates are fine (client drops seq ≤
    cursor) — that's the loss-tolerance.
- [ ] **3.3 Roles.** `--server [port]` (headless-capable), `--connect
  host:port`, default = single-player (listen-server can wait; simpler to
  debug two processes). Server: Game + transport, steps on a fixed-rate loop.
  Client in remote mode: no local Game stepping — FeedInput sends
  ClientInput; SnapshotApplier owns the world; render + audio consume it and
  the replicated event stream (Phase 1/2 seams make this mostly wiring).
- [ ] **3.4 Remote player's ship**: server spawns a player-type ship per
  connected client and pushes that client's InputCommands into its
  InputQueue — from the sim's perspective a network player IS the existing
  seam, nothing new.

**Done when:** two instances on one machine: both players see each other fly,
shoot, take damage, die, respawn; kill -9 of the client doesn't disturb the
server.

## Phase 4 — Interpolation

Remote entities render ~100ms behind: per-entity buffer of the last N
snapshot transforms in cgame, render at `serverTick - interpDelay`, lerp
between straddling snapshots (slerp-equivalent shortest-arc for rot).
Tunables in a debug tab (delay, extrapolation cap ~50ms). Player's own ship
still snaps (prediction is Phase 5).

**Done when:** at a simulated 20Hz snapshot rate motion looks as smooth as
local play; artificial 100ms +30ms jitter (add a debug-tunable delay queue in
LoopbackTransport/ENet wrapper) stays playable.

## Phase 5 — Prediction & reconciliation (own ship only)

The hard one; ADR constraint 6 defines the approved approximation. Client
steps its own ship locally each tick (input applied immediately), keeps
unacked commands; on snapshot: compare server state for own ship at acked
tick vs predicted history; if error > epsilon, snap to server state and
re-simulate pending commands against gravity + static bodies only (no
ship-ship contacts during replay — document artifacts). Requires: per-tick
predicted-state history ring; `Game`-side "step one body" helper exposed for
the client. Smoothing: blend visual correction over ~100ms.

**Done when:** at 100ms simulated latency your own ship feels local; firing
feels immediate (fire events also predicted locally: play the shot sound on
input, server confirms).

## Phase 6 — Deferred (needs its own design pass when reached)

- Delta-compressed snapshots (per-entity change masks vs last acked).
- Relevance/interest management (distance culling per client).
- Lag compensation for hits (ADR constraint 7: ~1s transform history ring,
  rewind DamageSystem's swept segment query to the shooter's view tick).
- Clock sync polish, connection quality HUD, host migration, encryption/auth.

---

## Invariants checklist (apply to EVERY gameplay PR from now on)

- No wall-clock, no `std::rand`, no iteration-order dependence in `game/`.
- New gameplay one-shots → `Game::EmitEvent`, never a cgame-side observer.
- New components declare their replication class (replicated / client-only /
  server-only) in a comment, and replicated ones stay POD + NetId-referencing.
- `gravitaris-sim-test` still builds (headless) and its checksum is stable
  across two runs.
