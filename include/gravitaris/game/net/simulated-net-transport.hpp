#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include <gravitaris/game/net/transport.hpp>

namespace Gravitaris {

// Debug-only decorator: wraps a real INetTransport and adds artificial
// delay/jitter/packet loss in both directions, so lag can be dialed in
// deterministically from the Net debug tab instead of hoping an OS/browser
// tool cooperates. (Chrome DevTools' network throttling doesn't touch WebRTC
// data channels at all -- a known DevTools limitation, not something wrong
// with this project's transport -- so it's a no-op once in-game over
// WebRtcTransport. A tool like clumsy (Windows, WinDivert-based) works
// because it intercepts at the OS level below WebRTC, but that's Windows
// -only and won't help the wasm/browser build either.)
//
// Only NetEventType::Packet is delayed/dropped -- Connected/Disconnected are
// transport-level state transitions the underlying transport itself never
// resends, so delaying or dropping one would desync this class's own
// bookkeeping from reality for no simulation benefit. A real network doesn't
// "lose" a TCP handshake's underlying link-up event either; the analogy here
// is the same.
//
// Headless (game/-level, ADR constraint 1) like every other INetTransport
// implementation, so it's usable from gravitaris-sim-test and from a
// dedicated server too, not just cgame. Its own RNG is seeded from
// std::random_device, not the deterministic per-tick seed streams the
// *simulation* uses (ADR 0001 constraint 5) -- this class sits below the
// protocol layer, simulating an unpredictable network, and is never part of
// replayable sim state.
class SimulatedNetTransport : public INetTransport {
public:
    struct Params {
        // One-way delay added to every packet, each direction independently
        // (so Send + the matching reply's Poll each pay this once -- two
        // instances, one per peer, roughly double it for a round trip, same
        // as a real network's two hops).
        float delayMs = 0.f;
        // Uniformly randomized +/- this much on top of delayMs per packet
        // (simulates jitter, not just flat latency).
        float jitterMs = 0.f;
        // 0-100: independent chance to silently drop a packet outright
        // (never delivered at all, not just delayed).
        float lossPercent = 0.f;

        [[nodiscard]] bool IsPassthrough() const
        {
            return delayMs <= 0.f && jitterMs <= 0.f && lossPercent <= 0.f;
        }
    };

    explicit SimulatedNetTransport(INetTransport& inner);

    // `params` is a live reference (Net debug tab sliders write straight
    // into it) -- no getter/setter pair needed, this class only ever reads
    // it fresh each call.
    Params& GetParams() { return m_params; }

    void Send(PeerId peer, std::uint8_t channel, const std::uint8_t* data, std::size_t size,
              bool reliable) override;
    std::vector<NetEvent> Poll() override;

private:
    struct PendingSend {
        PeerId peer;
        std::uint8_t channel;
        std::vector<std::uint8_t> data;
        bool reliable;
        std::chrono::steady_clock::time_point releaseTime;
    };
    struct PendingEvent {
        NetEvent event;
        std::chrono::steady_clock::time_point releaseTime;
    };

    [[nodiscard]] std::chrono::steady_clock::time_point ComputeReleaseTime();
    [[nodiscard]] bool RollLoss();

    INetTransport& m_inner;
    Params m_params;
    std::mt19937 m_rng{std::random_device{}()};
    std::deque<PendingSend> m_pendingSends;
    std::deque<PendingEvent> m_pendingEvents;
};

} // namespace Gravitaris
