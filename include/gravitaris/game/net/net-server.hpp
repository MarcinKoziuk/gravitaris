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
    };
    ankerl::unordered_dense::map<PeerId, PeerState> m_peers;

    void HandlePacket(PeerId peer, const std::uint8_t* data, std::size_t size);
    void HandleDisconnect(PeerId peer);

public:
    NetServer(flecs::world& registry, EntitySpawner& entitySpawner, const GameEventQueue& eventQueue,
             INetTransport& transport);

    // Polls the transport: completes the ClientHello/ServerWelcome handshake
    // (spawning a player ship per new peer), pushes ClientInput commands into
    // the matching ship's InputQueue, and destroys a peer's ship on
    // Disconnected. Call once per tick, before Game::Update().
    void IngestInput(std::uint64_t currentTick);

    // Sends a full snapshot (this tick's state + events since what this peer
    // was last sent) to every welcomed peer. Call after Game::Update(); the
    // caller decides the cadence (every tick, or every Nth for a lower
    // snapshot rate than the sim's own tick rate).
    void BroadcastSnapshot(std::uint64_t currentTick);

    [[nodiscard]] std::size_t PeerCount() const { return m_peers.size(); }
};

} // namespace Gravitaris
