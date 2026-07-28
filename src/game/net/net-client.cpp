#include <algorithm>
#include <cmath>

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
                m_serverClockStarted = false;
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
                        if (!m_snapshotHistory.empty() && snapshot.tick <= m_snapshotHistory.back().tick) {
                            ++m_droppedSnapshotCount;
                            break;
                        }

                        const auto now = std::chrono::steady_clock::now();
                        if (m_lastAckedSnapshotRecvTime) {
                            m_lastSnapshotIntervalMs =
                                    std::chrono::duration<float, std::milli>(now - *m_lastAckedSnapshotRecvTime).count();
                        }
                        ++m_acceptedSnapshotCount;

                        m_lastAckedSnapshotTick = snapshot.tick;
                        m_lastAckedSnapshotRecvTime = now;
                        for (const GameEvent& e : snapshot.events) {
                            if (e.seq > m_lastAckedEventSeq) m_lastAckedEventSeq = e.seq;
                        }
                        m_snapshotHistory.push_back(snapshot);
                        while (m_snapshotHistory.size() > SNAPSHOT_HISTORY_CAPACITY) m_snapshotHistory.pop_front();
                        m_latestSnapshot = std::move(snapshot);
                        break;
                    }
                    case PacketType::Pong: {
                        PongPacket pong;
                        if (!ReadPongBody(reader, pong)) break;
                        const auto pendingIt = m_pendingPings.find(pong.seq);
                        if (pendingIt == m_pendingPings.end()) break; // already pruned as stale, or a resend/dup
                        const auto now = std::chrono::steady_clock::now();
                        m_lastPingMs =
                                std::chrono::duration<float, std::milli>(now - pendingIt->second).count();
                        // Deviation against the average as it stood *before*
                        // this sample folds in, or the average would already
                        // have moved toward the sample and understate it.
                        const float deviation =
                                m_avgPingMs < 0.f ? 0.f : std::fabs(m_lastPingMs - m_avgPingMs);
                        m_pingJitterMs = m_pingJitterMs < 0.f
                                ? deviation
                                : m_pingJitterMs + PING_EMA_ALPHA * (deviation - m_pingJitterMs);
                        m_avgPingMs = m_avgPingMs < 0.f
                                ? m_lastPingMs
                                : m_avgPingMs + PING_EMA_ALPHA * (m_lastPingMs - m_avgPingMs);
                        m_pendingPings.erase(pendingIt);
                        break;
                    }
                    case PacketType::ClientHello:
                    case PacketType::ClientInput:
                    case PacketType::Ping:
                        break; // client never receives these
                }
                break;
            }
        }
    }

    UpdateServerClock();
    UpdateInputLead();
    SendPing();
}

std::uint64_t NetClient::ComputeInputLeadTicks(float rttMs, float jitterMs, std::uint32_t tickRate)
{
    // Two jitter deviations, so an ordinary bad packet is covered rather
    // than only the typical one.
    static constexpr float JITTER_ALLOWANCE = 2.f;
    // A tick's own width of slack on top: the command is stamped against an
    // estimate that is itself quantized by the client's frame cadence.
    const float msPerTick = 1000.f / static_cast<float>(std::max(tickRate, 1u));
    const float needMs = std::max(rttMs, 0.f) + JITTER_ALLOWANCE * std::max(jitterMs, 0.f) + msPerTick;

    const auto ticks = static_cast<std::uint64_t>(std::ceil(needMs / msPerTick));
    return std::clamp(ticks, MIN_INPUT_LEAD_TICKS, MAX_INPUT_LEAD_TICKS);
}

std::uint64_t NetClient::GetSuggestedInputLeadTicks() const
{
    if (m_avgPingMs < 0.f) return INPUT_LEAD_TICKS;
    return ComputeInputLeadTicks(m_avgPingMs, std::max(m_pingJitterMs, 0.f), m_tickRate);
}

