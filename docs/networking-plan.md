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
| PVS / interest management    | distance-based relevance (Phase 7, deferred)   |

One deliberate difference: quake3 attaches events to entities inside
snapshots; we use a single global, sequence-numbered event stream (simpler to
reason about, trivially delta-able by "events since your last acked seq", and
it works identically offline). Spatial filtering can be added at Phase 7
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

## Phase 3.5 — First playable multiplayer (two browsers, one server)

Goal, stated as the user asked it: **two browser instances connect to one
native Gravitaris server and play together (as team blue)**. This is the
minimum wiring pass that turns Phase 3's proven-but-headless pieces into a
thing two people can actually fly around in. It deliberately reuses what
exists instead of building new subsystems:

- `NetServer` already handles multiple peers (`PeerId`-keyed map) — only the
  *transport* is 1:1 today.
- `SpawnPlayer` already assigns `TeamId::Blue` — "as team blue" costs one
  spawn-offset tweak, nothing more.
- The Phase 2 Mirror path (`SnapshotApplier` + `m_mirrorRenderer2` in
  `cgame.cpp`) **is** the remote-client render path — it just needs to be fed
  by `NetClient::GetLatestSnapshot()` instead of the local sim's
  `WriteSnapshot` round-trip.

Explicitly out of scope (defer, don't gold-plate): interpolation (Phase 4 —
raw 60Hz snapshot application on LAN/localhost is playable, if slightly
steppy), prediction (Phase 5), STUN/TURN (host candidates suffice on
LAN/localhost), disconnect/rejoin UX, lobby/matchmaking, event-driven
audio/fx on the remote client (`SnapshotApplier`'s documented v1 gap — ships
fly and shoot silently on the client this pass if wiring events is any real
effort).

### 3.5.1 Signaling over WebSocket — the game server is its own signaling server

The one genuinely missing piece of infrastructure. A browser can't be
handed SDP through an in-process callback; it needs a network channel that
exists *before* the data channel does. Minimal answer: the native server
listens with `rtc::WebSocketServer` (built into libdatachannel — currently
compiled out by our `NO_WEBSOCKET=ON`; flip it), and clients signal over
`rtc::WebSocket` (datachannel-wasm ships a browser-native wrapper of the
same API unconditionally; libdatachannel provides it natively). No separate
signaling process, no JSON dependency — tiny text frames, e.g.
`desc\n<type>\n<sdp>` / `cand\n<mid>\n<candidate>`. Client is the Offerer
(matches the existing role split). After the data channel opens the
WebSocket can simply be closed. `ws://` (not `wss://`) is fine: browsers
allow plain `ws://` to localhost, and to any host when the page itself is
served over plain `http://` (which it is, via `python -m http.server`).

- [x] Flip `NO_WEBSOCKET` to `OFF` (native CMake block).
- [x] `WebRtcTransport` (client role) grows a
  `ConnectSignaling(const std::string& wsUrl)` path that runs the existing
  seam over a `rtc::WebSocket` instead of manual callbacks. Kept the manual
  seam — the sim-test proof depends on it and it stays the transport's
  unit-test surface. Real bug caught here: registering the local
  -description/candidate callbacks *after* constructing the `PeerConnection`
  loses a real race for an `Offerer` — `createDataChannel` can fire
  `onLocalDescription` within microseconds, before a caller-installed
  handler exists to catch it (plain callback slots, not queues). Fixed by
  splitting construction from negotiation: a new `Connect()` method starts
  it, called only after both callbacks are installed.
- [x] **Risk retired: `webrtc-transport.cpp` compiles clean under
  Emscripten** against datachannel-wasm — no API drift.
- [x] Wire format for signaling frames extracted into
  `game/net/webrtc-signaling.hpp/cpp` (`EncodeDescriptionFrame`/
  `EncodeCandidateFrame`/`DecodeSignalingFrame`), shared by both the client
  and server sides so they can't drift apart.

### 3.5.2 Multi-peer server transport + headless server binary

