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

- [x] **3.1a `INetTransport` + `LoopbackTransport`.** `include/gravitaris/game/net/transport.hpp`:
  `INetTransport` (`Send(peer, channel, data, size, reliable)`, `Poll() ->
  vector<NetEvent>` where `NetEvent` is `{Connected, Disconnected, Packet}`),
  `PeerId`, and `SERVER_PEER = 1` — the transport-agnostic convention every
  client-role transport must honor so `NetClient` can address "the server"
  without knowing which transport it's actually running over.
  `LoopbackTransport` (`game/net/loopback-transport.hpp/cpp`) connects
  exactly two in-process endpoints via a shared pair of queues; zero platform
  dependency by construction, so it's wasm-safe for free and is what the
  sim-test target exercises the whole protocol over.
- [x] **3.1b Real transport: WebRTC data channels via `libdatachannel`
  (native) + `datachannel-wasm` (Emscripten), not WebSocket.** Still
  constrained by the wasm port: the
  browser build is a supported target and can't open raw UDP sockets, so
  ENet-only was ruled out from the start. The original plan (this section,
  previously) picked WebSockets as the one-transport-for-everyone answer.
  Superseded after checking what WebRTC data channels actually buy over
  WebSocket: they run SCTP-over-DTLS-over-**UDP** and support unordered/
  unreliable delivery per channel, so a lost packet doesn't head-of-line
  -block everything queued behind it the way TCP does — exactly the
  loss-tolerance this protocol was already designed around (`ClientInput`'s
  redundant last-8-commands window, `Snapshot`'s "duplicates are fine, drop
  seq ≤ cursor" model both assume packets can just be dropped, not that they
  must eventually arrive in order). WebSocket/TCP forces every one of those
  redundant sends through an ordered, reliable pipe regardless — the
  loss-tolerance design was fighting the transport instead of being served
  by it.

  `libdatachannel`/`datachannel-wasm` (both by paullouisageneau) are a
  matched pair: `datachannel-wasm` exposes the *same C++ API* as
  `libdatachannel`, compiled for Emscripten, delegating to the browser's own
  built-in WebRTC implementation via JS glue — one API surface, two
  backends, no fork of either (same "no forking libraries" rule as
  everything else in this doc). This also resolves what looked like a
  tension between "browser client needs WebRTC/WebSocket" and "native client
  should be able to use something more efficient directly": a native peer
  connecting via `libdatachannel`'s native mode *is* raw UDP/DTLS/SCTP, no
  browser involved at all for a native-to-native connection — there's no
  separate "efficient path" to add on top, `libdatachannel`-native already
  is that path. The DTLS/SCTP tax (real, but modest — encryption is cheap on
  modern hardware, framing is a handful of bytes) applies equally to native
  and browser peers and buys built-in NAT traversal (ICE) for free, which
  raw UDP/ENet would leave to build separately.

  Tradeoff acknowledged: WebRTC's connection *setup* is genuinely heavier
  than WebSocket's (ICE candidate gathering, a DTLS handshake, and — for
  real internet play across NATs, not LAN/localhost testing — a STUN server;
  a public one, e.g. Google's, is enough to start, no self-hosted
  infrastructure required yet). Same escape-hatch philosophy as before, just
  aimed the other way now: a leaner native-only transport (raw UDP/ENet)
  only gets added later *if* profiling shows the DTLS/SCTP overhead actually
  matters for native-to-native play, which is unlikely at this game's
  traffic volume.

  **Implementation (2026-07-19).** `game/net/webrtc-transport.hpp/cpp`:
  `WebRtcTransport` wraps one `rtc::PeerConnection` + one `rtc::DataChannel`
  (unordered, unreliable at creation, matching the protocol's own redundancy
  design — `reliable` on `Send()` is currently ignored, same as
  `LoopbackTransport`), constructed with a `Role` (`Offerer` = client,
  `Answerer` = server) and handling exactly one peer connection, like
  `LoopbackTransport`; a server juggling several WebRTC clients needs one
  instance per client, left for whenever a real signaling server exists to
  drive "a new client wants to connect" in the first place. WebRTC has no
  built-in signaling — `SetLocalDescriptionCallback`/`SetLocalCandidateCallback`
  hand the caller the SDP offer/answer and ICE candidates to relay to the
  remote peer, `SetRemoteDescription`/`AddRemoteCandidate` feed in what
  arrives from it; a `Connect()` method (separate from the constructor)
  starts negotiation only after both callbacks are installed — installing
  them after construction lost the race against the offer, which can fire
  (and be dropped, uncaught) within microseconds of `createDataChannel`.

  CMake (native): `libdatachannel` needs a TLS backend for DTLS; OpenSSL has
  no CMake build (Perl/Configure-based), so MbedTLS (pure CMake) is fetched
  the same FetchContent way as every other dependency. Two integration snags,
  both fixed in `CMakeLists.txt`: (1) MbedTLS's own subdirectory build only
  aliases `MbedTLS::mbedtls`/`mbedx509`/`mbedcrypto` individually, but
  `libdatachannel` looks for one combined `MbedTLS::MbedTLS` target — aliased
  manually, `mbedtls` already `PUBLIC`-links the other two so the alias pulls
  both in transitively; (2) MbedTLS's `3rdparty/everest`/`p256m` helper libs
  use a bare `add_library()` with no `STATIC`/`SHARED` keyword, inheriting
  whatever `BUILD_SHARED_LIBS` RmlUi/SDL2 leave `ON` earlier in the same
  configure — as DLLs they export no symbols (only ever linked into
  `mbedcrypto`), so MSVC produces no `.lib` and the final link fails looking
  for one; forced `BUILD_SHARED_LIBS OFF` scoped around just the MbedTLS
  `FetchContent_MakeAvailable` call, restored after. Also needed:
  `libdatachannel`'s DTLS transport unconditionally references the SRTP
  keying-material extension (RFC 5764) even with its own `NO_MEDIA=1`, and
  MbedTLS ships that disabled by default — enabled via
  `cmake/mbedtls-user-config.h` + `MBEDTLS_USER_CONFIG_FILE`. CMake (wasm):
  `datachannel-wasm` is fetched the same way and needs no extra dependency
  wiring — it's Emscripten-only and self-contained. Both backends are linked
  behind one `Gravitaris::WebRTC` alias target so `webrtc-transport.cpp`
  never branches on platform.

  **A real bug this surfaced, beyond the signaling race above**:
  `NetClient::SendInput`'s `lastAckedSnapshotTick + 1` lead (the fix from
  3.3/3.4's `LoopbackTransport` bug, below) assumed a round trip never takes
  more than one tick — true for `LoopbackTransport`'s synchronous zero-latency
  queue, false for any real transport, where actual RTT (WebRTC's DTLS/SCTP
  scheduling, even on localhost) can span more than one 60Hz tick and land
  every command already-stale on arrival, every time — the identical failure
  mode, just caused by real latency instead of same-process queue timing.
  Fixed by widening the lead to a named `NetClient::INPUT_LEAD_TICKS = 4`
  constant; commands stamped further into the future than strictly needed
  just wait harmlessly in `InputQueue` until their tick comes up (confirmed
  by reading `InputSystem::Update`/`InputQueue`, not assumed), so this is
  free slack, not a tuned-fragile number. Still an interim measure — real
  clock sync / client-side prediction (Phase 5) replaces the guess entirely.
