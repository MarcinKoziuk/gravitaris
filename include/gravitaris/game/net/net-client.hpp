#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/input/input-command.hpp>
#include <gravitaris/game/net/snapshot.hpp>
#include <gravitaris/game/net/transport.hpp>

namespace Gravitaris {

// Client side of Protocol v1. Deliberately headless/render-free (game/ only,
// ADR 0001 constraint 1): it decodes snapshots into SnapshotData and hands
// them to the caller, it doesn't apply them -- that's cgame's
// SnapshotApplier (Phase 2), kept separate so this stays testable in
// gravitaris-sim-test with no GL/rendering dependency.
class NetClient {
    INetTransport& m_transport;
    std::string m_name;

    bool m_connected = false;
    bool m_welcomed = false;
    std::uint32_t m_clientId = 0;
    std::uint32_t m_yourShipNetId = 0;
    std::uint32_t m_tickRate = 60;

    std::uint64_t m_lastAckedSnapshotTick = 0;
    std::uint32_t m_lastAckedEventSeq = 0;

    // Rolling window sent with every ClientInput (CLIENT_INPUT_BACKUP deep);
    // matches InputQueue's own "resend, let the far end dedupe" model.
    std::deque<InputCommand> m_recentCommands;

    std::optional<SnapshotData> m_latestSnapshot;

public:
    NetClient(INetTransport& transport, std::string name);

    // Polls the transport: sends ClientHello on the first Connected event,
    // decodes ServerWelcome/Snapshot packets. Call once per client frame,
    // before SendInput().
    void Update();

    // Appends a command to the resend window and sends a ClientInput packet,
    // targeting `lastAckedSnapshotTick + INPUT_LEAD_TICKS` -- there's no
    // local prediction or clock sync yet (Phase 5), so this is the client's
    // only estimate of "a tick the server hasn't simulated yet": InputSystem
    // drops any command with tick < the step it's currently processing (no
    // staleness tolerance, by design -- see its own comment), so stamping
    // with the client's own last-known tick instead would arrive
    // already-stale after crossing the network and get silently dropped
    // every time. +1 exactly matches LoopbackTransport's synchronous,
    // zero-latency round trip, but a real transport's RTT can span more than
    // one tick even on localhost (WebRtcTransport's own DTLS/SCTP handshake
    // and per-message scheduling) -- INPUT_LEAD_TICKS gives it slack; queued
    // commands with a future tick just wait in InputQueue until due, so
    // leading further than strictly needed costs nothing. No-ops before the
    // handshake completes (there's no ship to control yet).
    static constexpr std::uint64_t INPUT_LEAD_TICKS = 4;
    void SendInput(const ControlFlags& flags);

    [[nodiscard]] bool IsWelcomed() const { return m_welcomed; }
    [[nodiscard]] std::uint32_t GetYourShipNetId() const { return m_yourShipNetId; }
    [[nodiscard]] std::uint32_t GetTickRate() const { return m_tickRate; }

    // The most recently decoded snapshot, or nullopt if none has arrived yet.
    // Consuming (moving out / clearing) is the caller's responsibility --
    // this just always holds the latest.
    [[nodiscard]] const std::optional<SnapshotData>& GetLatestSnapshot() const { return m_latestSnapshot; }
};

} // namespace Gravitaris
