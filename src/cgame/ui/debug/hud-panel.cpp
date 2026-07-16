#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "hud-panel.hpp"

namespace Gravitaris {

void DrawHudPanel(CGame& game)
{
    CGame::IndicatorParams& params = game.GetIndicatorParams();

    ImGui::SeparatorText("Off-screen target arrows");
    ImGui::Checkbox("Enabled", &params.enabled);
    ImGui::SetItemTooltip("Arrows around the screen center pointing at nearby enemies/planets that are off-screen.");

    ImGui::BeginDisabled(!params.enabled);

    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Ring radius (px)", &params.ringRadiusPx, 1.f, 20.f, 600.f, "%.0f");
    ImGui::SetItemTooltip("Distance from screen center to the arrow ring. Default 120.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Arrow size (px)", &params.arrowSizePx, 0.5f, 4.f, 120.f, "%.0f");
    ImGui::SetItemTooltip("Arrow width, and height at long range. Default 26.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Max height factor", &params.maxHeightFactor, 0.05f, 1.f, 8.f, "%.2f");
    ImGui::SetItemTooltip("How much taller (not wider) the arrow gets at point-blank range. "
                          "1 = no stretch. Default 2.5.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Height ramp rate", &params.heightRampFactor, 0.1f, 1.f, 20.f, "%.1f");
    ImGui::SetItemTooltip("Arrows stay flat over most of the range and only stretch tall within the "
                          "closest 1/rate fraction of it, instead of ramping linearly. Default 4.");

    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Enemy range", &params.enemyRange, 10.f, 100.f, 20000.f, "%.0f");
    ImGui::SetItemTooltip("World units: show enemies within this. Default 2500.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Planet range", &params.planetRange, 10.f, 100.f, 40000.f, "%.0f");
    ImGui::SetItemTooltip("World units: show planets within this. Default 6000.");

    ImGui::SetNextItemWidth(220.f);
    ImGui::DragInt("Max enemies", &params.maxEnemies, 0.1f, 0, 32);
    ImGui::SetItemTooltip("Cap on enemy arrows; the nearest win. Default 8.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragInt("Max planets", &params.maxPlanets, 0.1f, 0, 32);
    ImGui::SetItemTooltip("Cap on planet arrows; the nearest win. Default 4.");

    ImGui::SeparatorText("Fade");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Edge margin (px)", &params.edgeMarginPx, 0.5f, 0.f, 300.f, "%.0f");
    ImGui::SetItemTooltip("How far inside the view edge a target already counts as off-screen. Default 24.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Fade band (px)", &params.fadeBandPx, 1.f, 1.f, 400.f, "%.0f");
    ImGui::SetItemTooltip("Pixels past the edge over which an arrow fades fully in, so a target crossing the "
                          "view edge doesn't pop. Default 90.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Min strength", &params.minStrength, 0.f, 1.f, "%.2f");
    ImGui::SetItemTooltip("Size/brightness floor for a target at max range, so distant ones stay legible "
                          "instead of vanishing. Default 0.35.");

    ImGui::EndDisabled();
}

} // namespace Gravitaris
