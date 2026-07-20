#pragma once

#include <cstdint>

#include <ankerl/unordered_dense.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/net/transport.hpp>

namespace Gravitaris {

// Server side of Protocol v1 (docs/networking-plan.md 3.3/3.4): owns no Game
// itself (the caller does, and drives its tick loop) -- this only wires a
// transport's peers onto that Game's InputQueues and broadcasts snapshots.
// Headless by construction (game/ only), so it's testable in the sim-test
// target with zero rendering/transport-implementation dependency.
//
// Per-tick call order matters: IngestInput() before Game::Update() (so this
// tick's commands are in the InputQueue when InputSystem drains it),
// BroadcastSnapshot() after (so it reflects the state Update() just produced)
// -- the same before/after split GravitarisApplication's FeedInput/Render
// already follows for local input.
class NetServer {
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    const GameEventQueue& m_eventQueue;
    INetTransport& m_transport;

    struct PeerState {
        flecs::entity ship;
        // Server-authoritative: what this peer has already been sent, not
        // what it claims to have acked (ClientInputPacket::lastAckedEventSeq
        // is received but deliberately not trusted for this -- a peer that
        // never acks correctly must not get its event stream wedged).
        std::uint32_t lastSentEventSeq = 0;
        bool welcomed = false;
        // Highest InputCommand::tick already pushed into this peer's
        // InputQueue. The client resends its last CLIENT_INPUT_BACKUP
        // commands with every packet (so a dropped packet doesn't lose a
        // command), so without this the same commands get pushed again every
        // packet and silently evict genuinely-new ones once the 64-slot ring
        // fills up. Commands with tick <= this are duplicates and skipped.
        // Seeded to the welcome tick, so the dead-man timeout below measures
        // from "joined" rather than from tick 0.
        std::uint64_t lastQueuedInputTick = 0;
        // Commands arriving with tick < the server's current tick -- already
        // stale, InputSystem would drop them anyway (see its own comment) --
        // counted here as a health metric, not currently surfaced anywhere.
        std::uint64_t staleInputCount = 0;
        // Dead-man latch: true once the input timeout has injected a
        // synthetic idle command for the current stall episode (see
        // IngestInput); reset by the next genuinely accepted command. One
        // injection per episode is enough -- InputSystem's repeat-last
        // -command then repeats *idle*, the safe steady state.
        bool idleInjected = false;
        // Ticks left until a dead ship (destructed by DeathSystem) respawns;
        // -1 = no respawn pending (ship is alive, or hasn't been noticed dead
        // yet). Set to RESPAWN_DELAY_TICKS the first tick `ship` is found
        // dead; counts down in HandleRespawns.
        int respawnTimer = -1;
    };
    ankerl::unordered_dense::map<PeerId, PeerState> m_peers;

    // Dead-man timeout: if a welcomed peer hasn't landed a fresh command in
    // this many ticks, inject one synthetic all-clear command so repeat-last
    // -command can't keep a stalled client's ship thrusting/spinning forever
    // (a throttled browser tab whose input ticks drifted stale did exactly
    // that -- every command it sent was silently dropped while its last
    // consumed flags kept applying). 250ms: far above real packet-loss
    // gaps at the 60Hz send rate, far below "ship visibly flies away".
    static constexpr std::uint64_t INPUT_TIMEOUT_TICKS = 15;

    // Ticks between a peer's ship dying and a fresh one being spawned for
    // them (matches Game::RESPAWN_DELAY_TICKS, single-player's own respawn
    // delay -- no shared constant since NetServer otherwise has no
    // dependency on Game itself, only flecs::world&/EntitySpawner&).
    static constexpr int RESPAWN_DELAY_TICKS = 90;

    void HandlePacket(PeerId peer, const std::uint8_t* data, std::size_t size, std::uint64_t currentTick);
    void HandleDisconnect(PeerId peer);

    // Spawns a fresh ship (and re-welcomes the peer with its new NetId) for
    // any welcomed peer whose ship isn't alive, after RESPAWN_DELAY_TICKS.
    // Without this, a peer whose ship dies (e.g. crashed into a sun) stays a
    // permanent ghost: HandlePacket already refuses to queue input for a
    // peer with a dead ship, and nothing else ever gives them a new one.
    void HandleRespawns();

public:
    NetServer(flecs::world& registry, EntitySpawner& entitySpawner, const GameEventQueue& eventQueue,
             INetTransport& transport);

    // Polls the transport: completes the ClientHello/ServerWelcome handshake
    // (spawning a player ship per new peer), pushes ClientInput commands into
    // the matching ship's InputQueue, and destroys a peer's ship on
    // Disconnected. Also runs HandleRespawns -- called here (rather than
    // after BroadcastSnapshot) so a fresh ship is queuing input and included
    // in the very next snapshot, not one full tick later. Call once per
    // tick, before Game::Update().
    void IngestInput(std::uint64_t currentTick);

    // Sends a full snapshot (this tick's state + events since what this peer
    // was last sent) to every welcomed peer. Call after Game::Update(); the
    // caller decides the cadence (every tick, or every Nth for a lower
    // snapshot rate than the sim's own tick rate).
    void BroadcastSnapshot(std::uint64_t currentTick);

    [[nodiscard]] std::size_t PeerCount() const { return m_peers.size(); }
};

} // namespace Gravitaris
