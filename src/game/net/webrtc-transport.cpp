#include <rtc/rtc.hpp>

#include <gravitaris/game/net/webrtc-signaling.hpp>
#include <gravitaris/game/net/webrtc-transport.hpp>

namespace Gravitaris {

WebRtcTransport::WebRtcTransport(Role role)
        : m_role(role)
        // rtc::PeerConnection's no-arg constructor is declared but never
        // defined in datachannel-wasm (link error under Emscripten only) --
        // pass an explicit empty Configuration, which both backends
        // implement and behaves identically to the no-arg ctor on native.
        , m_pc(std::make_shared<rtc::PeerConnection>(rtc::Configuration{}))
{}

WebRtcTransport::~WebRtcTransport() = default;

void WebRtcTransport::Connect()
{
    m_pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) { BindDataChannel(std::move(channel)); });

    if (m_role == Role::Offerer) {
        rtc::DataChannelInit init;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0; // unset alongside maxPacketLifeTime => unreliable
        BindDataChannel(m_pc->createDataChannel("game", init));
    }
}

void WebRtcTransport::BindDataChannel(std::shared_ptr<rtc::DataChannel> channel)
{
    m_dc = std::move(channel);

    m_dc->onOpen([this]() {
        if (m_ws) m_ws->close(); // signaling's job is done once the data channel itself is up
        std::lock_guard lock(m_mutex);
        m_incoming.push(NetEvent{NetEventType::Connected, SERVER_PEER, {}});
    });
    m_dc->onClosed([this]() {
        std::lock_guard lock(m_mutex);
        m_incoming.push(NetEvent{NetEventType::Disconnected, SERVER_PEER, {}});
    });
    m_dc->onMessage(
            [this](rtc::binary data) {
                std::lock_guard lock(m_mutex);
                m_incoming.push(NetEvent{NetEventType::Packet, SERVER_PEER,
                                         std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()),
                                                                   reinterpret_cast<const std::uint8_t*>(data.data())
                                                                           + data.size())});
            },
            [](rtc::string) {}); // text messages aren't part of this protocol; drop silently
}

void WebRtcTransport::SetLocalDescriptionCallback(
        std::function<void(const std::string& sdp, const std::string& type)> cb)
{
    m_pc->onLocalDescription(
            [cb = std::move(cb)](rtc::Description description) { cb(std::string(description), description.typeString()); });
}

void WebRtcTransport::SetLocalCandidateCallback(
        std::function<void(const std::string& candidate, const std::string& mid)> cb)
{
    m_pc->onLocalCandidate(
            [cb = std::move(cb)](rtc::Candidate candidate) { cb(std::string(candidate), candidate.mid()); });
}

void WebRtcTransport::SetRemoteDescription(const std::string& sdp, const std::string& type)
{
    m_pc->setRemoteDescription(rtc::Description(sdp, type));
}

void WebRtcTransport::AddRemoteCandidate(const std::string& candidate, const std::string& mid)
{
    m_pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
}

void WebRtcTransport::ConnectSignaling(const std::string& wsUrl)
{
    SetLocalDescriptionCallback([this](const std::string& sdp, const std::string& type) {
        if (m_ws && m_ws->isOpen()) m_ws->send(EncodeDescriptionFrame(sdp, type));
    });
    SetLocalCandidateCallback([this](const std::string& candidate, const std::string& mid) {
        if (m_ws && m_ws->isOpen()) m_ws->send(EncodeCandidateFrame(candidate, mid));
    });

    m_ws = std::make_shared<rtc::WebSocket>();
    m_ws->onOpen([this]() { Connect(); });
    m_ws->onMessage(
            [](rtc::binary) {}, // signaling is text-only
            [this](rtc::string data) {
                const std::optional<SignalingFrame> frame = DecodeSignalingFrame(data);
                if (!frame) return;
                switch (frame->kind) {
                    case SignalingFrame::Kind::Description:
                        SetRemoteDescription(frame->b, frame->a);
                        break;
                    case SignalingFrame::Kind::Candidate:
                        AddRemoteCandidate(frame->b, frame->a);
                        break;
                }
            });
    m_ws->open(wsUrl);
}

void WebRtcTransport::Send(PeerId peer, std::uint8_t /*channel*/, const std::uint8_t* data, std::size_t size,
                           bool /*reliable*/)
{
    if (peer != SERVER_PEER || !m_dc || !m_dc->isOpen()) return;
    m_dc->send(reinterpret_cast<const rtc::byte*>(data), size);
}

std::vector<NetEvent> WebRtcTransport::Poll()
{
    std::lock_guard lock(m_mutex);
    std::vector<NetEvent> out;
    out.reserve(m_incoming.size());
    while (!m_incoming.empty()) {
        out.push_back(std::move(m_incoming.front()));
        m_incoming.pop();
    }
    return out;
}

} // namespace Gravitaris
