#include <algorithm>
#include <cfloat>

#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "net-panel.hpp"

namespace Gravitaris {

namespace {

// Same ring-buffer plotting idiom as perf-panel.cpp's DrawSectionRow, over
// CGame::RollingHistory instead of PerfMonitor::Section -- kept local rather
// than shared since the two aren't the same type.
void PlotHistory(const CGame::RollingHistory& history, const char* imguiId, const char* unit)
{
    if (history.sampleCount == 0) {
        ImGui::TextDisabled("no data yet");
        return;
    }
    const float last = history.samples[(history.writeIndex + CGame::RollingHistory::SIZE - 1) % CGame::RollingHistory::SIZE];
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
    ImGui::Text("Estimated server tick: %llu", static_cast<unsigned long long>(game.GetLastEstimatedServerTick()));
    ImGui::Text("Render tick (delayed): %llu", static_cast<unsigned long long>(game.GetLastRenderTick()));

    ImGui::SeparatorText("Connection health");
    ImGui::TextWrapped(
            "Snapshot interval spiking together with drift/resync = a real network gap. "
            "Drift/resync firing with snapshot interval flat = this client's own tab/thread "
            "stalled (GC, compositor hitch, OS power/thermal throttling) -- not the network.");

    ImGui::Text("Snapshots: %zu accepted, %zu dropped (out-of-order/resend)",
                game.GetAcceptedSnapshotCount(), game.GetDroppedSnapshotCount());
    ImGui::Text("Snapshot interval (ms)");
    PlotHistory(game.GetSnapshotIntervalHistory(), "##snapshot-interval", "ms");

    ImGui::Text("Predicted-tick drift/resync: %u events, last %llu ticks",
                game.GetResyncEventCount(), static_cast<unsigned long long>(game.GetLastResyncDriftTicks()));
    PlotHistory(game.GetDriftHistory(), "##drift", "ticks");

    ImGui::Text("Reconciliation correction magnitude");
    PlotHistory(game.GetCorrectionHistory(), "##correction", "units");
}

} // namespace Gravitaris
