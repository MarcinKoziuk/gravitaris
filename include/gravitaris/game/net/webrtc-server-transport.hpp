#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <gravitaris/game/net/transport.hpp>
#include <gravitaris/game/net/webrtc-transport.hpp>

namespace rtc {
class WebSocketServer;
} // namespace rtc

namespace Gravitaris {

// Native-only (a browser can't listen): the multi-peer counterpart to
// WebRtcTransport, for gravitaris-server (docs/networking-plan.md 3.5.2).
// One rtc::WebSocketServer accepts signaling connections; each accepted
// WebSocket gets its own WebRtcTransport (Answerer role) driven manually
// (not via WebRtcTransport::ConnectSignaling(), which is the Offerer/client
// path) -- SDP/ICE frames are relayed over that peer's WebSocket the same
// wire format webrtc-signaling.hpp defines, and the socket is closed once
// that peer's data channel opens. NetServer needs zero changes: this just
// fans PeerId-tagged events in and out across however many WebRtcTransport
// children are live.
class WebRtcServerTransport : public INetTransport {
public:
    // iceServers is handed to every per-peer WebRtcTransport; the server needs
    // its own server-reflexive candidate just as much as the client does --
    // see DefaultIceServers() in webrtc-transport.hpp.
    explicit WebRtcServerTransport(uint16_t port, std::vector<std::string> iceServers = DefaultIceServers());
    ~WebRtcServerTransport() override;

    WebRtcServerTransport(const WebRtcServerTransport&) = delete;
    WebRtcServerTransport& operator=(const WebRtcServerTransport&) = delete;

    void Send(PeerId peer, std::uint8_t channel, const std::uint8_t* data, std::size_t size,
              bool reliable) override;
    std::vector<NetEvent> Poll() override;

private:
    struct PeerState;

    std::unique_ptr<rtc::WebSocketServer> m_wsServer;

    std::mutex m_mutex; // guards m_peers -- onClient fires from libdatachannel's own thread
    ankerl::unordered_dense::map<PeerId, std::unique_ptr<PeerState>> m_peers;
    PeerId m_nextPeerId = 2; // 1 (SERVER_PEER) is a client-side transport's name for "the server", not used here
};

} // namespace Gravitaris
