#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/team.hpp>
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
    TeamId m_yourTeam = TeamId::Blue;
    // Sent with ClientHello; TeamId::None (the default) means "no
    // preference, auto-assign" -- see ClientHelloPacket::requestedTeam.
    // No round-setup UI exists yet to ever call SetRequestedTeam with
    // anything else.
    TeamId m_requestedTeam = TeamId::None;

    // See GetInputLeadTicks's own doc comment (declared further down, in the
    // public section -- this initializer is fine referencing it: default
    // member initializers are parsed in complete-class context).
    std::uint64_t m_inputLeadTicks = INPUT_LEAD_TICKS;

    std::uint64_t m_lastAckedSnapshotTick = 0;
    std::uint32_t m_lastAckedEventSeq = 0;
    // Wall-clock time the above tick was received, for EstimateCurrentServerTick.
    // Unset (nullopt) until the first snapshot arrives.
    std::optional<std::chrono::steady_clock::time_point> m_lastAckedSnapshotRecvTime;

    // Net debug tab diagnostics: wall-clock gap since the *previous* accepted
    // snapshot (not to be confused with m_lastAckedSnapshotRecvTime, which is
    // a point in time, not a duration). Distinguishes a real network gap
    // (this spikes) from a local main-thread stall (this stays regular while
    // CGame's predicted-tick drift/resync still fires) -- see cgame.cpp's
    // resync log and docs/networking-plan.md's Net debug tab section.
    float m_lastSnapshotIntervalMs = 0.f;
    std::size_t m_acceptedSnapshotCount = 0;
    // Snapshots rejected as older-than-or-equal-to the buffered latest (see
    // Update()'s out-of-order/resend guard) -- a genuinely unordered/lossy
    // transport shows up here, not as a gap in m_lastSnapshotIntervalMs.
    std::size_t m_droppedSnapshotCount = 0;

    // RTT probe (see PingPacket's own doc comment): self-scheduled inside
    // Update() rather than driven by the caller, so no cgame-side wiring is
    // needed to get a real measured number instead of guessing from
    // snapshot cadence. Send times keyed by seq, pruned on a stale timeout
    // (a lost Ping/Pong on this unreliable channel must not leak forever).
    static constexpr float PING_INTERVAL_SECONDS = 1.0f;
    static constexpr float PING_STALE_SECONDS = 5.0f;
    std::uint32_t m_nextPingSeq = 0;
    std::optional<std::chrono::steady_clock::time_point> m_lastPingSentTime;
    std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> m_pendingPings;
    float m_lastPingMs = -1.f; // -1 = no Pong observed yet
    // Exponential moving average, same smoothing idea as elsewhere in this
    // class -- one noisy sample shouldn't visibly jump the Net debug tab's
    // number every second.
    static constexpr float PING_EMA_ALPHA = 0.2f;
    float m_avgPingMs = -1.f;

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
    // the last acked snapshot using wall-clock elapsed time. Deliberately
    // doesn't use the Ping/Pong RTT measurement below -- that's real
    // round-trip time, but this needs the server's *tick*, and snapshot
    // arrival is what's actually stamped with one; mixing in a separately
    // -measured RTT to "help" would just be re-deriving the same estimate
    // through an extra noisy hop. `lastAckedSnapshotTick` alone goes stale
    // between snapshots (they arrive at the snapshot rate, not every frame), so a
    // fixed offset from it (the old INPUT_LEAD_TICKS scheme) routinely
    // undershoots once more than one snapshot interval has elapsed since the
    // last ack -- this is why input used to feel "laggy" under real RTT/
    // jitter: the stamped tick fell behind the server's actual current tick
    // and InputSystem silently dropped it as stale.
    [[nodiscard]] std::uint64_t EstimateCurrentServerTick() const;

    // Appends a command to the resend window and sends a ClientInput packet
    // stamped with `tick`. The caller supplies it explicitly (typically
    // `EstimateCurrentServerTick() + GetInputLeadTicks()`, but see
    // ClientPrediction: it needs the *exact* tick number it locally
    // predicted with -- re-deriving a fresh, possibly-jittery estimate here
    // instead would desync the two). The lead is slack for one-way trip +
    // jitter on top of the estimate: a queued command with a future tick
    // just waits in InputQueue until the server's own tick counter reaches
    // it -- InputSystem only ever applies an exact tick == step match (see
    // its own Update()), never early. In predict mode that wait is
    // invisible to the *owning* client (it always applies its own input
    // immediately via ClientPrediction, regardless of this stamped tick) --
    // only *other* clients watching this one's ship via snapshots see a
    // marginally later reaction. Without prediction (the no-client-
    // prediction branch) that assumption breaks: the wait becomes real,
    // directly felt input lag for the owning client too, since nothing
    // local shows the result any sooner. No-ops before the handshake
    // completes (there's no ship to control yet).
    void SendInput(std::uint64_t tick, const ControlFlags& flags);

    // Default/fallback lead (see GetInputLeadTicks) -- 8 (133ms @ 60Hz).
    //
    // 2 (33ms) was confirmed too tight from a real two-peer LAN session's
    // server logs (2026-07-19): "input arrived N ticks late" fired
    // recurringly, every few seconds, with N in [1,3] -- not occasional
    // large drift, just ordinary jitter routinely exceeding a 2-tick lead.
    // Each occurrence is one stale-dropped command -> InputSystem repeats
    // the last-consumed flags for that tick -> the server's real path
    // diverges from what ClientPrediction predicted -> a reconciliation
    // snap next snapshot. That recurring-every-few-seconds cadence matches
    // "still jitters often" exactly. Raised past the observed max with
    // headroom for jitter this short a sample didn't happen to catch --
    // tuned for *predicted* peers, where the cost of leading further than
    // strictly needed is invisible (see SendInput's own doc comment).
    static constexpr std::uint64_t INPUT_LEAD_TICKS = 8;

    // Runtime-adjustable version of the same knob (defaults to
    // INPUT_LEAD_TICKS): predict-mode peers rarely need to touch this, but a
    // no-predict peer feels every tick of it directly as input lag, so it's
    // worth dialing down to the smallest value real measured RTT/jitter
    // (GetLastPingMs/GetAveragePingMs) actually needs -- too low reintroduces
    // the stale-drop/repeat-last-command jitter the 8-tick default was raised
    // to fix.
    [[nodiscard]] std::uint64_t GetInputLeadTicks() const { return m_inputLeadTicks; }
    void SetInputLeadTicks(std::uint64_t ticks) { m_inputLeadTicks = ticks; }

    // Must be called before the handshake fires (i.e. before the first
    // Update() that observes a Connected event) to take effect -- ClientHello
    // is built and sent the instant that event arrives.
    void SetRequestedTeam(TeamId team) { m_requestedTeam = team; }

    [[nodiscard]] bool IsWelcomed() const { return m_welcomed; }
    [[nodiscard]] std::uint32_t GetYourShipNetId() const { return m_yourShipNetId; }
    [[nodiscard]] std::uint32_t GetTickRate() const { return m_tickRate; }
    [[nodiscard]] TeamId GetYourTeam() const { return m_yourTeam; }

    // The most recently decoded snapshot, or nullopt if none has arrived yet.
    [[nodiscard]] const std::optional<SnapshotData>& GetLatestSnapshot() const { return m_latestSnapshot; }

    // Strictly tick-ascending buffer of every accepted snapshot still held
    // (up to SNAPSHOT_HISTORY_CAPACITY deep), for SnapshotInterpolator
    // (cgame/net/) to render a delayed, interpolated position from instead
    // of snapping to the latest one every frame.
    [[nodiscard]] const std::deque<SnapshotData>& GetSnapshotHistory() const { return m_snapshotHistory; }

    // Net debug tab diagnostics (see the fields' own doc comments).
    [[nodiscard]] float GetLastSnapshotIntervalMs() const { return m_lastSnapshotIntervalMs; }
    [[nodiscard]] std::size_t GetAcceptedSnapshotCount() const { return m_acceptedSnapshotCount; }
    [[nodiscard]] std::size_t GetDroppedSnapshotCount() const { return m_droppedSnapshotCount; }

    // Real measured round-trip time (PingPacket's own doc comment) -- not an
    // estimate derived from snapshot/tick cadence. -1 until the first Pong
    // arrives. GetLastPingMs is the single most recent sample (noisier);
    // GetAveragePingMs is the smoothed EMA (what the Net debug tab should
    // show by default).
    [[nodiscard]] float GetLastPingMs() const { return m_lastPingMs; }
    [[nodiscard]] float GetAveragePingMs() const { return m_avgPingMs; }

private:
    void SendPing();
};

} // namespace Gravitaris
