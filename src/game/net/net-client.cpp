#include <algorithm>

#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/protocol.hpp>
#include <gravitaris/game/net/net-client.hpp>

namespace Gravitaris {

NetClient::NetClient(INetTransport& transport, std::string name)
        : m_transport(transport)
        , m_name(std::move(name))
{}

void NetClient::Update()
{
    for (NetEvent& event : m_transport.Poll()) {
        switch (event.type) {
            case NetEventType::Connected: {
                m_connected = true;
                ClientHelloPacket hello;
                hello.name = m_name;
                hello.requestedTeam = m_requestedTeam;
                ByteWriter writer;
                WriteClientHello(hello, writer);
                m_transport.Send(SERVER_PEER, 0, writer.Data(), writer.Size(), true);
                break;
            }
            case NetEventType::Disconnected:
                m_connected = false;
                m_welcomed = false;
                break;
            case NetEventType::Packet: {
                ByteReader reader(event.data.data(), event.data.size());
                const auto type = static_cast<PacketType>(reader.ReadU8());
                switch (type) {
                    case PacketType::ServerWelcome: {
                        ServerWelcomePacket welcome;
                        if (!ReadServerWelcomeBody(reader, welcome)) break;
                        m_clientId = welcome.clientId;
                        m_yourShipNetId = welcome.yourShipNetId;
                        m_tickRate = welcome.tickRate;
                        m_yourTeam = welcome.yourTeam;
                        m_welcomed = true;
                        break;
                    }
                    case PacketType::Snapshot: {
                        SnapshotData snapshot;
                        if (!ReadSnapshot(reader, snapshot)) break;
                        // Unordered transport: a snapshot older than (or
                        // equal to -- a resend) what's already buffered
                        // must not roll the tick/history back or duplicate
                        // an entry.
                        if (!m_snapshotHistory.empty() && snapshot.tick <= m_snapshotHistory.back().tick) break;

                        m_lastAckedSnapshotTick = snapshot.tick;
                        m_lastAckedSnapshotRecvTime = std::chrono::steady_clock::now();
                        for (const GameEvent& e : snapshot.events) {
                            if (e.seq > m_lastAckedEventSeq) m_lastAckedEventSeq = e.seq;
                        }
                        m_snapshotHistory.push_back(snapshot);
                        while (m_snapshotHistory.size() > SNAPSHOT_HISTORY_CAPACITY) m_snapshotHistory.pop_front();
                        m_latestSnapshot = std::move(snapshot);
                        break;
                    }
                    case PacketType::ClientHello:
                    case PacketType::ClientInput:
                        break; // client never receives these
                }
                break;
            }
        }
    }
}

std::uint64_t NetClient::EstimateCurrentServerTick() const
{
    if (!m_lastAckedSnapshotRecvTime) return m_lastAckedSnapshotTick;
    const float elapsedSeconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - *m_lastAckedSnapshotRecvTime).count();
    const auto elapsedTicks = static_cast<std::uint64_t>(std::max(elapsedSeconds, 0.f) * static_cast<float>(m_tickRate));
    return m_lastAckedSnapshotTick + elapsedTicks;
}

void NetClient::SendInput(std::uint64_t tick, const ControlFlags& flags)
{
    if (!m_welcomed) return;

    InputCommand cmd;
    cmd.tick = tick;
    cmd.flags = flags;
    m_recentCommands.push_back(cmd);
    while (m_recentCommands.size() > CLIENT_INPUT_BACKUP) m_recentCommands.pop_front();

    ClientInputPacket packet;
    packet.lastAckedSnapshotTick = m_lastAckedSnapshotTick;
    packet.lastAckedEventSeq = m_lastAckedEventSeq;
    packet.commands.assign(m_recentCommands.begin(), m_recentCommands.end());

    ByteWriter writer;
    WriteClientInput(packet, writer);
    m_transport.Send(SERVER_PEER, 0, writer.Data(), writer.Size(), false);
}

} // namespace Gravitaris