void NetClient::UpdateInputLead()
{
    if (!m_inputLeadAuto || m_avgPingMs < 0.f) return;

    const std::uint64_t suggested = GetSuggestedInputLeadTicks();
    if (suggested >= m_inputLeadTicks) {
        m_inputLeadTicks = suggested;
        m_leadRelaxSince.reset();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!m_leadRelaxSince) {
        m_leadRelaxSince = now;
        return;
    }
    if (std::chrono::duration<double>(now - *m_leadRelaxSince).count() >= LEAD_RELAX_SECONDS) {
        --m_inputLeadTicks;
        m_leadRelaxSince = now;
    }
}

void NetClient::UpdateServerClock()
{
    if (!m_lastAckedSnapshotRecvTime) return;

    const auto now = std::chrono::steady_clock::now();
    const double elapsedSeconds = std::chrono::duration<double>(now - *m_lastAckedSnapshotRecvTime).count();
    // Deliberately not floored, unlike EstimateCurrentServerTick: this is the
    // signal the smoothing below tracks, and quantizing it to whole ticks
    // would just be a second staircase for the filter to chase.
    const double arrivalEstimate =
            static_cast<double>(m_lastAckedSnapshotTick) + std::max(elapsedSeconds, 0.0) * m_tickRate;

    if (!m_serverClockStarted) {
        m_smoothedServerTick = arrivalEstimate;
        m_serverClockStarted = true;
        m_lastServerClockUpdate = now;
        return;
    }

    const double dtSeconds = std::chrono::duration<double>(now - *m_lastServerClockUpdate).count();
    m_lastServerClockUpdate = now;

    const double error = arrivalEstimate - m_smoothedServerTick;
    if (std::fabs(error) > CLOCK_SNAP_TICKS) {
        m_smoothedServerTick = arrivalEstimate;
        ++m_clockSnapCount;
        return;
    }

    const double rateAdjust =
            std::clamp(error * CLOCK_SMOOTHING_GAIN, -CLOCK_MAX_RATE_ADJUST, CLOCK_MAX_RATE_ADJUST);
    m_smoothedServerTick += dtSeconds * m_tickRate * (1.0 + rateAdjust);
}

void NetClient::SendPing()
{
    if (!m_connected) return;

    const auto now = std::chrono::steady_clock::now();
    if (m_lastPingSentTime &&
        std::chrono::duration<float>(now - *m_lastPingSentTime).count() < PING_INTERVAL_SECONDS) {
        return;
    }
    m_lastPingSentTime = now;

    // Prune stale entries first (a Pong that's never coming, e.g. the
    // matching Ping was itself dropped) so this map can't grow unbounded
    // over a long-lived connection.
    for (auto it = m_pendingPings.begin(); it != m_pendingPings.end();) {
        if (std::chrono::duration<float>(now - it->second).count() > PING_STALE_SECONDS) {
            it = m_pendingPings.erase(it);
        } else {
            ++it;
        }
    }

    const std::uint32_t seq = m_nextPingSeq++;
    m_pendingPings[seq] = now;

    PingPacket ping;
    ping.seq = seq;
    ByteWriter writer;
    WritePing(ping, writer);
    m_transport.Send(SERVER_PEER, 0, writer.Data(), writer.Size(), false);
}

std::uint64_t NetClient::EstimateCurrentServerTick() const
{
    if (!m_lastAckedSnapshotRecvTime) return m_lastAckedSnapshotTick;
    const float elapsedSeconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - *m_lastAckedSnapshotRecvTime).count();
    const auto elapsedTicks = static_cast<std::uint64_t>(std::max(elapsedSeconds, 0.f) * static_cast<float>(m_tickRate));
    return m_lastAckedSnapshotTick + elapsedTicks;
}

void NetClient::SendInput(std::uint64_t tick, const ControlFlags& flags, UpgradePick upgradePick)
{
    if (!m_welcomed) return;

    InputCommand cmd;
    cmd.tick = tick;
    cmd.flags = flags;
    cmd.upgradePick = upgradePick;
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
