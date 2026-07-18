#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "camera-panel.hpp"

namespace Gravitaris {

void DrawCameraPanel(CGame& game)
{
    CameraDirector::CameraParams& params = game.GetCameraParams();

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
    ImGui::SetItemTooltip("Time constant for easing toward the dynamic (speed/framing) zoom target. Default 2.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Wheel zoom smoothing (s)", &params.manualZoomTau, 0.02f, 2.f, "%.2f");
    ImGui::SetItemTooltip("Time constant for easing toward a wheel-driven zoom target. Lower = scroll reads "
                          "more immediately. Default 0.25.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Wheel sensitivity", &params.scrollSensitivity, 1.01f, 1.3f, "%.3f");
    ImGui::SetItemTooltip("Zoom multiplier per wheel notch. Default 1.08.");
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

    ImGui::SeparatorText("Planet framing");
    ImGui::Checkbox("Zoom out for nearby planets", &params.planetFraming);
    ImGui::SetItemTooltip("Zoom out to fit a planet/sun while approaching it, then release near the surface "
                          "so the final approach zooms back in.");
    ImGui::BeginDisabled(!params.planetFraming);
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Framing range", &params.planetFramingRange, 20.f, 100.f, 8000.f, "%.0f");
    ImGui::SetItemTooltip("Surface distance at which the zoom-out starts fading in. Default 2000.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Release distance", &params.planetReleaseDist, 5.f, 20.f, 1500.f, "%.0f");
    ImGui::SetItemTooltip("Surface distance below which framing releases, handing back to the normal "
                          "speed-driven zoom-in for landing. Default 90.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Fit margin##planet", &params.planetFramingMargin, 5.f, 0.f, 3000.f, "%.0f");
    ImGui::SetItemTooltip("Extra world units kept around the fitted body. Default 350.");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Close-combat zoom");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Close range", &params.closeZoomRange, 5.f, 20.f, 2000.f, "%.0f");
    ImGui::SetItemTooltip("Enemy distance under which the zoom-in ramps toward Close fraction. Default 320.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Close fraction", &params.closeZoomFraction, 0.1f, 1.f, "%.2f");
    ImGui::SetItemTooltip("Fraction of max zoom reached at point-blank range -- 1 would be a full snap-in. "
                          "Default 0.7.");
}

} // namespace Gravitaris
