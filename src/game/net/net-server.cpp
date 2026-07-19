#include <gravitaris/game/id.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/protocol.hpp>
#include <gravitaris/game/net/snapshot.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/net/net-server.hpp>

namespace Gravitaris {

NetServer::NetServer(flecs::world& registry, EntitySpawner& entitySpawner, const GameEventQueue& eventQueue,
                     INetTransport& transport)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
        , m_transport(transport)
{}

void NetServer::IngestInput(std::uint64_t currentTick)
{
    for (NetEvent& event : m_transport.Poll()) {
        switch (event.type) {
            case NetEventType::Connected:
                m_peers.emplace(event.peer, PeerState{});
                LOG(info) << "net: peer " << event.peer << " connected (" << m_peers.size() << " total)";
                break;
            case NetEventType::Disconnected:
                HandleDisconnect(event.peer);
                break;
            case NetEventType::Packet:
                HandlePacket(event.peer, event.data.data(), event.data.size());
                break;
        }
    }
    (void)currentTick; // reserved: server-side input-tick validation, not needed yet
}

void NetServer::HandlePacket(PeerId peer, const std::uint8_t* data, std::size_t size)
{
    ByteReader reader(data, size);
    const auto type = static_cast<PacketType>(reader.ReadU8());

    switch (type) {
        case PacketType::ClientHello: {
            ClientHelloPacket hello;
            if (!ReadClientHelloBody(reader, hello)) return;
            if (hello.protocolVersion != PROTOCOL_VERSION) return; // silently drop, no version-mismatch UX yet

            auto it = m_peers.find(peer);
            if (it == m_peers.end()) return; // Packet before Connected shouldn't happen; be defensive
            if (it->second.welcomed) return; // duplicate hello (resent, lost welcome) -- ignore

            // Placeholder spawn point/model until Phase 3 grows real player
            // -slot selection; matches Game::Start()'s single local player.
            // Offset by however many peers already hold a slot (this one
            // included, since Connected already inserted it) so players
            // don't stack on top of each other.
            const id_t playerModel = "models/ships/fighter-1"_id;
            const double spawnOffset = 200. * static_cast<double>(m_peers.size() - 1);
            const flecs::entity ship = m_entitySpawner.SpawnPlayer(playerModel, Vector2d{spawnOffset, 0.});
            it->second.ship = ship;
            it->second.welcomed = true;

            ServerWelcomePacket welcome;
            welcome.clientId = peer;
            welcome.yourShipNetId = ship.get<NetId>().value;
            welcome.tickRate = 60;

            ByteWriter writer;
            WriteServerWelcome(welcome, writer);
            m_transport.Send(peer, 0, writer.Data(), writer.Size(), true);
            LOG(info) << "net: peer " << peer << " welcomed, ship NetId " << welcome.yourShipNetId
                      << " at (" << spawnOffset << ", 0)";
            break;
        }
        case PacketType::ClientInput: {
            ClientInputPacket input;
            if (!ReadClientInputBody(reader, input)) return;

            const auto it = m_peers.find(peer);
            if (it == m_peers.end() || !it->second.welcomed || !it->second.ship.is_alive()) return;

            InputQueue& queue = it->second.ship.get_mut<InputQueue>();
            for (const InputCommand& cmd : input.commands) queue.Push(cmd);
            break;
        }
        case PacketType::ServerWelcome:
        case PacketType::Snapshot:
            break; // server never receives these
    }
}

void NetServer::HandleDisconnect(PeerId peer)
{
    const auto it = m_peers.find(peer);
    if (it == m_peers.end()) return;
    if (it->second.ship.is_alive()) it->second.ship.destruct();
    m_peers.erase(it);
}

void NetServer::BroadcastSnapshot(std::uint64_t currentTick)
{
    for (auto& [peer, state] : m_peers) {
        if (!state.welcomed) continue;

        SnapshotData snapshot;
        GatherSnapshot(m_registry, m_eventQueue, currentTick, state.lastSentEventSeq, snapshot);
        if (!snapshot.events.empty()) {
            state.lastSentEventSeq = snapshot.events.back().seq;
        }

        ByteWriter writer;
        WriteSnapshotPacket(snapshot, writer);
        m_transport.Send(peer, 0, writer.Data(), writer.Size(), false);
    }
}

} // namespace Gravitaris
