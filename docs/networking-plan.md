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

## Phase 0 — Sim hygiene (no networking code yet)

Goal: make the sim loop and its inputs server-grade. Everything here pays off
even if netplay never ships.

- [ ] **0.1 Fix the tick loop.** In `GravitarisApplication::tickEvent`, step
  the sim in a `while (m_frameTimeAccumulator >= PHYSICS_DELTA)` loop, with a
  cap (e.g. 5 steps/frame; drop the remainder beyond that and log once) so a
  debugger pause doesn't spiral. `FeedInput()` must run once per *step*, not
  once per frame (each sim tick needs its command — this is also why the
  input seam is tick-stamped).
- [ ] **0.2 `NetId`.** New replicated component
  `include/gravitaris/game/component/net-id.hpp`:
  `struct NetId { std::uint32_t value = 0; };` (0 = invalid). EntitySpawner
  assigns monotonically from a member counter on every spawn (player, AI,
  planet, bullet). Add a `NetIdRegistry` (game-side singleton or member of
  Game): `unordered_map<u32, flecs::entity>` maintained by spawner +
  an OnRemove observer. No consumer changes yet — this is plumbing.
- [ ] **0.3 Kill `std::rand`.** `SpawnRandomAIShip` picks its preset via
  SplitMix64 seeded from `(GetStep(), spawn counter)` — same pattern as
  DeathSystem's frag scatter.
- [ ] **0.4 State checksum + headless harness.** Add
  `Game::ComputeStateChecksum()` — FNV-1a over each entity's (NetId, quantized
  pos/rot/vel) in NetId order (ordering matters: flecs iteration order is not
  stable across worlds). New CMake target `gravitaris-sim-test`: links game/
  only (NOT cgame — this is ADR 0001's "cheap test"; if it won't link
  headless, a constraint got violated), spawns 2 AI ships + planet, steps
  1800 ticks, prints the checksum. Run twice, assert identical.

**Done when:** sim catches up after a stall (drop a breakpoint, resume, game
fast-forwards); `gravitaris-sim-test` builds without cgame and prints a stable
checksum; replays still work.

## Phase 1 — Centralized event queue (the quake3 part)

Goal: every gameplay one-shot flows through one serializable stream. This is
the "network boundary seam" — and it immediately fixes the audio observer
hacks, so it's valuable stand-alone.

- [ ] **1.1 The event types.** `include/gravitaris/game/event/game-event.hpp`:

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
- [ ] **1.2 The queue.** Game owns a fixed ring (`std::array<GameEvent, 256>`
  + head/count, like InputQueue) plus the seq counter. `Game::EmitEvent(type,
  sourceEntity, pos, param)` resolves the entity's NetId and stamps seq/tick.
  Consumers read via `Game::EventsSince(u32 seq)` (returns a span/pair of
  ranges) — multiple independent consumers, each tracking its own cursor,
  exactly how a per-client "events since last ack" will work in Phase 3.
- [ ] **1.3 Emit.** ShipControlsSystem → BulletFired (both weapons);
  DamageSystem → Impact (bullet hits) and LandingCrash (impact events over
  the damage threshold); DeathSystem → Explosion. All emitted server-side,
  same tick the thing happened.
- [ ] **1.4 Consume — AudioSystem.** Delete the Bullet OnSet observer and the
  `m_lastFlash` edge-detection maps; AudioSystem keeps an event cursor and
  plays laser/hit sounds from BulletFired/Impact. Thruster loops stay
  state-driven (correct — continuous). LandingCrash/Explosion get sounds when
  assets exist; wire them to the hit sound for now.
- [ ] **1.5 Move the hit flash client-side.** Remove `flashAmount` from
  `Damageable`; new cgame-side flash state (map NetId → amount inside
  ModelRenderer2's instance-color path, or a small `CGame` map consumed by
  it) set to 1 on Impact/LandingCrash events, decayed client-side. This fixes
  the replication-class violation and the missed-edge fragility in one move.
- [ ] **1.6 Replay check.** A recorded replay must produce the identical
  event stream (compare final seq + a checksum over (seq,type,tick) in the
  sim-test harness).

**Done when:** audio behaves identically by ear; AudioSystem contains no flecs
observers and no flash-edge maps; `Damageable` has no presentation fields;
replaying a recording yields the same event-stream checksum.

## Phase 2 — Snapshot serialization (still zero sockets)

Goal: world → bytes → world, proven inside one process. This is the highest
-value/lowest-risk replication step and needs no networking knowledge.

- [ ] **2.1 Byte IO.** `include/gravitaris/game/net/byte-stream.hpp`:
  `ByteWriter`/`ByteReader` (little-endian u8/u16/u32/u64/float, plus
  `WriteQuantizedFloat(v, min, max, bits)`). Unit-test roundtrips in the sim
  -test target.
- [ ] **2.2 EntityState.** What replicates per entity (keep it brutal-simple
  for v1): `netId, entityType (u8: Ship/Bullet/Planet), modelId (u32 hash —
  already stable, it's the asset-path FNV), teamId (u8), pos (2×f32),
  rot (f32), vel (2×f32), angVel (f32), controlsFlags (u8 pack — drives
  remote thruster visuals/sound), hp (f32)`. Planets replicate once
  (they're static); fine to include them in full snapshots v1.
- [ ] **2.3 SnapshotWriter (game/).** `WriteSnapshot(ByteWriter&)`: tick,
  entity count, EntityStates in NetId order, then all events since a given
  seq. Server-side, reads only replicated components.
- [ ] **2.4 SnapshotApplier (cgame/).** Applies a snapshot into a *client*
  world: creates missing entities by NetId (new spawn path in CEntitySpawner
  that attaches Transform/Team/Renderable but **no RigidBodyDesc — remote
  entities have no client physics**; ADR constraint 6 keeps prediction
  minimal), updates existing ones, destroys absent ones. Client-side Game
  systems (physics, AI, damage...) must not run on these — the client world
  in remote mode runs only presentation.
- [ ] **2.5 The mirror test.** Debug-only mode (F1 toggle or CLI flag):
  every tick, serialize the live Game and apply into a second flecs world
  rendered by the same renderers (swap via the existing renderer toggle
  pattern). The mirror must be visually indistinguishable except bullets/
  ships snapping at 60Hz (no interpolation yet). This proves the whole
  replication path with zero transport.

**Done when:** the mirror view plays identically; serialize+apply cost shows
up acceptable in the perf panel (< 0.5ms for a typical scene).

## Phase 3 — Transport & protocol (first real netplay)

Goal: two processes on localhost, second player visible and flying.

- [ ] **3.1 Transport choice: ENet** (zlib license, tiny, reliable+unreliable
  channels, sequencing, fragmentation — solves exactly the parts worth not
  hand-rolling; FetchContent like every other dep). Wrap it behind
  `INetTransport` (game/net/): `Send(peer, channel, bytes, reliable)`,
  `Poll() -> {Connected, Disconnected, Packet}` events. Also implement
  `LoopbackTransport` (in-process pair) so protocol tests run in the sim-test
  target without sockets.
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
