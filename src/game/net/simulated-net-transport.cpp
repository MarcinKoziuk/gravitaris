#include <algorithm>

#include <gravitaris/game/net/simulated-net-transport.hpp>

namespace Gravitaris {

SimulatedNetTransport::SimulatedNetTransport(INetTransport& inner)
        : m_inner(inner)
{}

std::chrono::steady_clock::time_point SimulatedNetTransport::ComputeReleaseTime()
{
    std::uniform_real_distribution<float> jitterDist(-m_params.jitterMs, m_params.jitterMs);
    const float delayMs = std::max(m_params.delayMs + jitterDist(m_rng), 0.f);
    return std::chrono::steady_clock::now()
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<float, std::milli>(delayMs));
}

bool SimulatedNetTransport::RollLoss()
{
    if (m_params.lossPercent <= 0.f) return false;
    std::uniform_real_distribution<float> lossDist(0.f, 100.f);
    return lossDist(m_rng) < m_params.lossPercent;
}

void SimulatedNetTransport::Send(PeerId peer, std::uint8_t channel, const std::uint8_t* data, std::size_t size,
                                 bool reliable)
{
    if (m_params.IsPassthrough()) {
        m_inner.Send(peer, channel, data, size, reliable);
        return;
    }
    if (RollLoss()) return; // vanished -- never reaches the real transport at all

    PendingSend pending;
    pending.peer = peer;
    pending.channel = channel;
    pending.data.assign(data, data + size);
    pending.reliable = reliable;
    pending.releaseTime = ComputeReleaseTime();
    m_pendingSends.push_back(std::move(pending));
}

std::vector<NetEvent> SimulatedNetTransport::Poll()
{
    const auto now = std::chrono::steady_clock::now();

    // Flush outgoing sends whose simulated transit time has elapsed. Full
    // scan rather than peeking front(): per-packet jitter means release
    // times aren't necessarily monotonic in send order, so an earlier-queued
    // packet can legitimately still be in flight while a later, luckier one
    // is already due -- exactly the packet reordering a real unreliable/
    // unordered channel can produce, which this protocol already tolerates.
    for (auto it = m_pendingSends.begin(); it != m_pendingSends.end();) {
        if (it->releaseTime <= now) {
            m_inner.Send(it->peer, it->channel, it->data.data(), it->data.size(), it->reliable);
            it = m_pendingSends.erase(it);
        } else {
            ++it;
        }
    }

    // Ingest freshly-arrived events from the real transport. Connected/
    // Disconnected pass through immediately, undelayed and undroppable (see
    // class doc comment); only Packet events are simulated.
    for (NetEvent& event : m_inner.Poll()) {
        if (event.type != NetEventType::Packet) {
            m_pendingEvents.push_back({std::move(event), now});
            continue;
        }
        if (RollLoss()) continue; // vanished -- caller never sees it
        m_pendingEvents.push_back({std::move(event), ComputeReleaseTime()});
    }

    std::vector<NetEvent> out;
    for (auto it = m_pendingEvents.begin(); it != m_pendingEvents.end();) {
        if (it->releaseTime <= now) {
            out.push_back(std::move(it->event));
            it = m_pendingEvents.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

} // namespace Gravitaris
