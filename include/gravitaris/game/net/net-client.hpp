#pragma once

#include <chrono>
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
    // Wall-clock time the above tick was received, for EstimateCurrentServerTick.
    // Unset (nullopt) until the first snapshot arrives.
    std::optional<std::chrono::steady_clock::time_point> m_lastAckedSnapshotRecvTime;

    // Rolling window sent with every ClientInput (CLIENT_INPUT_BACKUP deep);
    // matches InputQueue's own "resend, let the far end dedupe" model.
    std::deque<InputCommand> m_recentCommands;

    // Bounded, strictly tick-ascending buffer for Phase 4 interpolation
    // (docs/networking-plan.md): the data channel is unordered, so a
    // snapshot can arrive after a newer one -- anything not strictly newer
    // than the current back() is dropped rather than appended, which is
    // also what keeps this (and m_lastAckedSnapshotTick) monotonic. Capacity
    // is generous headroom for jitter/reordering, not a tuned interpolation
    // delay (that's SnapshotInterpolator's job, cgame-side).
    static constexpr std::size_t SNAPSHOT_HISTORY_CAPACITY = 32;
    std::deque<SnapshotData> m_snapshotHistory;
    // Mirrors m_snapshotHistory.back() for GetLatestSnapshot()'s reference
    // -returning API: `*GetLatestSnapshot()` binding to a subobject of a
    // by-value std::optional<SnapshotData> return does NOT lifetime-extend
    // the optional through the operator*() call boundary (only a *direct*
    // reference binding to a temporary does) -- that dangled every existing
    // `const SnapshotData& s = *client.GetLatestSnapshot();` call site.
    // Keeping this as a real member sidesteps the trap entirely.
    std::optional<SnapshotData> m_latestSnapshot;

public:
    NetClient(INetTransport& transport, std::string name);

    // Polls the transport: sends ClientHello on the first Connected event,
    // decodes ServerWelcome/Snapshot packets. Call once per client frame,
    // before SendInput().
    void Update();

    // Estimates the server's current sim tick by extrapolating forward from
    // the last acked snapshot using wall-clock elapsed time (snapshot arrival
    // is the only clock reference this client has -- no ping/clock-sync
    // packet exists yet). `lastAckedSnapshotTick` alone goes stale between
    // snapshots (they arrive at the snapshot rate, not every frame), so a
    // fixed offset from it (the old INPUT_LEAD_TICKS scheme) routinely
    // undershoots once more than one snapshot interval has elapsed since the
    // last ack -- this is why input used to feel "laggy" under real RTT/
    // jitter: the stamped tick fell behind the server's actual current tick
    // and InputSystem silently dropped it as stale.
    [[nodiscard]] std::uint64_t EstimateCurrentServerTick() const;

    // Appends a command to the resend window and sends a ClientInput packet,
    // targeting `EstimateCurrentServerTick() + INPUT_LEAD_TICKS`.
    // INPUT_LEAD_TICKS is now just slack for one-way trip + jitter on top of
    // the extrapolated estimate (rather than the estimate itself, as before):
    // queued commands with a future tick just wait in InputQueue until due,
    // so leading further than strictly needed costs nothing. No-ops before
    // the handshake completes (there's no ship to control yet).
    static constexpr std::uint64_t INPUT_LEAD_TICKS = 2;
    void SendInput(const ControlFlags& flags);

    [[nodiscard]] bool IsWelcomed() const { return m_welcomed; }
    [[nodiscard]] std::uint32_t GetYourShipNetId() const { return m_yourShipNetId; }
    [[nodiscard]] std::uint32_t GetTickRate() const { return m_tickRate; }

    // The most recently decoded snapshot, or nullopt if none has arrived yet.
    [[nodiscard]] const std::optional<SnapshotData>& GetLatestSnapshot() const { return m_latestSnapshot; }

    // Strictly tick-ascending buffer of every accepted snapshot still held
    // (up to SNAPSHOT_HISTORY_CAPACITY deep), for SnapshotInterpolator
    // (cgame/net/) to render a delayed, interpolated position from instead
    // of snapping to the latest one every frame.
    [[nodiscard]] const std::deque<SnapshotData>& GetSnapshotHistory() const { return m_snapshotHistory; }
};

} // namespace Gravitaris