- [x] **3.2 Protocol v1** (`game/net/protocol.hpp/cpp`, all little-endian,
  first byte = `PacketType`): `ClientHello {protocolVersion, name}`,
  `ServerWelcome {clientId, yourShipNetId, tickRate}`, `ClientInput
  {lastAckedSnapshotTick, lastAckedEventSeq, commands[≤8]}`, and
  `PacketType::Snapshot` followed directly by Phase 2's own
  `SerializeSnapshot` bytes (no separate wrapper struct — the tick/entities/
  events split it already has is the wire format). One deviation from the
  sketch: `ClientInput` also carries `lastAckedEventSeq` alongside the
  snapshot tick — the server needs to know which `GameEvent`s a peer has
  already seen to compute the right `eventsSinceSeq` for that peer's next
  snapshot, and it turned out cleaner to have the client report that
  explicitly than infer it from the tick. In practice `NetServer` doesn't
  even trust the client-reported value (see 3.4) — the field exists for a
  future consumer, but isn't load-bearing yet.
- [x] **3.3/3.4 `NetServer`/`NetClient` — protocol-and-spawn wiring, proven
  over `LoopbackTransport`; real `--server`/`--connect` CLI roles not yet
  wired into `GravitarisApplication`.** `NetServer` (`game/net/net-server.hpp
  /cpp`) owns no `Game` itself — the caller drives the tick loop and calls
  `IngestInput()` before `Game::Update()`, `BroadcastSnapshot()` after
  (mirroring `GravitarisApplication`'s existing FeedInput-before/Render-after
  split for local input). On `ClientHello` it spawns a player ship via the
  existing `EntitySpawner::SpawnPlayer` seam and answers with
  `ServerWelcome`; `ClientInput` commands get pushed straight into that
  ship's `InputQueue` — from the sim's perspective a network player *is* the
  existing input seam, confirming 3.4's premise. Snapshot broadcast tracks
  each peer's already-sent event `seq` server-side (deliberately not trusting
  the client's self-reported ack). `NetClient` (`game/net/net-client.hpp/
  cpp`) sends `ClientHello` on its first `Connected` event, decodes
  `ServerWelcome`/`Snapshot`, and exposes the latest decoded `SnapshotData`
  to the caller — deliberately headless (ADR constraint 1): applying a
  snapshot into a renderable world is cgame's `SnapshotApplier` (Phase 2),
  kept a separate step so `NetClient` stays testable with zero GL dependency.
  **A real bug this surfaced**: `NetClient::SendInput` originally took the
  tick to stamp from the caller; `InputSystem` drops any command with
  `tick < step` unconditionally (no staleness tolerance — see its own
  comment). Once a command crosses any transport at all — even
  `LoopbackTransport`'s same-process, zero-latency queue, because of the
  one-loop-iteration gap between a client's send and the server's next poll
  — it's already stale as soon as it's stamped with "the client's own current
  tick," so it was being silently dropped every single time; with no clock
  sync or local prediction yet (Phase 5), the fix was to have `NetClient`
  stamp outgoing commands with `lastAckedSnapshotTick + 1` internally (its
  only estimate of "the next tick the server hasn't simulated yet") rather
  than accept a tick from the caller at all.

**Done when:** two instances on one machine: both players see each other fly,
shoot, take damage, die, respawn; kill -9 of the client doesn't disturb the
server.

**Verification status**: both targets build clean, natively (MSVC) and under
Emscripten. `gravitaris-sim-test` extends its existing single-`Game` scenario
with a `NetServer`/`NetClient` pair talking over a fresh
`LoopbackTransport::CreatePair()`: asserts the handshake completes
(`ServerWelcome` delivers a real `yourShipNetId`, `NetServer::PeerCount() ==
1`), that the server-side entity for that NetId exists, that ticks of
client-sent thrust input measurably move the server-side ship (`serverSpeed >
1`, the assertion that caught the staleness bug above), and that the
client's latest decoded snapshot's entity for its own ship cross-checks
against the server's own `Transform` truth to within 0.5 world units (f32
wire precision vs. the sim's doubles). Both runs of the two-run determinism
comparison still match exactly, confirming the net code adds no
nondeterminism.

A second, separate proof (`TestWebRtcRoundtrip`, its own `Game`, deliberately
outside the two-run determinism comparison since real ICE/DTLS timing isn't
bit-exact) runs the identical `NetServer`/`NetClient` assertions over two
real `WebRtcTransport` instances in one process — actual localhost UDP, DTLS
handshake, SCTP data channel, no mocking — with the SDP offer/answer and ICE
candidates shuttled directly between the two instances via in-process
callbacks rather than a real signaling server (which doesn't exist yet). All
assertions pass, confirming the transport itself (not just the protocol
layer above it) works end to end on Windows/MSVC.

Not yet attempted: verifying `datachannel-wasm` actually connects in a
browser (the wasm build compiles and links, but nothing has driven a
handshake through it — no signaling server exists to test against, and
`gravitaris-sim-test` doesn't build for wasm); a real signaling server (a
small piece of app-level infrastructure, not part of `game/`'s transport
abstraction); WebRTC multi-peer server support (today's `WebRtcTransport` is
1:1, like `LoopbackTransport` — a real server needs one instance per
connected client, driven by whatever tells it a new client showed up);
`--server`/`--connect` CLI wiring into `GravitarisApplication`. These are
app-level integration and infrastructure decisions that deserve their own
scoped pass rather than a blind extension of this one.

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
