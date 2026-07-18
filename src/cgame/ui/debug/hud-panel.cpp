#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "hud-panel.hpp"

namespace Gravitaris {

void DrawHudPanel(CGame& game)
{
    CGame::IndicatorParams& params = game.GetIndicatorParams();

    ImGui::SeparatorText("Off-screen target arrows");
    ImGui::Checkbox("Enabled", &params.enabled);
    ImGui::SetItemTooltip("Arrows around the screen center pointing at nearby enemies that are off-screen.");

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
    ImGui::DragInt("Max enemies", &params.maxEnemies, 0.1f, 0, 32);
    ImGui::SetItemTooltip("Cap on enemy arrows; the nearest win. Default 8.");

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

    MinimapRenderer::Params& minimap = game.GetMinimapRenderer().GetParams();

    ImGui::SeparatorText("Minimap");
    ImGui::Checkbox("Minimap enabled", &minimap.enabled);
    ImGui::SetItemTooltip("Blank panel when off; hide/restyle the panel itself in ui/hud.rml.");

    ImGui::BeginDisabled(!minimap.enabled);
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("World radius", &minimap.worldRadius, 25.f, 500.f, 30000.f, "%.0f");
    ImGui::SetItemTooltip("World units from the player to the map edge. Default 10000.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Ship dot (px)", &minimap.shipDotPx, 0.1f, 1.f, 12.f, "%.1f");
    ImGui::SetItemTooltip("Ship dot radius in minimap texture pixels. Default 3.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Player dot (px)", &minimap.playerDotPx, 0.1f, 0.5f, 12.f, "%.1f");
    ImGui::SetItemTooltip("Player marker dot radius in minimap texture pixels. Default 1.5.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Planet min (px)", &minimap.planetMinPx, 0.1f, 1.f, 24.f, "%.1f");
    ImGui::SetItemTooltip("Smallest ring a planet can shrink to; real world radius is used when it maps "
                          "bigger than this. Default 4.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Sun min (px)", &minimap.starMinPx, 0.1f, 1.f, 32.f, "%.1f");
    ImGui::SetItemTooltip("Smallest ring a sun can shrink to; real world radius is used when it maps "
                          "bigger than this. Default 7.");
    ImGui::Checkbox("Show view rectangle", &minimap.showViewRect);
    ImGui::SetItemTooltip("Outline the main camera's visible extent on the map.");
    ImGui::EndDisabled();
}

} // namespace Gravitaris