- [x] `WebRtcServerTransport : INetTransport` (native-only, guarded in
  CMake — browsers can't listen): owns the `rtc::WebSocketServer`, and per
  incoming WebSocket assigns the next `PeerId` and builds a
  `PeerConnection`/`DataChannel` pair (Answerer role, driven manually rather
  than via `ConnectSignaling`, which is the Offerer/client path), emitting
  `Connected`/`Disconnected`/`Packet` tagged with that `PeerId`. `Send(peer,
  ...)` routes to that peer's channel. `NetServer` needed zero changes.
- [x] `gravitaris-server` (`src/server/main.cpp`): the sim-test pattern
  promoted to a long-running process — `FilesystemPhysFS` + `Game` (started
  via `BuildClassicScenario` directly, *not* `Game::Start()`, which would
  spawn an uncontrolled, unreplicated local player) + `NetServer` +
  `WebRtcServerTransport`, wall-clock-paced 60Hz loop, links `game/` only.
  Logs peer connect/welcome via the normal `LOG()` macro.
- [x] Spawn offsets: `NetServer`'s hello handler now spaces players 200
  world units apart on X, keyed off how many peer slots are already taken.

**Gate:** met, but only for **one** client at a time so far — 3.5.1's own
proof (`TestWebRtcSignalingRoundtrip`, below) exercises exactly this stack
(`WebRtcServerTransport` + `gravitaris-server`'s own code path) with a
single real client over real localhost UDP/WebSocket. A *second*
simultaneous peer (`PeerCount() == 2`) was not separately exercised this
pass — worth a follow-up sim-test extension before relying on it.

### 3.5.3 Client mode in the app

- [x] `--connect ws://host:port` (native argv, plain scan — no getopt
  dependency added); wasm reads `?connect=...` from `window.location.search`
  via a small `EM_ASM_PTR` block (`GetConnectUrl` in `gravitaris.cpp`).
  **Real bug caught**: `CORRADE_TARGET_EMSCRIPTEN` isn't defined until
  `Corrade/configure.h` has been transitively included (via the Magnum
  headers) — an `#ifdef CORRADE_TARGET_EMSCRIPTEN` guard placed *before*
  those includes silently evaluates false even under Emscripten, so the
  `#include <emscripten/emscripten.h>` never happened and `EM_ASM_PTR` was
  an undeclared identifier at the call site (the compiler then parsed the
  JS code block as literal, invalid C++). Fixed by moving the guarded
  include after the Magnum includes. **Second bug**: `EM_ASM_PTR`
  stringifies its code argument through the C preprocessor, which splits on
  *any* top-level comma (even one nested only inside JS's own parens, per
  `em_asm.h`'s own doc comment) — `stringToUTF8(value, ptr, bytes)`'s commas
  broke this until the whole code block was wrapped in an extra `(( ... ))`.
- [x] Client mode wiring in `GravitarisApplication`/`CGame`: `CGame::Render()`
  now branches to a separate `RenderNetClient()` when `IsNetClient()` — no
  `CameraDirector::Update()` (it's bound to `m_registry`, not
  `m_mirrorWorld`, so it can't follow a mirror-world entity; the camera hard
  -follows the tracked ship directly via `Camera::SetPosition`/`SetZoom`
  instead — no dead-zone/enemy-framing on a remote client yet, deliberately).
  `tickEvent()` skips the local accumulator/fixed-step loop entirely in this
  mode (nothing to keep in step with real time) and just forwards
  `m_currentInput` to `NetClient::SendInput` once per frame.
  `SnapshotApplier` grew `EntityForNetId()` so the camera can find the
  player's own mirror-world entity.
- [x] Camera: follows via the mechanism above.

### 3.5.4 Browser proof

**Two real bugs found and fixed** getting this far, both build/link-time,
neither visible from the sim-test (native-only, no browser):

1. **datachannel-wasm's `PeerConnection()` no-arg constructor is declared
   but never defined** — a link error (`undefined symbol:
   rtc::PeerConnection::PeerConnection()`) that only appears once something
   in the actually-linked call graph constructs a `WebRtcTransport` (nothing
   did, before this phase). Fixed by always passing an explicit
   `rtc::Configuration{}`, which both backends implement identically.
2. **datachannel-wasm's JS glue (`wasm/js/webrtc.js`) reaches into
   `Module['HEAPU8']`/`HEAPU32` directly** to marshal SDP/candidate strings
   and binary payloads across the JS/wasm boundary — not exported by
   default in this Emscripten version. Silent at build time; at runtime the
   module **aborted permanently** (`Aborted('HEAPU8' was not exported...)`)
   the moment signaling tried to send anything, which looked identical to
   the already-known Chrome/wasm black-screen rendering bug until traced
   via the browser console. Fixed with
   `-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,UTF8ToString,stringToUTF8,lengthBytesUTF8`.

With both fixed: the wasm client boots in Chrome, resolves `?connect=`,
opens the signaling WebSocket, and **the real DataChannel handshake
completes** — confirmed server-side (`gravitaris-server` logs "peer N
connected", meaning that peer's `WebRtcTransport::BindDataChannel`'s
`onOpen` fired, i.e. real DTLS/SCTP established from an actual browser).
This proves the browser/wasm side of 3.1b end to end for the first time.

**What's not verified**: the client never reaches `ServerWelcome` in this
environment, and the game canvas renders a single black frame. Traced to
`document.hidden === true` / `document.visibilityState === "hidden"` in
this specific browser-automation tool — Chrome fully suspends a
`requestAnimationFrame`-driven main loop (which is how Magnum's Emscripten
`Sdl2Application` backend runs `tickEvent`/`drawEvent`) for a
non-foregrounded tab. This reproduces identically with **zero** networking
code involved (a plain `?connect`-less single-player load in a second tab
hangs the same way) — confirming it's an environment property of this
testing tool, not a regression from this phase's changes: WebRTC/WebSocket
connection setup happens on the browser's own networking stack, independent
of the render loop, which is exactly why the handshake gets as far as a
real data channel while nothing C++-side ever runs again to react to it
(`NetClient::Update()`, which sends `ClientHello`, only runs from inside
`tickEvent`/`Render`). **Needs verification in a real foregrounded browser
tab** — expected to work, since everything upstream of the stalled main
loop is now proven.

**Done when (the phase gate):** two browser tabs + one native server; both
players see each other fly and shoot as team blue; closing one tab doesn't
disturb the server or the other client. **Not yet met** — blocked on the
above, plus the untested second-simultaneous-peer path (3.5.2).

## Design direction — client-side simulation (decided 2026-07-19)

This section is the architecture context for Phases 4–6 below; they are its
execution steps, not independent features. Recorded so a future session can
pick it up cold. **Decision: go the client-simulation route** (client runs the
real sim as prediction, server stays authoritative and corrects), rather than
the cheaper "thin mirror" fix. Motivation: bullets are *real traveling
entities* (`ShipControlsSystem` → `EntitySpawner::SpawnBullet`, a physics body
with velocity + lifetime, paced by `Controls::fireCooldown` — not hitscan), so
at high ping aiming is unplayable unless the client both predicts its own ship
and the server compensates for what the client actually saw when it fired.

### Two distinct techniques (do not conflate)

- **Client-side prediction** — makes *your own* ship/aim feel instant. Client
  steps its own ship locally, applies input immediately, reconciles on
  snapshot. In this game aiming *is* rotating your own ship, so prediction is
  most of the "aim feels laggy" fix. → Phase 5.
- **Lag compensation ("unlagged" — Quake 3 / "favor the shooter")** — makes
  your *shots register against what you saw*. Server keeps a per-tick history
  ring; a fire command stamped "I acted on tick T" is resolved by rewinding
  the relevant entities to tick T, testing the hit there, applying the result
  in the present. This is the "confirm the kill the client saw" behaviour.
  Turned out not to be needed for this game's non-targeted bullets — see
  Phase 6's scope note below; kept here as Phase 7 ("Lag compensation for
  hits") for if/when a targeted/homing weapon needs it.

They are complementary: prediction alone leaves your shots resolving against a
server world where the enemy is elsewhere; lag comp alone leaves your ship
feeling laggy to fly. Both are needed.

### The projectile wrinkle (scope honestly)

Classic unlagged/Valve lag comp targets **hitscan**: rewind, raycast, done.
Our bullets **travel for many ticks**, which is the harder, rarer case — you
cannot lag-compensate a projectile's *entire* flight without it being both
exploitable and unfair (shots landing on a target long after it took cover).
Chosen treatment, matching what shipping games do for projectiles:

1. **Predicted cosmetic bullet on the client** — spawned instantly on fire so
   it looks immediate and flies through the world the player sees. Cosmetic,
   never authoritative.
2. **Server spawns the authoritative bullet** when the fire command arrives
   (stamped with the client's predicted tick), and **lag-compensates the spawn
   moment only** — rewind to place the muzzle/initial trajectory relative to
   where the target was on the shooter's screen. The real bullet then travels
   in server time.
3. **Reconcile** — when the authoritative bullet appears in a snapshot, drop or
   align the cosmetic one.

So: near-range / fire-instant kills are confirmable; full "everything on my
screen is authoritative" over 300–500ms of travel is explicitly *not* a goal.
Server stays authoritative always — client sim is prediction, never a claim.

### Why this dissolves camera/minimap challenges 1 & 2

The current mirror world (`SnapshotApplier` into `CGame::m_mirrorWorld`) is a
thin *presentation shadow*: it hand-builds only the components the renderer
needs. That is why making the minimap and camera framing work in MP looked like
two problems — (1) no client `PhysicsSystem` for planet radius, (2) the applier
must reconstruct `Damageable`/`Planet`/star-ness. Under client-sim both vanish:

- the client runs a **real `PhysicsSystem`** → planet radius query just works,
  nothing to replicate;
- entities are built by the **real `EntitySpawner`** → they already carry
  `Damageable`, `Planet`, `Orbit`, `PhysicsRef`, `Renderable`, everything.

The mirror stops being an entity *factory* and becomes a *correction*
mechanism (nudge/snap authoritative transforms + hp onto entities the client
already simulates, keyed by `NetId`). Camera (`CameraDirector::Update`) and
minimap (`MinimapRenderer::Render`) then run against a real sim world,
**identical to single-player** — no world-agnostic parameterization required,
and `CameraDirector::UpdateNetClient` (the interim hard-follow added
2026-07-19) gets deleted -- done, see Phase 5's status below. Making
`MinimapRenderer`/enemy-framing world/`PhysicsSystem`-agnostic (so they'd
also work for entities that stay presentation-only, i.e. everyone but the
own ship) is still a reasonable tidy-up but is no longer load-bearing.

### Prerequisites already in place (why this is feasible)

- **Deterministic fixed-step sim**, proven bit-identical by
  `gravitaris-sim-test`'s checksum across runs. Determinism is usually *the*
  blocker for prediction/replay; it is already verified here.
- **Tick clock + tick-stamped input** — `NetClient::EstimateCurrentServerTick()`
  + `INPUT_LEAD_TICKS` (added 2026-07-19) is exactly the "I fired at tick T"
  basis lag comp needs.
- **Snapshot history** — the server already produces full per-tick snapshots;
  the rewind ring is "keep the last N of them" (ADR constraint 7: ~1s).
- **`NetId`** cross-world identity links a local predicted entity to its
  authoritative counterpart.
- **Replicated events** (`BulletFired`, …) give fire/hit signalling a channel.

### Sequencing (order matters)

1. **Phase 4 — interpolation** for entities you do *not* predict (other
   players' ships: you have no inputs to replay for them, so render them in the
   past and lerp). Substrate everything else assumes.
2. **Phase 5 — predict + reconcile your own ship.** Where "aim feels off at
   high ping" mostly goes away. ADR constraint 6 approximation stands: replay
   pending inputs against gravity + static bodies only, no ship-ship contacts
   during replay, document the artifacts.
3. **Phase 6 — bullets + lag comp**: predicted-cosmetic bullet + authoritative
   server bullet with lag-compensated spawn resolution.

Do *not* attempt a big-bang "client runs everything authoritatively." Two
reconciliation regimes coexist by design: your own ship replays inputs; remote
ships/bullets are interpolated or snapped (no inputs to replay for them).

### Risks / accepted approximations

- Full ship-ship contact reconciliation stays deferred/approximated (ADR 6).
- Projectile lag comp is spawn-moment only, not full-flight (above).
- Server remains sole authority; any client-authoritative hit claim is a cheat
  vector and is out of scope.

### Open questions for the implementing session

- Client sim seeding: run `BuildClassicScenario` client-side and spawn ships as
  players join (server drives join order), reconciling by `NetId` — confirm
  entity-identity mapping when the client's locally-created entity must adopt a
  server `NetId` it hasn't seen yet.
- Correction policy for remote (non-predicted) entities: pure interpolation vs
  let-local-physics-run-and-snap. Interpolation is simpler and recommended.
- Visual smoothing of own-ship correction (blend over ~100ms) vs hard snap.

## Phase 4 — Interpolation

Remote entities render ~100ms behind: per-entity buffer of the last N
snapshot transforms in cgame, render at `serverTick - interpDelay`, lerp
between straddling snapshots (slerp-equivalent shortest-arc for rot).
Tunables in a debug tab (delay, extrapolation cap ~50ms). Player's own ship
still snaps (prediction is Phase 5).

- [x] **`NetClient` buffers snapshot history, not just the latest.**
  `m_snapshotHistory` (bounded, `SNAPSHOT_HISTORY_CAPACITY = 32`, strictly
  tick-ascending) replaces the old single-snapshot overwrite. Guards against
  the data channel's unordered delivery: a decoded snapshot with `tick <=`
  the current back is dropped rather than appended, so a late/reordered
  packet can't roll the buffer (or `m_lastAckedSnapshotTick`) backward.
  `GetSnapshotHistory()` exposes it for the interpolator; `GetLatestSnapshot()`
  keeps its original signature/behavior via a `m_latestSnapshot` member kept
  in sync alongside the buffer (see the dangling-reference bug below for why
  it isn't just derived from `m_snapshotHistory.back()` on demand).
- [x] **`SnapshotInterpolator`** (`cgame/net/snapshot-interpolator.hpp/cpp`):
  `Compute(history, renderTick, exemptNetId, tickRate, params)` turns the
  buffered history into one synthetic `SnapshotData` for `renderTick`,
  reusing the *existing* `SnapshotApplier::Apply()` unchanged — the
  interpolator only ever produces another `SnapshotData`, it doesn't touch
  entity lifecycle itself. Three cases: `renderTick` before the earliest
  buffered snapshot clamps to it (covers "just connected"); at/past the
  newest one extrapolates via velocity, capped at `params.
  extrapolationCapSeconds` (default 50ms, per spec); otherwise finds the
  straddling pair (binary search, history is sorted) and lerps
  position/scale/velocity, shortest-arc-lerps rotation (wraps the raw delta
  into `(-pi, pi]` before scaling by `t` — verified in the sim-test proof
  below, e.g. 170deg->-170deg interpolates through the 180deg wrap, not back
  through 0deg). Presence (entity created/destroyed) follows the *newer*
  straddling snapshot: absent-in-newer is omitted (already gone at
  `renderTick`), present-only-in-newer gets its exact state (freshly
  spawned, nothing to interpolate from). `exemptNetId` (the local player's
  own ship) always gets the latest known state instead of the interpolated
  one, matching "player's own ship still snaps" — lives in `cgame/net/`
  (SnapshotApplier's neighbor) but has zero GL dependency, so it's also
  compiled into `gravitaris-sim-test` for the math proof (see ADR 0001
  constraint 1's own enforcement mechanism: if it ever gains a real
  dependency, that target simply stops linking).
- [x] **Wired into `CGame::RenderNetClient`**: computes `renderTick =
  NetClient::EstimateCurrentServerTick() - interpDelayTicks` (the latter
  already added alongside `EstimateCurrentServerTick()` in Phase 3.5's
  tail-end work), feeds `SnapshotInterpolator::Compute`'s output into the
  existing `SnapshotApplier::Apply()` in place of the raw latest snapshot.
- [x] **Net debug tab** (`src/cgame/ui/debug/net-panel.{hpp,cpp}`): interp
  delay slider (0-300ms, default 100ms), extrapolation cap slider
  (0-150ms, default 50ms), and read-only diagnostics (buffered snapshot
  count, estimated server tick, render tick). Disabled with an explanatory
  message in single-player.
- [x] **Sim-test proof** (`TestSnapshotInterpolation`, no `Game`/transport
  needed — pure `SnapshotData`-in/out math against hand-built history):
  straddled lerp correctness including the shortest-arc rotation case,
  extrapolation-past-newest capping, own-ship exemption snapping to latest
  rather than interpolating, and presence handling for an entity that
  despawns vs. one that spawns between two buffered snapshots.

**A real bug this surfaced**: the first `GetLatestSnapshot()` rewrite
returned `std::optional<SnapshotData>` *by value* (derived fresh from
`m_snapshotHistory.back()` each call) instead of the original `const
std::optional<SnapshotData>&`. Every existing call site did `const
SnapshotData& s = *client.GetLatestSnapshot();` — binding a reference to a
subobject of the return value. That pattern is only safe when the reference
binds *directly* to a temporary; here it binds to what `operator*()` (a
function call) returns a reference *into*, and lifetime extension does not
propagate through that call boundary — the temporary `optional` is
destroyed at the end of the full expression, and `s` dangles immediately
after. This is silent UB, not a crash: reading `s.entities.size()` off
freed memory returned 0 (a plausible, non-crashing value) rather than
faulting, which is exactly why `TestNetRoundtrip` failed with "snapshot
contains no entities" instead of an obvious segfault — and why it only
reproduced with a large-enough entity count/allocation history to actually
get the freed memory reused before the read (small/short-lived tests
happened not to trip it). Fixed by keeping `m_latestSnapshot` as a real
member again (updated alongside the history buffer) rather than trying to
derive a reference-returning API from a container access on demand.

**Deviation from the sketch**: no jitter-injection wrapper (the doc's
suggested "debug-tunable delay queue in LoopbackTransport/ENet wrapper")
was built this pass — "stays playable" is an inherently subjective, by-eye
judgment the sim-test can't assert, and the substantive engineering risk
(the interpolation math itself) is what the sim-test proof above actually
covers. Worth adding later if jitter-under-load ever needs to be reproduced
on demand rather than waited for on a real connection.

**Done when:** at a simulated 20Hz snapshot rate motion looks as smooth as
local play; artificial 100ms +30ms jitter (add a debug-tunable delay queue in
LoopbackTransport/ENet wrapper) stays playable.

**Verification status**: `GravitarisNG` (native + Emscripten) and
`gravitaris-sim-test` all build clean; the interpolation math proof and the
full two-run determinism/net-roundtrip suite all pass. Done from an
unattended session. **Not yet manually verified**: the actual "does it look
smooth" visual gate — needs a real multiplayer session (two clients, one
`gravitaris-server`) with the Net debug tab open, watching a remote ship
under real network jitter at various interp-delay settings. The Phase 3.5
browser-environment blocker (`document.hidden` suspending the wasm main
loop in the automated browser tool) still applies to verifying this
in-browser from an unattended session — a real foregrounded browser tab (or
two native `--connect` clients) is needed for the interactive pass.

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

- [x] **Protocol: `GravitySource` replicated for planets.** `EntityState`
  gains `gravityMass`/`gravityMultiplier` (bumped `SNAPSHOT_VERSION` to 2);
  `GatherSnapshot` populates them from `GravitySource`, `SnapshotApplier`
  emplaces `GravitySource` on Planet-typed mirror entities. Exists so
  prediction can compute gravity from the server's own live planet
  positions instead of running a second, independently-seeded orbit
  simulation client-side — the two would need their phase (where in its
  orbit each planet currently is) synchronized somehow, since the client
  joins long after the server's own orbit sim started; reading the
  already-correct, already-interpolated replicated position sidesteps that
  entirely, at the cost of needing this one extra replicated field.
- [x] **`ClientPrediction`** (`game/net/client-prediction.hpp/cpp`,
  headless): owns the locally-predicted own ship, spawned via the same
  `EntitySpawner::SpawnPlayer` single-player uses — a real Chipmunk body on
  the `"main"_id` space. Since nothing else is ever spawned into a
  net-client's `m_registry`, that body is alone in its space: **"no
  ship-ship contacts during replay" falls out automatically from there
  being nothing else in the space to hit, not from any filtering code.**
  `Step(tick, flags, planets)` applies `ShipControlsSystem::ApplyMovement`
  (extracted from `Update()` — rotation/thrust only, no weapons, shared by
  both so prediction can't drift from the real sim's force/torque
  constants) plus gravity computed manually against `planets` (this
  snapshot's Planet-typed `EntityState`s), then steps `PhysicsSystem`
  normally and records the result in a 180-tick (3s) ring.
  `Reconcile(authoritativeTick, authoritative, planets)` looks up that tick
  in the ring; within `POSITION_EPSILON` (1 world unit) it's a no-op (and
  prunes everything older, no longer needed), otherwise it snaps the real
  body to `authoritative` and replays every predicted tick after it.
  **Accepted approximation beyond the ADR's own one:** the predicted ship
  has no shape to collide with at all during prediction/replay (not even a
  planet), since gravity is read from replicated data rather than a second
  local `PhysicsSystem`-simulated planet — a landing/crash briefly looks
  wrong during prediction and corrects on the next reconciliation once the
  server's real collision response arrives.
- [x] **`NetClient::SendInput` takes an explicit tick** now, instead of
  deriving `EstimateCurrentServerTick() + INPUT_LEAD_TICKS` internally: the
  client's own prediction ring and the wire-stamped tick must use the exact
  same number for `Reconcile` to find a match, and re-deriving a fresh
  (wall-clock, jittery) estimate on every `SendInput` call could silently
  desync the two. `CGame` now owns a simple local counter
  (`m_nextPredictedTick`), seeded once from the estimate when the own ship
  first spawns, incrementing by exactly one per predicted tick after that —
  deliberately *not* re-querying the noisy wall-clock estimate every tick.
- [x] **Wired into `CGame`**: `GravitarisApplication::tickEvent()`'s
  net-client branch grew back a fixed-step accumulator loop (mirroring
  single-player's), calling the new `CGame::TickNetClient(flags)` once per
  `PHYSICS_DELTA` — it spawns the own ship the first time a snapshot
  confirms where it should appear (avoids popping in from the origin),
  predicts one tick, and sends input. `RenderNetClient` reconciles once per
  newly arrived snapshot (`ReconcileOwnShipIfNeeded`, tracking
  `m_lastReconciledTick` so the same snapshot isn't reprocessed) and blends
  the correction via a **camera-space offset** rather than touching the
  real simulated `Transform` (which must stay exactly correct for the next
  predicted tick) — decays exponentially over `CORRECTION_SMOOTH_SECONDS`
  (0.1s). `SnapshotInterpolator` now *omits* the exempt (own) NetId
  entirely instead of including it at latest-known position, since it has
  a real rendering source now and showing both would be two competing
  ships on screen. Camera/minimap switched from the old `UpdateNetClient`
  hard-follow (now deleted, dead code) to the *real* single-player
  `CameraDirector::Update`/`RenderMinimap` against `m_registry` — dead-zone
  follow and dynamic zoom now work identically to single-player; enemy/
  planet auto-framing still won't engage, since only the own ship (not
  remote entities) is ever real `m_registry` state this phase.

**Known gaps, explicitly out of scope this pass** (own ship *movement*
prediction was the ask; these are natural next steps, not oversights):
firing is not predicted locally at all (no cosmetic bullet, no immediate
shot sound) — the doc's own "Done when" gate mentions this, but predicting
a bullet client-side needs a client-assigned NetId reconciled against the
server's once the real one arrives, which is exactly Phase 6's "predicted
-cosmetic + lag-compensated-spawn" design, not a small add-on here; no
damage/hit-flash feedback on the locally predicted ship (`HitFlashSystem`
isn't wired into the net-client path — it would work today if it were,
since `m_registry` has a real entity now, but nothing currently damages it
locally since `DamageSystem` doesn't run there either); autopilot isn't
wired into net-client mode (also would likely work now that there's a real
`m_registry` entity — untried).

**Real bugs found from actual playtesting (2026-07-19), fixed same-day:**

1. **Thrust exhaust never showed.** `ClientPrediction::Step`/`Reconcile`
   applied `flags` to the Chipmunk body via `ShipControlsSystem::
   ApplyMovement` but never wrote them to the ship's own `Controls`
   component — `ModelRenderer2`'s `_thrust` tag group gates on `Controls::
   actionFlags.thrustForward`, which stayed permanently false. Fixed: both
   `Step` and each replayed step in `Reconcile` now set
   `m_ownShip.get_mut<Controls>().actionFlags` too.
2. **Camera "smooth jitter", worst while accelerating/decelerating.**
   `POSITION_EPSILON` (1.0 world units) was far tighter than real ship
   speeds warrant — ordinary f32-wire/quantization noise routinely exceeded
   it, triggering a reconciliation correction on nearly every snapshot;
   each one nudges the visual-correction camera offset (see Phase 5's own
   wiring notes above), and frequent small nudges read as jitter. Fixed:
   raised the default to 8.0 and made it a runtime-tunable
   (`ClientPrediction::{Get,Set}PositionEpsilon`, `CGame::{Get,Set}
   PredictionEpsilon`, exposed as "Reconcile epsilon" in the Net debug tab)
   rather than re-guessing a fixed constant — the right value depends on
   real ship speeds/network conditions this session couldn't observe
   directly.
3. **Minimap (and, by the same cause, other players/enemy+planet camera
   framing) invisible in multiplayer.** `MinimapRenderer::Render()` and
   `CameraDirector`'s enemy/planet framing both queried `m_registry`
   directly — never told to look anywhere else, and only the own ship is
   real `m_registry` state in net-client mode (everyone/everything else
   lives in the presentation-only mirror world). **Fixed same day
   (2026-07-19, follow-up session)**:
   - `Planet` promoted from an empty tag to a real component holding the
     body's true collision radius (`float radius`, world units before
     `Transform::scale`). Populated once at creation from the same `Body`
     resource by modelId both in the real sim (`EntitySpawner::
     SpawnCelestial`) and in `SnapshotApplier` for mirror-world planets —
     no live `PhysicsSystem`/`PhysicsRef` needed to read it, which is what
     let both consumers stop depending on `PhysicsSystem` entirely.
   - `SnapshotApplier` also now reconstructs `Damageable` (Ship-typed
     entities; `hp` was already on the wire) and a presence-only `Orbit`
     (bumped `SNAPSHOT_VERSION` to 3 for one new bit, `EntityState::isStar`
     — a sun has none) purely so the existing `!entity.has<Orbit>()`
     star-vs-planet check keeps working unmodified in the mirror world.
   - `CameraDirector::Update()` and `MinimapRenderer::Render()` both gained
     an optional `flecs::world* remoteWorld` parameter: every per-entity
     query (enemy search, planet-framing sweep, minimap's planet-ring and
     ship-dot loops) now runs once against the primary world and, if set,
     again against `remoteWorld` via the same shared lambda — one code
     path, not a duplicated multiplayer variant. `CGame` passes
     `&m_mirrorWorld` from `RenderNetClient`/`RenderMinimap`, `nullptr`
     (i.e. unchanged behavior) from single-player's `Render()`.
   - **Cross-world entity-identity bug caught before it shipped**:
     `CameraDirector`'s sticky enemy-framing (`m_framedEnemy`) compares
     `flecs::entity` handles with `==`/`!=` — but `flecs::entity`'s only
     comparison is via its implicit `operator id_t()`, which exposes just
     the raw 64-bit id, not which `flecs::world` it came from. Two
     *different* worlds (`m_registry`, `m_mirrorWorld`) assign ids
     independently, so a same-numeric-id collision across them would have
     silently mis-identified an unrelated entity as the framed one. Fixed
     with `CameraDirector::SameEntity(a, b)` (raw id equality *and*
     `a.world().c_ptr() == b.world().c_ptr()`), used everywhere
     `m_framedEnemy` is compared.

4. **Two browser tabs: one tab's ship desyncs after ~5s and spins/freezes
   server-side forever, while that tab still feels fully controllable (no
   errors anywhere).** Root cause: `CGame::m_nextPredictedTick` is seeded
   once and then free-runs, +1 per *executed* tick — but the browser
   doesn't guarantee ticks execute. rAF throttling on a backgrounded tab
   (switching between two tabs guarantees one is always throttled), GC
   hitches, and `tickEvent`'s net-client step cap (which *discarded* the
   excess backlog: `m_frameTimeAccumulator = 0.0` at `MAX_STEPS_PER_FRAME`)
   all lose wall-clock time the counter never sees — permanent backward
   drift vs. the server's wall-clock-paced tick. Once total drift exceeds
   `INPUT_LEAD_TICKS` (2 ticks = 33ms!), every `ClientInput` from that tab
   is stamped in the server's past: `InputSystem` drops it as stale,
   repeat-last-command latches the last consumed flags (mid-rotation =
   spins forever; idle = frozen), and the affected tab never notices — its
   own ship is locally predicted (responsive), and reconciliation quietly
   stops matching (the snapshot tick no longer lines up with the drifted
   prediction ring), so nothing ever corrects it. The *other* tab renders
   the server truth: a spinning/frozen ship. Fixed two ways, same day:
   - **Client tick resync** (`CGame::TickNetClient`): each tick, compare
     `m_nextPredictedTick` against `EstimateCurrentServerTick() +
     INPUT_LEAD_TICKS`; drift beyond 5 ticks (~83ms, above the estimate's
     own jitter) re-seeds the counter (logged, with the old/new values).
     Small drift is still left alone so consecutive predicted ticks stay
     exactly one `PHYSICS_DELTA` apart (the property Phase 5 introduced
     the free-running counter for).
   - **Server dead-man switch** (`NetServer::INPUT_TIMEOUT_TICKS = 15`,
     250ms): a welcomed peer that hasn't landed a fresh command for that
     long gets one synthetic idle command injected into its `InputQueue`
     (once per stall episode, `PeerState::idleInjected`), so
     repeat-last-command settles on "hands off the controls" instead of
     replaying held thrust/rotate forever — defense in depth for *any*
     future input stall, not just this one. `lastQueuedInputTick` is now
     seeded to the welcome tick so the timeout measures from "joined".
     Proven in sim-test: `TestNetRoundtrip` now goes silent for 30 ticks
     after sustained thrust and asserts the server zeroed
     `Controls::actionFlags.thrustForward`.
   (The `[si_destination_compare] send failed` lines in the server log are
   macOS system-log noise from the ICE/mDNS stack inside libdatachannel —
   harmless, unrelated.)

**Verification status**: all four targets (native `GravitarisNG`,
Emscripten `GravitarisNG`, `gravitaris-sim-test`, `gravitaris-server`) build
clean. `TestClientPrediction` (sim-test) exercises `ClientPrediction`
against a real headless `Game`'s `PhysicsSystem`/`EntitySpawner` (not just
hand-computed math, unlike the Phase 4 interpolation proof) — gravity from
a replicated planet measurably pulls the predicted ship, a close
authoritative match triggers no correction, a large divergence snaps +
replays and lands near the corrected path, and re-reconciling an
already-replayed tick correctly finds nothing. The full two-run determinism
suite and all earlier net/WebRTC proofs still pass unchanged. Done from an
unattended session. **Not yet manually verified**: the actual "does my own
ship feel local" and "camera feels like single-player" gates — both are
inherently by-feel judgments that need a real multiplayer session (two
clients, one `gravitaris-server`) with actual network latency, not
something the sim-test can assert. Also not exercised: reconciliation
under real (non-zero, jittery) RTT specifically — the sim-test proof drives
`Step`/`Reconcile` directly with synthetic data, not through a live
`NetClient`/transport round-trip the way Phase 3's `TestWebRtcRoundtrip`
did for the protocol layer.

**Follow-up (2026-07-19): minimap/camera-framing fix verification.** All
four targets (native `GravitarisNG`, Emscripten `GravitarisNG`,
`gravitaris-sim-test`, `gravitaris-server`) rebuilt clean after the item-3
fix above; the full sim-test suite (including `TestClientPrediction` and
`TestSnapshotInterpolation`, both of which build `SnapshotData`/`EntityState`
values directly) passes unchanged, checksum-identical to before the fix —
expected, since `Planet::radius`/`isStar` aren't part of
`Game::ComputeStateChecksum()`'s hashed fields. **Not yet manually
verified**: the actual "does the minimap show remote ships/planets" and
"does enemy/planet camera framing engage for a remote target" visual gates
— needs a real multiplayer session (two clients, one `gravitaris-server`),
same caveat as the rest of Phase 4/5's by-feel gates above.

## Phase 6 — Bullets: predicted-cosmetic bullet + immediate feedback

- [x] `ShipControlsSystem::ComputeBulletSpawn` extracted (was file-local
  `GetBulletSpawnPosAndVel`), `BULLET_LIFETIME_SECONDS` made public — shared
  by the real sim and `ClientPrediction` the same way `ApplyMovement` already
  is, so the cosmetic bullet's muzzle math matches the server's exactly.
- [x] `ClientPrediction::Step` mirrors `ShipControlsSystem::Update`'s own
  fire-cooldown/cadence logic (`FIRE_COOLDOWN_TICKS`) for `firePrimary`, and
  on firing spawns a sensor bullet directly into the registry via the same
  `EntitySpawner::SpawnBullet` + `Bullet` component the real sim uses —
  expired the same way, by the caller's own `BulletLifetimeSystem`.
- [x] `ClientPrediction` gained a `GameEventQueue&` constructor param and
  emits a local `BulletFired` event on fire, purely so `AudioSystem`'s
  existing event-driven one-shot path plays the laser sound — no new audio
  code. This event is never serialized; it only exists in this client's own
  in-process queue.
- [x] `CGame::TickNetClient` now runs `m_bulletLifetimeSystem.Update(...)`
  each predicted tick, and `RenderNetClient` now calls `m_audioSystem.Update
  (camera position)` each frame — a side effect of the latter: the
  previously-silent thruster loop in net-client mode (same root cause as the
  Phase 5 thrust-exhaust visual bug, just the audio half of it) now works too.

**Scope note — no server-side spawn-moment rewind was implemented.** The
Design Direction section above frames Phase 6 as needing lag-compensated
spawn resolution because a targeted/homing weapon's hit relevance would
depend on exactly where the target was when the shooter's client saw it fire.
This game's bullets aren't targeted: `ComputeBulletSpawn` only ever reads the
shooter's own transform, and Phase 3's tick-stamped `ClientInput`/`InputQueue`
already guarantees the server spawns the real bullet from the shooter's
correct position for that input tick. So for this weapon model, "lag
compensating the spawn moment" reduces to "process input on the tick it
claims," which was already true — there was no additional rewind logic to
add. If a targeted/homing weapon is ever added, this note is the first thing
to revisit.

**Known gaps, explicitly out of scope this pass**: `fireSecondary` (the box
spawn) is not predicted, only `firePrimary`; the cosmetic bullet is never
explicitly matched against or dropped in favor of the authoritative
replicated one that arrives later — it simply expires on its own short local
timer, so a brief visual overlap between the two is an accepted
simplification rather than a bug.

**Verification status**: all four targets build clean.
`TestClientPrediction` (sim-test) extended to cover firing: `firePrimary`
spawns exactly one cosmetic bullet and emits exactly one local event, the
cooldown gates cadence (no second bullet until `FIRE_COOLDOWN_TICKS` have
elapsed), and holding the trigger past the cooldown fires again — the same
7-tick cadence `ShipControlsSystem::Update` uses. Two-run determinism suite
unaffected. **Not yet manually verified**: whether the laser sound and
cosmetic tracer actually read as "immediate" over real network latency in an
actual multiplayer session — by-feel judgment, not something sim-test proves.

## Phase 7 — Client-side collision geometry for prediction

Found from playtesting (2026-07-19): two symptoms traced to the same root
cause. (1) Two Blue (friendly) ships bumping into each other makes the camera
shake. (2) Landing on an orbiting planet and standing still instead shows a
stairstep jitter synced to the planet's motion.

Root cause: `ClientPrediction::Step`'s Chipmunk space contains *only* the
locally-predicted own ship. Gravity was applied as a manual force computed
from replicated planet positions (`ClientPrediction::ApplyGravity`) — there
was no planet *shape* to rest on, and no other ship's hull to bounce off, in
that space at all. So whenever the server resolves a contact the client
can't see (bouncing off a planet or another ship), prediction diverges hard
every tick and `Reconcile` snaps + replays every single snapshot during the
whole contact episode — each snap dumps into the visual-correction offset
faster than its ~100ms decay bleeds off. This is exactly the approximation
`ClientPrediction`'s own header comment and ADR constraint 6 already flag
("no shape to collide with at all during prediction/replay, not even a
planet" / "no ship-ship contacts during replay").

- [x] **Planet collision proxies** (`ClientPrediction::SyncPlanetProxies`):
  for each Planet-typed `EntityState` in the current snapshot, lazily spawn
  a minimal client-only entity in the *same* Chipmunk space as the own ship
  (`Transform` + `RigidBodyDesc("main"_id, body)`, `body` loaded by the
  already-replicated `modelId` — the same `Body` resource the real sim uses,
  so the kinematic-ness and collision shape come along for free) plus
  `GravitySource` when the planet has one (mass/multiplier already on the
  wire since Phase 5). Every call drives it with
  `PhysicsSystem::SetKinematicMotion(ref, pos, vel)` from the snapshot's
  replicated position/velocity, same mechanism `OrbitSystem` uses for the
  real sim's orbiting planets — just sourced from replicated data instead of
  recomputed orbit math. No `Renderable` (the mirror world already draws the
  visual planet — this is collision geometry only, and would double-render
  otherwise) and deliberately no `Planet` component either (camera/minimap
  already see the real one via `m_mirrorWorld`; adding it here would make
  both sweeps count the same planet twice).
- [x] **`ClientPrediction::ApplyGravity`'s manual force hack deleted.**
  `PhysicsSystem::Simulate` already runs its own per-space `ApplyGravity`
  every tick, querying `m_registry` for `(GravitySource, PhysicsRef)` sources
  and pulling every dynamic body in that space — once the proxies above
  register as real `GravitySource`-bearing bodies in the ship's own space,
  that existing machinery just works, with zero special-casing. This also
  means gravity and collision are now resolved by the exact same code path
  single-player uses, not a parallel hand-rolled one.
- [x] Proxies are synced (created/updated/pruned) once per `Step` and once
  per `Reconcile` call (a `Reconcile` can run from a differently-timed point
  in the frame than the `Step` that preceded it, against a possibly-newer
  snapshot's planet list) — idempotent, keyed by the planet's `NetId`.

**Ship-ship contact is *not* fixed by this** (deliberately out of scope,
matches the ADR's own call): predicting a bounce off another player would
need a kinematic proxy for that ship too, positioned from its own
interpolated (i.e., ~100ms-in-the-past) remote state — the contact timing
would never quite match the server's, and it's the exact case ADR 0001
constraint 6 already says to approximate rather than chase. Two friendly
ships colliding still produces a reconciliation-thrash camera shake; a
future pass could either build the same kinematic-proxy machinery for
remote ships (more correct, more complexity/edge cases) or just detect a
sustained-correction episode and clamp/ease the visual-correction offset
harder so the camera glides instead of shaking (cheaper, doesn't pretend to
predict the bounce).

**Done when:** landing on an orbiting planet and standing still actually
looks still (no stairstep jitter) at a simulated 20Hz snapshot rate.

**Verification status**: all four targets (native `GravitarisNG`, Emscripten
`GravitarisNG`, `gravitaris-sim-test`, `gravitaris-server`) build clean.
`TestClientPrediction` extended with a dedicated collision proof: a real
planet body (`models/planets/simple`) placed exactly at the predicted
ship's own current position with zero gravitational mass (isolating the
effect from gravity) measurably pushes the ship away next tick — proof the
proxy has real collision shape, not just gravitational pull. The existing
gravity/reconciliation/bullet assertions in the same test still pass
unchanged (now routed through `PhysicsSystem::Simulate`'s own per-space
gravity instead of the deleted manual force, same underlying formula). Full
two-run determinism suite unchanged (identical checksum to the previous
session). **Not yet manually verified**: the actual "does landing feel
still" and "does bumping a friendly ship still shake the camera as
expected (unfixed)" by-feel gates — needs a real multiplayer session.

**Follow-up (2026-07-19, same day): planets exempted from interpolation
delay.** Playtesting surfaced two remaining symptoms this session's proxy
fix didn't cover, both traced to one further cause. (1) A landed ship
looked sunk ~10px into the surface, scaling with planet speed. (2) Camera
zoom pulsed quickly (smoothly, but fast) specifically while approaching a
planet, not during other motion. Root cause:
`ClientPrediction::SyncPlanetProxies` collides the ship against a planet
proxy positioned from the *latest raw* snapshot (undelayed), but the
*visual* planet was drawn via `SnapshotInterpolator`, which deliberately
renders ~100ms behind (Phase 4's smoothing delay) — for an orbiting planet
those two positions differ by however far it moved in that gap, which is
exactly the sinking. The same live-vs-delayed mismatch fed
`CameraDirector`'s planet-framing `nearestSurfaceDist` calculation, and
every mode switch between the interpolator's lerp-between-snapshots and
extrapolate-past-newest paths as fresh snapshots arrived nudged that
distance slightly — a fast, repeating perturbation into the zoom-fit math.

Fix: `SnapshotInterpolator::Compute` now always uses the newest known raw
state for `NetEntityType::Planet` entities specifically, the same
"exempt from delay" treatment the local player's own ship already gets —
never lerped, never extrapolated. Justified because planets move on a
perfectly smooth, deterministic circular orbit; unlike input-driven ships,
there's no snapshot-rate jerkiness to hide, so the delay was pure downside
here. This makes physics and visuals agree (both now read the same latest
snapshot), which removes the sinking, and removes the interpolation-mode
-switch noise that was perturbing camera framing. Proven in sim-test
(`TestSnapshotInterpolation`): a planet moving between two snapshots
renders at the *newer* snapshot's exact position at a render tick straddled
between them, not the halfway lerped point a Ship-typed entity would get.

**Accepted trade-off**: since planets now update only when a new snapshot
actually arrives (no continuous lerp/extrapolation smoothing anymore),
their rendered motion could show a small per-snapshot step rather than
buttery continuous motion — expected to be imperceptible given how slowly
orbits move relative to snapshot rate, but worth eyeballing. An easy
follow-up if it ever is noticeable: extrapolate planets forward from the
latest snapshot using their replicated velocity, uncapped (unlike the
existing capped extrapolation for everyone else — safe here specifically
because orbital motion is smooth/predictable, so guessing further ahead
doesn't accumulate visible error the way guessing a ship's next move
would).

**Verification status**: all four targets build clean; full sim-test suite
(including the new planet-interpolation proof) passes, determinism
checksum unchanged. **Manually verified (2026-07-19)**: planets look right
now — no visible sinking, and the per-snapshot step called out in the
accepted trade-off above wasn't noticeable in practice. The uncapped
-extrapolation follow-up stays a documented option, not something currently
needed.

## Follow-up (2026-07-19, same day): local-ship reconciliation was jittering camera *framing*, not zoom

After the planet fix above, zoom pulsing near planets was confirmed gone,
but camera *position* framing (the dead-zone rectangle) was still jittering
during ordinary movement — a fast, smooth-but-quick oscillation, distinct
from the zoom issue and *not* explained by lag in synced enemy/planet data
(which is fine, per the phases above — genuinely just delayed, not jittery).
Correctly suspected as a separate architectural issue rather than "more of
the same," and it was: purely local, nothing to do with how remote entities
are synced.

Root cause, in `CGame::RenderNetClient`: `ReconcileOwnShipIfNeeded()` can
hard-snap the real predicted ship's Transform (`ClientPrediction::Reconcile`
calls `cpBodySetPosition`/`cpBodySetVelocity` directly) whenever prediction
and the authoritative snapshot diverge past epsilon — this is necessary and
correct, replay must continue from the corrected state. The very next call,
`CameraDirector::Update(GetPlayer(), ...)`, read that (possibly
just-snapped) Transform straight from the ECS as its "where is the player"
input — feeding the raw, discontinuous jump directly into dead-zone follow,
enemy-framing distance, and planet-framing distance, all *before* any
smoothing. The existing `m_visualCorrectionOffset` mechanism (Phase 5) was
built to hide exactly this kind of snap, but it was applied *afterward*, by
nudging the camera's already-computed output position — by which point
dead-zone follow had already reacted to the raw jump if it exceeded the
dead zone's slack. Two uncoordinated mechanisms fighting the same
correction, on top of an input that was discontinuous in the first place:
architecturally wrong ordering, not a data/lag problem.

Fix: `CameraDirector::Update` gains an optional `positionOverride`
parameter, used instead of reading `Transform::pos` when set (single-player
never sets it — unchanged behavior there). `RenderNetClient` now decays
`m_visualCorrectionOffset` *before* calling `Update`, adds it to the real
position to produce an already-smoothed position, and passes *that* in —
so dead-zone follow, enemy framing, and planet framing all see a
continuous, eased signal from the start instead of reacting to a snap and
then getting corrected after the fact. The old post-hoc
`camera.SetPosition(camera.GetPosition() - offset)` step is gone entirely;
smoothing now happens once, at the source, for everything that consumes
"where is the player" — not as a bolt-on to just the camera's final
position.

**Verification status**: all four targets build clean; full sim-test suite
passes unchanged (this path isn't exercised by sim-test — no headless
CameraDirector — so no new proof here; it's cgame-only, GL-dependent
code). **Not yet manually verified**: needs a real multiplayer session to
confirm the rectangle-framing jitter is actually gone.

**Correction (2026-07-19, same playtest): not a camera bug.** Reproducing
further, the symptom persisted even while the camera frame itself wasn't
moving — the jitter is the *ship* teleporting 10-50 world units, which
reconciliation (`ClientPrediction::Reconcile`, called from
`ReconcileOwnShipIfNeeded`) can legitimately do: it hard-snaps the real
Transform whenever prediction and the authoritative snapshot diverge past
`m_positionEpsilon`, by design, so replay continues from the correct state.
The position-framing-smoothing fix above is very likely still worth
keeping (it closes a real, separate architectural gap — camera framing
should never consume a mid-frame snap raw, regardless of how often
corrections happen), but it doesn't explain 10-50-unit teleports; those are
either large, infrequent corrections (plausible always, at that
magnitude), or a symptom of something making corrections both large *and*
frequent.

**Leading hypothesis, not yet confirmed**: `NetClient::INPUT_LEAD_TICKS =
2` (33ms) may be too tight for real RTT/jitter. `InputSystem` drops any
command whose tick has already passed with zero tolerance and repeats the
last-consumed flags instead (quake3-style) — if lead time is insufficient,
a chunk of input arrives already stale, gets silently discarded
server-side, and the server keeps simulating the *previous* command while
the client predicts the *new* one; once a snapshot reveals that gap,
reconciliation has real ground to cover, and the correction looks abrupt
because it is one. This is a hypothesis, not a confirmed cause — added
logging instead of guess-fixing it:
- `CGame::ReconcileOwnShipIfNeeded` now logs (`LOG(trace)`) every
  correction's tick and magnitude.
- `NetServer::HandlePacket` now logs (`LOG(trace)`) every command that
  arrives already stale, with exactly how many ticks late it was.
- Both intentionally at the lowest severity (`trace`) — this codebase's
  logger has no level filter (every `LOG()` call always prints, see
  `src/game/logging.cpp`), so these are always-on and meant to be
  correlated by eye during a real session, not gated behind a flag.

Next step once real numbers are in hand: if stale-input lateness clusters
near or above `INPUT_LEAD_TICKS`, that confirms the hypothesis and the fix
is to grow the lead (or make it adaptive to observed RTT/jitter rather than
a fixed constant). If corrections are large but stale-input logging stays
quiet, the cause is elsewhere (e.g. genuine physics divergence between
client and server Chipmunk state) and needs a different investigation.

**Real fix found (2026-07-19, same day): the ship model itself was never
smoothed, only the camera.** Playtesting with the logging above still in
progress, but the actual gap was found by inspection first:
`ModelRenderer2::Render(double)`'s parameter is unused — this renderer does
no prevPos/pos interpolation of its own at all (unlike a typical fixed-tick
renderer), it draws `Transform::pos` directly. The camera-framing fix
earlier this session gave `CameraDirector` a smoothed position, but the
*ship model* was still drawn from the raw, just-corrected Transform — so a
reconciliation snap was always going to look like the ship itself
teleporting, independent of anything the camera does, and independent of
whether corrections are caused by the `INPUT_LEAD_TICKS` hypothesis above
or ordinary prediction noise. This is very likely most or all of the
originally-reported "ship teleports 10-50 units."

One more thing worth being precise about: there is no teleport-hack /
client-authority concern here to fix in the first place. `NetClient::
SendInput` only ever sends control *flags* (thrust/rotate/fire); the ship's
position is never client-reported, it's fully server-simulated from those
flags using the server's own `PhysicsSystem`. A margin already exists for
exactly the "don't correct trivial prediction noise" purpose --
`ClientPrediction::m_positionEpsilon` -- widening it further would trade
*visible occasional snaps* for *invisible unbounded drift* (nothing would
ever pull a small-enough divergence back), which is worse, not a fix, and
still wouldn't touch the render-side gap above.

Fix: `RenderNetClient` now draws the own ship at the same smoothed position
already computed for the camera (`smoothedPlayerPos`) instead of the raw
Transform -- save the real `Transform::pos`, overwrite it for just this one
`ModelRenderer2::Render` call, restore it immediately after. The actual
simulated state the next predicted tick builds on is never touched, only
one frame's worth of what gets drawn. Safe specifically because the own
ship is the *only* renderable entity ever in `m_registry` in net-client
mode (Phase 7's planet proxies deliberately carry no `Renderable`), so
there's no risk of this transient mutation leaking into any other entity's
render this frame.

**Verification status**: all four targets build clean; full sim-test suite
passes unchanged (this is a `cgame`-only render-path change, not exercised
by the headless suite). **Not yet manually verified**: needs a real
multiplayer session — both to confirm the ship no longer visibly teleports,
and to check whether the `INPUT_LEAD_TICKS`-lateness logging above shows
anything now that the dominant *visible* symptom should be gone regardless
of its outcome.

## Phase 8 — Deferred (needs its own design pass when reached)

- Delta-compressed snapshots (per-entity change masks vs last acked).
- Relevance/interest management (distance culling per client).
- Lag compensation for hits (ADR constraint 7: ~1s transform history ring,
  rewind DamageSystem's swept segment query to the shooter's view tick) — for
  a future targeted/homing weapon; today's ballistic bullets don't need this
  (see Phase 6's scope note).
- Clock sync polish, connection quality HUD, host migration, encryption/auth.
- Ship-ship contact prediction (kinematic proxies for remote ships), or the
  cheaper camera-shake mitigation, for the reconciliation-thrash case Phase 7
  explicitly left unfixed.

---

## Invariants checklist (apply to EVERY gameplay PR from now on)

- No wall-clock, no `std::rand`, no iteration-order dependence in `game/`.
- New gameplay one-shots → `Game::EmitEvent`, never a cgame-side observer.
- New components declare their replication class (replicated / client-only /
  server-only) in a comment, and replicated ones stay POD + NetId-referencing.
- `gravitaris-sim-test` still builds (headless) and its checksum is stable
  across two runs.
