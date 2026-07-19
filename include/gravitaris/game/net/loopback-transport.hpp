#pragma once

#include <memory>
#include <queue>

#include <gravitaris/game/net/transport.hpp>

namespace Gravitaris {

// In-process transport connecting exactly two endpoints (docs/networking
// -plan.md 3.1): what NetServer/NetClient run over before a real transport
// exists, and what protocol tests use in the sim-test target without
// sockets. Zero platform dependency by construction, so it's wasm-safe for
// free -- it's the one INetTransport impl every build configuration gets.
//
// Each side sees the other as SERVER_PEER (a loopback pair has only one
// reachable peer, and SERVER_PEER == 1 is the transport-agnostic "the other
// end" convention every client-role transport honors -- see transport.hpp);
// both sides receive a Connected event on their first Poll() after
// CreatePair(), matching what a real transport does on handshake.
class LoopbackTransport : public INetTransport {
    struct SharedState {
        std::queue<NetEvent> toA;
        std::queue<NetEvent> toB;
    };

    std::shared_ptr<SharedState> m_shared;
    bool m_isA;

    LoopbackTransport(std::shared_ptr<SharedState> shared, bool isA);

public:
    // The two returned instances are connected to each other; destroying one
    // does not notify the other (LoopbackTransport does not model
    // disconnection -- the sim-test/caller controls both lifetimes directly).
    static std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>> CreatePair();

    void Send(PeerId peer, std::uint8_t channel, const std::uint8_t* data, std::size_t size,
              bool reliable) override;
    std::vector<NetEvent> Poll() override;
};

} // namespace Gravitaris
