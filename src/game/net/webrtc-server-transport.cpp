#include <rtc/rtc.hpp>

#include <gravitaris/game/net/webrtc-server-transport.hpp>
#include <gravitaris/game/net/webrtc-signaling.hpp>
#include <gravitaris/game/net/webrtc-transport.hpp>

namespace Gravitaris {

struct WebRtcServerTransport::PeerState {
    std::shared_ptr<rtc::WebSocket> ws;
    std::unique_ptr<WebRtcTransport> transport;
};

WebRtcServerTransport::WebRtcServerTransport(uint16_t port)
{
    rtc::WebSocketServerConfiguration config;
    config.port = port;
    m_wsServer = std::make_unique<rtc::WebSocketServer>(config);

    m_wsServer->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
        const PeerId peer = m_nextPeerId++;

        auto peerState = std::make_unique<PeerState>();
        peerState->ws = ws;
        peerState->transport = std::make_unique<WebRtcTransport>(WebRtcTransport::Role::Answerer);

        peerState->transport->SetLocalDescriptionCallback([ws](const std::string& sdp, const std::string& type) {
            if (ws->isOpen()) ws->send(EncodeDescriptionFrame(sdp, type));
        });
        peerState->transport->SetLocalCandidateCallback([ws](const std::string& candidate, const std::string& mid) {
            if (ws->isOpen()) ws->send(EncodeCandidateFrame(candidate, mid));
        });
        peerState->transport->Connect();

        WebRtcTransport* transportPtr = peerState->transport.get();
        ws->onMessage(
                [](rtc::binary) {}, // signaling is text-only
                [transportPtr](rtc::string data) {
                    const std::optional<SignalingFrame> frame = DecodeSignalingFrame(data);
                    if (!frame) return;
                    switch (frame->kind) {
                        case SignalingFrame::Kind::Description:
                            transportPtr->SetRemoteDescription(frame->b, frame->a);
                            break;
                        case SignalingFrame::Kind::Candidate:
                            transportPtr->AddRemoteCandidate(frame->b, frame->a);
                            break;
                    }
                });

        std::lock_guard lock(m_mutex);
        m_peers.emplace(peer, std::move(peerState));
    });
}

WebRtcServerTransport::~WebRtcServerTransport()
{
    m_wsServer->stop();
}

void WebRtcServerTransport::Send(PeerId peer, std::uint8_t channel, const std::uint8_t* data, std::size_t size,
                                 bool reliable)
{
    std::lock_guard lock(m_mutex);
    const auto it = m_peers.find(peer);
    if (it == m_peers.end()) return;
    it->second->transport->Send(SERVER_PEER, channel, data, size, reliable);
}

std::vector<NetEvent> WebRtcServerTransport::Poll()
{
    std::lock_guard lock(m_mutex);

    std::vector<NetEvent> out;
    std::vector<PeerId> disconnected;
    for (auto& [peer, state] : m_peers) {
        for (NetEvent& event : state->transport->Poll()) {
            event.peer = peer;
            if (event.type == NetEventType::Disconnected) disconnected.push_back(peer);
            out.push_back(std::move(event));
        }
    }
    // Erased after the loop above (not inline) to avoid invalidating m_peers
    // mid-iteration; a signaling frame arriving for an already-erased peer
    // between this Poll() and the next is a known, low-risk gap (its
    // WebSocket has already served its purpose by the time a data channel's
    // Disconnected fires).
    for (PeerId peer : disconnected) m_peers.erase(peer);

    return out;
}

} // namespace Gravitaris
