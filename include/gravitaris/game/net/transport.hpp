#pragma once

#include <cstdint>
#include <vector>

namespace Gravitaris {

// Opaque per-connection handle, meaningful only to the INetTransport instance
// that issued it. 0 is reserved invalid (matches NetId's convention).
using PeerId = std::uint32_t;
constexpr PeerId INVALID_PEER = 0;

// For a client-role transport (exactly one connection, to one server), the
// server is always reachable as this PeerId. Every INetTransport
// implementation used client-side must honor this convention so NetClient
// (and any other client-side code) can address "the server" without knowing
// which transport it's actually running over.
constexpr PeerId SERVER_PEER = 1;

enum class NetEventType : std::uint8_t {
    Connected,
    Disconnected,
    Packet,
};

struct NetEvent {
    NetEventType type = NetEventType::Packet;
    PeerId peer = INVALID_PEER;
    std::vector<std::uint8_t> data; // Packet only; empty otherwise
};

// Transport abstraction (docs/networking-plan.md 3.1): the sim/protocol layer
// never touches sockets directly, so the same NetServer/NetClient code runs
// over LoopbackTransport (tests, no platform dependency) and, for real
// connections, WebRtcTransport -- which must work in a browser build
// (Emscripten can't open raw UDP sockets), so "real" here means WebRTC data
// channels via libdatachannel/datachannel-wasm, not raw UDP or ENet.
class INetTransport {
public:
    virtual ~INetTransport() = default;

    // `reliable` is a hint the transport may or may not need (LoopbackTransport
    // ignores it -- an in-process queue can't lose or reorder). `channel` is
    // reserved for a future reliable/unreliable split; unused for now.
    virtual void Send(PeerId peer, std::uint8_t channel, const std::uint8_t* data, std::size_t size,
                      bool reliable) = 0;

    // Drains and returns every event since the last call. Never blocks.
    virtual std::vector<NetEvent> Poll() = 0;
};

} // namespace Gravitaris
