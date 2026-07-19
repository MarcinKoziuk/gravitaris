#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "net-panel.hpp"

namespace Gravitaris {

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

    ImGui::SeparatorText("Diagnostics");
    ImGui::Text("Snapshot history: %zu buffered", game.GetSnapshotHistorySize());
    ImGui::Text("Estimated server tick: %llu", static_cast<unsigned long long>(game.GetLastEstimatedServerTick()));
    ImGui::Text("Render tick (delayed): %llu", static_cast<unsigned long long>(game.GetLastRenderTick()));
}

} // namespace Gravitaris
