#include <cstdint>
#include <vector>

#include <imgui.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/nav/trajectory-predictor.hpp>

#include <gravitaris/cgame/cgame.hpp>

#include "trajectory-panel.hpp"

namespace Gravitaris {

using Magnum::Vector2d;

// Shared between the tab (settings + readout) and the per-frame overlay.
namespace {

struct TrajectoryState {
    bool enabled = true;
    float horizonSeconds = 5.f;
    int drawStride = 2; // draw every Nth sample; prediction still runs per-tick

    // Live drift meter: a prediction captured at `baselineTick` is compared
    // against the ship's actual position as the sim catches up to it -- an
    // in-app stand-in for the headless drift test (docs/ai-ships.md phase 1).
    std::vector<Vector2d> baseline;
    std::uint64_t baselineTick = 0;
    double lastDrift = 0.0;
    double lastDriftAge = 0.0;
};

TrajectoryState s_state;

} // namespace

void DrawTrajectoryPanel(CGame& game)
{
    TrajectoryState& s = s_state;

    ImGui::SeparatorText("Predicted path (player)");
    ImGui::Checkbox("Draw predicted trajectory", &s.enabled);
    ImGui::SetItemTooltip("Test-particle forward integration against all non-bullet bodies.");

    ImGui::SetNextItemWidth(160.f);
    ImGui::SliderFloat("Horizon (s)", &s.horizonSeconds, 0.5f, 30.f, "%.1f");
    ImGui::SetNextItemWidth(160.f);
    ImGui::SliderInt("Draw stride", &s.drawStride, 1, 8);
    ImGui::SetItemTooltip("Draw every Nth sample. Prediction always runs at full tick rate.");

    ImGui::SeparatorText("Prediction vs. reality");
    if (s.baseline.empty()) {
        ImGui::TextDisabled("No baseline yet.");
    } else {
        // Drift resets whenever the baseline is re-captured (each horizon).
        ImGui::Text("Drift: %.3f units after %.2f s", s.lastDrift, s.lastDriftAge);
        ImGui::TextDisabled("Ballistic flight isolates integrator error;\nthrusting invalidates the baseline by design.");
    }
    if (ImGui::Button("Re-capture baseline")) {
        s.baseline.clear();
    }
}

void DrawTrajectoryOverlay(CGame& game, const Magnum::Vector2& uiSize)
{
    TrajectoryState& s = s_state;

    const std::optional<flecs::entity> maybePlayer = game.GetPlayer();
    if (!maybePlayer) return;

    const Transform* transf = maybePlayer->try_get<Transform>();
    if (!transf) return;

    const int steps = static_cast<int>(s.horizonSeconds / Game::PHYSICS_DELTA);
    std::vector<Vector2d> path = game.GetTrajectoryPredictor().Predict(*maybePlayer, steps, Game::PHYSICS_DELTA);
    if (path.empty()) return;

    // Drift meter bookkeeping: hold a captured prediction and compare the
    // actual position against it as ticks elapse; re-capture once exhausted.
    const std::uint64_t tick = game.GetStep();
    if (s.baseline.empty() || tick < s.baselineTick
            || tick - s.baselineTick >= s.baseline.size()) {
        s.baseline = path;
        s.baselineTick = tick;
    }
    const std::size_t age = static_cast<std::size_t>(tick - s.baselineTick);
    if (age < s.baseline.size()) {
        s.lastDrift = (transf->pos - s.baseline[age]).length();
        s.lastDriftAge = static_cast<double>(age) * Game::PHYSICS_DELTA;
    }

    if (!s.enabled) return;

    // World -> ImGui coordinates: camera-centered, ppu = zoom in framebuffer
    // pixels (see CGame::GetViewportSize), world +Y up vs. ImGui +Y down,
    // then framebuffer -> logical UI scale.
    const Camera& camera = game.GetCamera();
    const Magnum::Vector2 camPos = camera.GetPosition();
    const float ppu = camera.GetZoom();
    const Magnum::Vector2 fbToUi = uiSize / game.GetViewportSize();

    auto worldToUi = [&](const Vector2d& w) -> ImVec2 {
        const float dx = static_cast<float>(w.x()) - camPos.x();
        const float dy = static_cast<float>(w.y()) - camPos.y();
        return ImVec2(uiSize.x() * 0.5f + dx * ppu * fbToUi.x(),
                      uiSize.y() * 0.5f - dy * ppu * fbToUi.y());
    };

    std::vector<ImVec2> points;
    points.reserve(path.size() / static_cast<std::size_t>(s.drawStride) + 2);
    for (std::size_t i = 0; i < path.size(); i += static_cast<std::size_t>(s.drawStride)) {
        points.push_back(worldToUi(path[i]));
    }
    if ((path.size() - 1) % static_cast<std::size_t>(s.drawStride) != 0) {
        points.push_back(worldToUi(path.back())); // keep the true endpoint
    }

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->AddPolyline(points.data(), static_cast<int>(points.size()),
                          IM_COL32(80, 255, 190, 170), ImDrawFlags_None, 1.5f);
    // Mark the horizon end so the path visibly terminates rather than fading
    // into ambiguity.
    drawList->AddCircle(points.back(), 4.f, IM_COL32(80, 255, 190, 170), 0, 1.5f);
}

} // namespace Gravitaris
