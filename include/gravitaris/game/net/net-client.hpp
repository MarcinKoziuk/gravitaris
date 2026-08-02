#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/input/input-command.hpp>
#include <gravitaris/game/net/protocol.hpp>
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
    std::uint32_t m_sectorSeed = 0;
    // Sent with ClientHello; TeamId::None (the default) means "no
    // preference, auto-assign" -- see ClientHelloPacket::requestedTeam. The
    // client's intro dialog fills this in via CGame::ConnectToServer.
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
    // EMA of |sample - average|, i.e. how much a single round trip typically
    // deviates from the smoothed one. The lead has to cover the unlucky
    // packet, not the average one, so this is sized in explicitly rather than
    // hidden inside a blanket multiplier on the average.
    float m_pingJitterMs = -1.f;

    // Auto-sizing of the input lead from those two measurements (see
    // ComputeInputLeadTicks). On by default: the fixed INPUT_LEAD_TICKS
    // fallback is sized for an unknown wire, and every tick of it that real
    // latency doesn't need is a tick of skew between where this client draws
    // its own ship and where every other client draws it.
    bool m_inputLeadAuto = true;
    // Raising the lead is urgent (under-covered = commands dropped as
    // stale); lowering it is not, and doing it eagerly would chase jitter.
    // So: up immediately, down one tick per this many seconds of the
    // suggestion staying below the current value.
    static constexpr double LEAD_RELAX_SECONDS = 3.0;
    std::optional<std::chrono::steady_clock::time_point> m_leadRelaxSince;

    // Smoothed, fractional, monotonic server-clock estimate for *rendering*
    // (EstimateCurrentServerTickF). The raw arrival-based estimate is a
    // staircase that only steps when a packet lands, so feeding it to
    // SnapshotInterpolator hands raw network arrival jitter straight to the
    // screen: a late snapshot re-bases the estimate backwards and remote
    // entities visibly rewind a whole tick of travel, worse the faster
    // they're moving. This recovers a continuous clock from it instead --
    // free-run at real time, closed back onto the raw estimate by warping
    // the rate within +/-CLOCK_MAX_RATE_ADJUST (never by jumping, which is
    // what keeps it monotonic).
    static constexpr double CLOCK_SMOOTHING_GAIN = 0.1;   // rate warp per tick of error
    static constexpr double CLOCK_MAX_RATE_ADJUST = 0.25; // < 1, or the clock could run backwards
    // Past this the error is a real desync (a stall, a long gap in
    // snapshots), not jitter to be smoothed out -- warping the rate would
    // take seconds to close it, so snap and accept the one-time jump.
    static constexpr double CLOCK_SNAP_TICKS = 10.0;
    double m_smoothedServerTick = 0.0;
    bool m_serverClockStarted = false;
    std::optional<std::chrono::steady_clock::time_point> m_lastServerClockUpdate;
    std::size_t m_clockSnapCount = 0;

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

    // Chat lines received since the caller last drained them. Bounded like
    // every other receive buffer here: a client that stops draining (paused
    // in a debugger, say) must not grow this without limit.
    static constexpr std::size_t CHAT_INBOX_CAPACITY = 32;
    std::deque<ChatMessagePacket> m_chatInbox;

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

    // The same estimate, but continuous and monotonic: fractional ticks, no
    // backward steps, advancing every frame rather than only when a packet
    // lands (see m_smoothedServerTick). This is what the *render* path must
    // use -- SnapshotInterpolator can only lerp between two snapshots if the
    // tick it's asked for actually falls between them, and consecutive
    // snapshots are one tick apart, so a whole-tick render clock always
    // lands exactly on one and its lerp fraction is always 0.
    //
    // Prediction deliberately keeps using the integer estimate above: it
    // needs the exact tick numbers it stamps input with, and PredictedTickClock
    // already does its own (different) filtering on top.
    //
    // Updated inside Update(), so it's only as fresh as the last call to it.
    [[nodiscard]] double EstimateCurrentServerTickF() const { return m_smoothedServerTick; }

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
    void SendInput(std::uint64_t tick, const ControlFlags& flags, UpgradePick upgradePick = 0);

    // Sends one composed chat line, reliably. No-op before the handshake
    // completes -- the server can't attribute a line to a peer it hasn't
    // welcomed, so it would drop it anyway.
    void SendChat(const std::string& text);

    // Everything received since the last call, oldest first; the caller owns
    // them from here (this is what keeps the inbox from growing).
    [[nodiscard]] std::vector<ChatMessagePacket> TakeChatMessages();

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
    // Manual override: also turns auto-sizing off, or the next Update()
    // would immediately undo whatever was just set.
    void SetInputLeadTicks(std::uint64_t ticks)
    {
        m_inputLeadTicks = ticks;
        m_inputLeadAuto = false;
    }

    // Never below this even on a zero-latency loopback: one tick of slack for
    // the send/receive landing either side of a tick boundary, and the
    // interval between the client's own frames is itself ~a tick.
    static constexpr std::uint64_t MIN_INPUT_LEAD_TICKS = 2;
    static constexpr std::uint64_t MAX_INPUT_LEAD_TICKS = 180;

    // The lead a given measurement calls for, in ticks. Pure and static so
    // the sizing rule is testable without a live connection.
    //
    // Sized off the *full* round trip, not half of it, and that's not
    // conservatism -- it's what the arithmetic requires. This client stamps
    // commands `lead` ahead of its own server-tick estimate, and that
    // estimate is itself derived from snapshot arrivals, so it already lags
    // the server's true tick by one one-way trip. A command sent now reaches
    // the server one further one-way trip later. Cover both or it lands in
    // the server's past:
    //
    //     stamp = (true - owd) + lead   must be >=   true + owd
    //     =>  lead >= 2 * owd  =  rtt
    //
    // Which also means the own ship necessarily runs one one-way trip ahead
    // of the server's true state, and no tuning removes that -- it's the
    // floor this whole scheme sits on.
    static std::uint64_t ComputeInputLeadTicks(float rttMs, float jitterMs, std::uint32_t tickRate);

    // What ComputeInputLeadTicks makes of the current measurements, or the
    // INPUT_LEAD_TICKS fallback while no Pong has arrived yet.
    [[nodiscard]] std::uint64_t GetSuggestedInputLeadTicks() const;

    [[nodiscard]] bool IsInputLeadAuto() const { return m_inputLeadAuto; }
    void SetInputLeadAuto(bool on)
    {
        m_inputLeadAuto = on;
        m_leadRelaxSince.reset();
    }

    // Must be called before the handshake fires (i.e. before the first
    // Update() that observes a Connected event) to take effect -- ClientHello
    // is built and sent the instant that event arrives.
    void SetRequestedTeam(TeamId team) { m_requestedTeam = team; }

    [[nodiscard]] bool IsWelcomed() const { return m_welcomed; }
    [[nodiscard]] std::uint32_t GetYourShipNetId() const { return m_yourShipNetId; }
    [[nodiscard]] std::uint32_t GetTickRate() const { return m_tickRate; }
    [[nodiscard]] TeamId GetYourTeam() const { return m_yourTeam; }

    // The seed the server's sector was generated from, 0 until welcomed.
    [[nodiscard]] std::uint32_t GetSectorSeed() const { return m_sectorSeed; }

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
    // Times the render clock had to snap rather than warp (see
    // CLOCK_SNAP_TICKS) -- each one is a visible one-time jump in every
    // remote entity's position.
    [[nodiscard]] std::size_t GetClockSnapCount() const { return m_clockSnapCount; }

    // Real measured round-trip time (PingPacket's own doc comment) -- not an
    // estimate derived from snapshot/tick cadence. -1 until the first Pong
    // arrives. GetLastPingMs is the single most recent sample (noisier);
    // GetAveragePingMs is the smoothed EMA (what the Net debug tab should
    // show by default).
    [[nodiscard]] float GetLastPingMs() const { return m_lastPingMs; }
    [[nodiscard]] float GetAveragePingMs() const { return m_avgPingMs; }
    [[nodiscard]] float GetPingJitterMs() const { return m_pingJitterMs; }

private:
    void SendPing();
    void UpdateServerClock();
    void UpdateInputLead();
};

} // namespace Gravitaris
