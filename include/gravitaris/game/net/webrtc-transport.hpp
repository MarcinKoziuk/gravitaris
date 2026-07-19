#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <gravitaris/game/net/transport.hpp>

namespace rtc {
class PeerConnection;
class DataChannel;
class WebSocket;
} // namespace rtc

namespace Gravitaris {

// Single-peer WebRTC transport over an rtc::DataChannel (docs/networking
// -plan.md 3.1b): game traffic rides an unreliable/unordered data channel
// (SCTP-over-DTLS-over-UDP), avoiding the TCP head-of-line blocking a
// WebSocket transport would impose on this protocol's already-redundant,
// loss-tolerant design (NetClient's resend window, snapshot-not-delta
// replication). Backed by libdatachannel natively and datachannel-wasm under
// Emscripten -- same rtc:: header API, picked by CMake (Gravitaris::WebRTC).
//
// WebRTC has no built-in signaling: two peers can't reach a connected
// DataChannel without some other channel carrying the SDP offer/answer and
// ICE candidates first. This class doesn't provide one -- the local-
// description/candidate callbacks hand the caller what to relay to the
// remote peer, and SetRemoteDescription/AddRemoteCandidate feed in what
// arrives from it. A real deployment needs a small signaling server (not yet
// built); the sim-test proof shuttles these directly between two in-process
// instances instead.
//
// Handles exactly one peer connection (like LoopbackTransport): a server
// juggling several WebRTC clients needs one instance per client. Multiplexing
// that behind a single INetTransport is deferred until a real signaling
// server exists to drive "a new client wants to connect" in the first place.
class WebRtcTransport : public INetTransport {
public:
    enum class Role {
        Offerer,  // creates the data channel; client role
        Answerer, // waits for the remote-created data channel; server role
    };

    explicit WebRtcTransport(Role role);
    ~WebRtcTransport() override;

    WebRtcTransport(const WebRtcTransport&) = delete;
    WebRtcTransport& operator=(const WebRtcTransport&) = delete;

    // Signaling seam -- see class comment. Callbacks may fire from
    // libdatachannel's own worker thread. Register both before calling
    // Connect(): an Offerer's local description/candidates can otherwise
    // fire (and be silently dropped -- these are plain callback slots, not
    // queues) before a caller-installed handler exists to catch them.
    void SetLocalDescriptionCallback(std::function<void(const std::string& sdp, const std::string& type)> cb);
    void SetLocalCandidateCallback(std::function<void(const std::string& candidate, const std::string& mid)> cb);
    void SetRemoteDescription(const std::string& sdp, const std::string& type);
    void AddRemoteCandidate(const std::string& candidate, const std::string& mid);

    // Starts negotiation: an Offerer creates its data channel here (which
    // triggers the local description/candidate callbacks above), an Answerer
    // just starts listening for the remote-created one via onDataChannel.
    void Connect();

    // Offerer/client-role convenience: drives the signaling seam above over
    // an rtc::WebSocket instead of manual callbacks (docs/networking-plan.md
    // 3.5.1 -- the WebSocket carries only signaling frames, never game
    // traffic). Installs its own local-description/candidate callbacks and
    // calls Connect() once the socket is open, then closes the socket once
    // the data channel itself opens. Do not also call
    // SetLocalDescriptionCallback/SetLocalCandidateCallback/Connect() when
    // using this path -- it owns them.
    void ConnectSignaling(const std::string& wsUrl);

    // `reliable` is currently ignored: the data channel's reliability mode is
    // fixed (unordered, unreliable) at creation to match the protocol's own
    // redundancy-based design -- see the class comment.
    void Send(PeerId peer, std::uint8_t channel, const std::uint8_t* data, std::size_t size,
              bool reliable) override;
    std::vector<NetEvent> Poll() override;

private:
    void BindDataChannel(std::shared_ptr<rtc::DataChannel> channel);

    Role m_role;
    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::DataChannel> m_dc;
    std::shared_ptr<rtc::WebSocket> m_ws; // only set when using ConnectSignaling()

    // Poll() runs on the game thread; libdatachannel's onOpen/onMessage
    // callbacks fire from its own internal thread.
    std::mutex m_mutex;
    std::queue<NetEvent> m_incoming;
};

} // namespace Gravitaris
