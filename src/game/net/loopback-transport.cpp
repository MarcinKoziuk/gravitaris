#include <gravitaris/game/net/loopback-transport.hpp>

namespace Gravitaris {

LoopbackTransport::LoopbackTransport(std::shared_ptr<SharedState> shared, bool isA)
        : m_shared(std::move(shared))
        , m_isA(isA)
{}

std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>> LoopbackTransport::CreatePair()
{
    auto shared = std::make_shared<SharedState>();

    // Queue each side's own Connected event onto the OTHER side's outgoing
    // queue in reverse -- simplest correct way to say "each side's own Poll()
    // yields Connected first" without a third bespoke code path.
    shared->toA.push(NetEvent{NetEventType::Connected, SERVER_PEER, {}});
    shared->toB.push(NetEvent{NetEventType::Connected, SERVER_PEER, {}});

    auto a = std::unique_ptr<LoopbackTransport>(new LoopbackTransport(shared, true));
    auto b = std::unique_ptr<LoopbackTransport>(new LoopbackTransport(shared, false));
    return {std::move(a), std::move(b)};
}

void LoopbackTransport::Send(PeerId peer, std::uint8_t /*channel*/, const std::uint8_t* data, std::size_t size,
                             bool /*reliable*/)
{
    if (peer != SERVER_PEER) return; // loopback has exactly one reachable peer

    NetEvent event{NetEventType::Packet, SERVER_PEER, std::vector<std::uint8_t>(data, data + size)};
    if (m_isA) {
        m_shared->toB.push(std::move(event));
    } else {
        m_shared->toA.push(std::move(event));
    }
}

std::vector<NetEvent> LoopbackTransport::Poll()
{
    std::queue<NetEvent>& incoming = m_isA ? m_shared->toA : m_shared->toB;

    std::vector<NetEvent> out;
    out.reserve(incoming.size());
    while (!incoming.empty()) {
        out.push_back(std::move(incoming.front()));
        incoming.pop();
    }
    return out;
}

} // namespace Gravitaris
