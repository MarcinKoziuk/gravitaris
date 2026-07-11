# ADR 0001: Authoritative server + prediction, not deterministic lockstep

Status: proposed (matches the direction sketched in IDEAS.md "Simulation and
Multiplayer"; details below are recommendations to be confirmed)

## Decision

Multiplayer uses an **authoritative server with client-side prediction and
snapshot interpolation** (the quake3 model the module split already mirrors).
We explicitly do NOT pursue deterministic lockstep.

## Why not lockstep

- Chipmunk2D simulates in floats; cross-platform/compiler determinism is not
  guaranteed. Lockstep would mean fighting the physics engine forever (or
  replacing it with fixed-point).
- Lockstep couples every client's frame rate to the slowest peer and makes
  late-join/drop-in hard; the campaign mode's per-hex servers assume
  server-owned state anyway.

## Consequences — constraints on ALL gameplay code

These apply from the first single-player prototype, because retrofitting is
expensive. AI counts as a "client" driven by the same command interface.

1. **The sim (`game/`) is headless.** It must never include rendering, GL,
   window, or `cgame/` headers. `Magnum::Math` is fine. If a sim system needs
   something visual-sounding (e.g. "muzzle position"), it's data on a
   component, not a renderer query.
2. **Components are split into three replication classes:**
   - *Replicated* (server → clients): plain serializable data, no pointers,
     no resource handles. Lives in `include/gravitaris/game/component/`.
   - *Client-only* (presentation: render handles, interpolation buffers,
     prediction state). Lives in `cgame/`.
   - *Server-only* (AI internals, lag-comp history). Lives in `game/` but is
     never serialized to clients.
3. **Never serialize raw entity ids.** ECS ids are process-local. Replicated
   entities carry a `NetId` (server-assigned, stable, u32/u64); each side
   keeps a NetId ↔ entity map. Cross-entity references inside replicated
   components (missile target, projectile owner) store NetId, not entity.
4. **Input is a tick-stamped command struct** (thrust/rotate/fire bits +
   analog values), quake3 `usercmd_t`-style. The sim consumes commands; it
   never reads the keyboard. The current `Controls::actionFlags` is close —
   it becomes the payload of a per-tick command queue.
5. **Stepping is a pure function of (state, commands, dt).** Fixed tick
   (`Game::PHYSICS_DELTA` already exists). No wall-clock reads inside sim
   systems; RNG only via a per-system seeded stream stored in a component or
   singleton, so the server can re-run ticks.
6. **Prediction scope is minimal**: the local player's ship only. Remote
   entities render interpolated ~1-2 snapshots behind. On misprediction the
   client snaps the player body to server state and replays unacked commands.
   Chipmunk's space is stateful, which makes replay awkward — acceptable
   approximation: re-step only the player's body against static gravity
   sources during replay (ships rarely contact-collide during correction
   windows; document artifacts if they show up).
7. **Hitscan needs lag compensation**: the server keeps a short ring buffer
   (~1s) of entity transforms per tick and rewinds hit tests to the shooter's
   acked view tick. Design the transform history storage with this in mind.
8. **Snapshots are delta-compressed** against the last client-acked snapshot,
   per-entity component change masks. First implementation can be naive
   (full snapshots at low rate) — but the component split (point 2) is what
   makes delta compression possible later, so keep it clean now.

## Deliberately deferred

- Transport library (candidates: ENet, Valve GameNetworkingSockets, raw UDP).
  Pick when netplay work actually starts.
- Interest management / per-hex server handoff protocol (entity ownership
  transfer at tile edges). Needs its own ADR before the campaign mode.
- Encryption/auth.

## The cheap test that keeps this honest

Once the slice exists: run `game/` in a headless unit test that steps a
scripted 2-ship fight for N ticks and asserts the end state. If that test
can't be written without linking cgame/Magnum GL, constraint 1 has been
violated somewhere.
