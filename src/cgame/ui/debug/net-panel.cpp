#include <algorithm>
#include <cfloat>
#include <cmath>

#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "net-panel.hpp"

namespace Gravitaris {

namespace {

// Same ring-buffer plotting idiom as perf-panel.cpp's DrawSectionRow, over
// RollingHistory instead of PerfMonitor::Section -- kept local rather than
// shared since the two aren't the same type.
void PlotHistory(const RollingHistory& history, const char* imguiId, const char* unit)
{
    if (history.sampleCount == 0) {
        ImGui::TextDisabled("no data yet");
        return;
    }
    const float last = history.samples[(history.writeIndex + RollingHistory::SIZE - 1) % RollingHistory::SIZE];
    float maxVal = 0.f;
    for (std::size_t i = 0; i < history.sampleCount; ++i) maxVal = std::max(maxVal, history.samples[i]);
    ImGui::Text("last %.1f %s (max %.1f %s over last %zu events)", last, unit, maxVal, unit, history.sampleCount);
    ImGui::PlotLines(imguiId, history.samples.data(), static_cast<int>(history.sampleCount),
                      static_cast<int>(history.writeIndex), nullptr, 0.f, FLT_MAX, ImVec2(0.f, 40.f));
}

} // namespace

void DrawNetPanel(CGame& game)
{
    if (!game.IsNetClient()) {
        ImGui::TextDisabled("Not connected to a server (single-player) -- nothing to tune here.");
        return;
    }

    ImGui::SeparatorText("Connection");
    if (game.GetAveragePingMs() < 0.f) {
        ImGui::TextDisabled("Ping: measuring... (first Pong not received yet)");
    }
    else {
        ImGui::Text("Ping (RTT): %.1f ms avg, %.1f ms last, %.1f ms jitter", game.GetAveragePingMs(),
                    game.GetLastPingMs(), game.GetPingJitterMs());
    }
    ImGui::SetItemTooltip("Real measured round-trip time (dedicated Ping/Pong probe, ~1/s), not "
                          "derived from snapshot cadence. Use this to tell whether perceived input "
                          "lag is real network/transport latency or comes from tuned budgets "
                          "(interpolation delay, INPUT_LEAD_TICKS) instead.");

    // 180 ticks (3s) rather than the old 20 (333ms): must comfortably cover
    // the simulated-delay slider below, whose one-way max (1000ms) implies a
    // round trip of ~2s once jitter/loss retries are folded in -- 20 ticks
    // capped this well short of that, so cranking simulated delay past
    // ~300ms made every command permanently arrive stale no matter how high
    // this was dragged: the ship goes fully uncontrollable, not a bug in the
    // simulated transport, just this slider's ceiling being too low to
    // compensate for it (see PacketType::ClientInput's own doc comment on
    // net-server.cpp -- a late command isn't merely delayed, it's silently
    // discarded unapplied, forever, the instant InputSystem's next pass
    // sees `tick < step`).
    bool leadAuto = game.IsInputLeadAuto();
    if (ImGui::Checkbox("Auto input lead (from measured ping/jitter)", &leadAuto)) {
        game.SetInputLeadAuto(leadAuto);
    }
    ImGui::SetItemTooltip("Sizes the lead to the connection instead of a fixed worst-case guess. "
                          "Every tick of lead beyond what latency needs is a tick of skew between "
                          "where you draw your own ship and where everyone else draws it. Raises "
                          "immediately, lowers a tick at a time. Moving the slider takes over "
                          "manually.");

    int leadTicks = static_cast<int>(game.GetInputLeadTicks());
    ImGui::BeginDisabled(leadAuto);
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderInt("Input lead (ticks)", &leadTicks, 0, 180)) {
        game.SetInputLeadTicks(static_cast<std::uint64_t>(std::max(leadTicks, 0)));
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("How far ahead of the estimated server tick this client's own input is "
                          "stamped -- a command waits in the server's queue until its stamped tick "
                          "arrives, so this is a real floor on how soon it can possibly take effect. "
                          "Too low: input arrives 'late' and is discarded unapplied, repeating the "
                          "last held command until one finally lands in time -- at the extreme, full "
                          "loss of control. Use the suggestion below (from real measured ping) rather "
                          "than guessing.");

    if (game.GetAveragePingMs() >= 0.f) {
        const auto suggested = static_cast<int>(game.GetSuggestedInputLeadTicks());
        ImGui::Text("Measurement calls for: %d ticks (rtt + 2x jitter + 1 tick)", suggested);
        if (!leadAuto) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply##lead")) {
                game.SetInputLeadTicks(static_cast<std::uint64_t>(std::max(suggested, 0)));
            }
        }
    }

    if (SimulatedNetTransport::Params* sim = game.GetSimulatedNetParams()) {
        ImGui::SeparatorText("Simulated network conditions");
        ImGui::TextWrapped("Raising Delay here pushes real measured Ping up too (Ping travels the same "
                           "simulated path) -- re-check the Input lead suggestion above after changing "
                           "it, or every command starts arriving permanently late and the ship stops "
                           "responding entirely.");
        ImGui::TextWrapped("Chrome DevTools' network throttling doesn't touch WebRTC data channels at "
                           "all, so it's a no-op once in-game -- this dials artificial lag/loss in "
                           "directly instead, live, no OS tool needed.");

        ImGui::SetNextItemWidth(160.f);
        ImGui::SliderFloat("Delay (ms, one-way)", &sim->delayMs, 0.f, 1000.f, "%.0f");
        ImGui::SetItemTooltip("Added once per direction (Send and the reply's Poll each pay it), so a "
                              "round trip feels roughly double this on top of the real network's own.");

        ImGui::SetNextItemWidth(160.f);
        ImGui::SliderFloat("Jitter (ms, +/-)", &sim->jitterMs, 0.f, 500.f, "%.0f");

        ImGui::SetNextItemWidth(160.f);
        ImGui::SliderFloat("Packet loss (%)", &sim->lossPercent, 0.f, 100.f, "%.0f");
        ImGui::SetItemTooltip("Independent chance to silently drop each packet -- Send and incoming "
                              "Poll events both roll separately, so this applies per direction too.");
    }

    ImGui::SeparatorText("Interpolation (docs/networking-plan.md Phase 4)");

    float delayMs = game.GetInterpDelaySeconds() * 1000.f;
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Delay (ms)", &delayMs, 0.f, 300.f, "%.0f")) {
        game.SetInterpDelaySeconds(delayMs / 1000.f);
    }
    ImGui::SetItemTooltip("How far behind the estimated server tick remote entities render. "
                          "Higher smooths jitter at the cost of latency.");

    float capMs = game.GetInterpParams().extrapolationCapSeconds * 1000.f;
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Extrapolation cap (ms)", &capMs, 0.f, 150.f, "%.0f")) {
        game.GetInterpParams().extrapolationCapSeconds = capMs / 1000.f;
    }
    ImGui::SetItemTooltip("How far past the newest received snapshot entities are allowed to "
                          "extrapolate (via velocity) before snapping to it instead.");

    ImGui::SeparatorText("Prediction (docs/networking-plan.md Phase 5)");

    float epsilon = static_cast<float>(game.GetPredictionEpsilon());
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Reconcile epsilon (world units)", &epsilon, 0.f, 50.f, "%.1f")) {
        game.SetPredictionEpsilon(epsilon);
    }
    ImGui::SetItemTooltip("Position error past which the own ship snaps to the server's "
                          "authoritative state and replays pending input. Too low corrects on "
                          "ordinary prediction noise (visible as camera jitter); too high lets "
                          "real desyncs linger uncorrected.");

    ImGui::SeparatorText("Diagnostics");
    ImGui::Text("Snapshot history: %zu buffered", game.GetSnapshotHistorySize());
    ImGui::Text("Estimated server tick (input path): %llu",
                static_cast<unsigned long long>(game.GetLastEstimatedServerTick()));
    ImGui::Text("Render tick (smoothed, delayed): %.2f", game.GetLastRenderTick());
    ImGui::Text("Render-clock snaps: %zu", game.GetClockSnapCount());
    ImGui::SetItemTooltip("Times the smoothed render clock had to jump instead of easing back into sync. "
                          "Climbing steadily = every remote entity is jumping with it.");

    ImGui::SeparatorText("Connection health");
    ImGui::TextWrapped(
            "Snapshot interval spiking together with drift/resync = a real network gap. "
            "Drift/resync firing with snapshot interval flat = this client's own tab/thread "
            "stalled (GC, compositor hitch, OS power/thermal throttling) -- not the network.");

    const NetDiagnostics& diag = game.GetNetDiagnostics();

    ImGui::Text("Snapshots: %zu accepted, %zu dropped (out-of-order/resend)",
                game.GetAcceptedSnapshotCount(), game.GetDroppedSnapshotCount());
    ImGui::Text("Snapshot interval (ms)");
    PlotHistory(diag.snapshotIntervalHistory, "##snapshot-interval", "ms");

    ImGui::Text("Predicted-tick drift/resync: %u events, last %llu ticks",
                diag.resyncEventCount, static_cast<unsigned long long>(diag.lastResyncDriftTicks));
    ImGui::Text("Ticks given back (lead lowered): %u", diag.tickSkipCount);
    ImGui::SetItemTooltip("One skipped own-ship tick each, spent walking the predicted clock down onto "
                          "a reduced input lead. A burst after connecting is the lead converging; a "
                          "steady trickle means it never settles.");
    PlotHistory(diag.driftHistory, "##drift", "ticks");

    ImGui::Text("Reconciliation correction magnitude");
    PlotHistory(diag.correctionHistory, "##correction", "units");
}

} // namespace Gravitaris
