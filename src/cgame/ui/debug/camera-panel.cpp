#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "camera-panel.hpp"

namespace Gravitaris {

void DrawCameraPanel(CGame& game)
{
    CGame::CameraParams& params = game.GetCameraParams();

    ImGui::Text("Current zoom: %.2f", game.GetCameraZoom());
    ImGui::SameLine();
    if (game.IsManualZoomActive()) {
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "(manual override)");
    } else {
        ImGui::TextDisabled("(dynamic)");
    }

    ImGui::SeparatorText("Zoom");
    ImGui::Checkbox("Dynamic zoom (speed-driven)", &params.dynamicZoom);
    ImGui::SetItemTooltip("Zoom out as the player speeds up, back in when slow.");

    ImGui::BeginDisabled(!params.dynamicZoom);
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloatRange2("Zoom range", &params.minZoom, &params.maxZoom, 0.01f, 0.1f, 10.f, "%.2f");
    ImGui::SetItemTooltip("min = fastest / framing (most zoomed out), max = at rest (most zoomed in). Default 0.5-5.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Speed falloff", &params.speedFalloff, 1.f, 20.f, 420.f, "%.0f");
    ImGui::SetItemTooltip("World units/sec at which the zoom noticeably backs off. Default 220.");
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Zoom smoothing (s)", &params.zoomTau, 0.1f, 3.9f, "%.2f");
    ImGui::SetItemTooltip("Time constant for easing toward the target zoom. Default 2.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Wheel hold (s)", &params.manualHold, 0.f, 10.f, "%.1f");
    ImGui::SetItemTooltip("Grace period after a wheel nudge before flying the ship cancels the manual zoom. Default 5.");

    ImGui::SeparatorText("Enemy framing");
    ImGui::Checkbox("Frame nearby enemy", &params.enemyFraming);
    ImGui::SetItemTooltip("Pan toward, and zoom out to include, the nearest enemy ship.");
    ImGui::BeginDisabled(!params.enemyFraming);
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Detect radius", &params.enemyRadius, 5.f, 100.f, 5000.f, "%.0f");
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Pan bias", &params.framingBias, 0.f, 1.f, "%.2f");
    ImGui::SetItemTooltip("0 = stay centered on player, 1 = center between player and enemy.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Fit margin", &params.framingMargin, 1.f, 0.f, 2000.f, "%.0f");
    ImGui::SetItemTooltip("Extra world units kept around the framed pair.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Framing smoothing (s)", &params.framingTau, 0.05f, 2.4f, "%.2f");
    ImGui::SetItemTooltip("Time constant for easing the pan/zoom-fit in and out as an enemy enters/leaves range.");
    ImGui::EndDisabled();
}

} // namespace Gravitaris
