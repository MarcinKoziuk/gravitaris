#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "hud-panel.hpp"

namespace Gravitaris {

void DrawHudPanel(CGame& game)
{
    IndicatorRenderer::Params& params = game.GetIndicatorParams();

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
    ImGui::Checkbox("Auto-fit", &minimap.autoFit);
    ImGui::SetItemTooltip("Grow the radius to cover the whole sector. Clear it to set one by hand.");
    ImGui::BeginDisabled(minimap.autoFit);
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("World radius", &minimap.worldRadius, 25.f, 500.f, 60000.f, "%.0f");
    ImGui::SetItemTooltip("World units from the map center to the map edge. Default 12000.");
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Ship tri (px)", &minimap.shipTriPx, 0.1f, 1.f, 12.f, "%.1f");
    ImGui::SetItemTooltip("Ship triangle circumradius in minimap texture pixels. Default 3.5.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Freighter tri (px)", &minimap.freighterTriPx, 0.1f, 1.f, 12.f, "%.1f");
    ImGui::SetItemTooltip("Freighter triangle circumradius in minimap texture pixels. Default 2.5.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Player dot (px)", &minimap.playerDotPx, 0.1f, 0.5f, 12.f, "%.1f");
    ImGui::SetItemTooltip("Player marker dot radius in minimap texture pixels. Default 3.");
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

    CompassRenderer::Params& compass = game.GetCompassRenderer().GetParams();

    ImGui::SeparatorText("Compass");
    ImGui::Checkbox("Compass enabled", &compass.enabled);
    ImGui::SetItemTooltip("Blank panel when off; hide/restyle the panel itself in ui/hud.rml.");

    ImGui::BeginDisabled(!compass.enabled);
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Ship fit", &compass.shipFit, 0.2f, 0.95f, "%.2f");
    ImGui::SetItemTooltip("Ship's bounding radius in the dial's -1..1 space; the ring sits at 0.8. "
                          "Default 0.6.");
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Stroke width", &compass.lineWidth, 1.f, 12.f, "%.1f");
    ImGui::SetItemTooltip("Stroke width in texture pixels, before the panel downsamples the 128px dial to "
                          "its on-screen size (~3x). Default 5.");

    ImGui::Checkbox("Velocity marker", &compass.showVelocity);
    ImGui::SetItemTooltip("Amber dot on the ring showing which way the ship is actually travelling, as "
                          "opposed to which way it points.");
    ImGui::BeginDisabled(!compass.showVelocity);
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Min speed", &compass.minSpeed, 0.5f, 0.f, 200.f, "%.0f");
    ImGui::SetItemTooltip("Speed below which the velocity marker is dropped -- the direction of a near-zero "
                          "vector is noise. Default 8.");
    ImGui::EndDisabled();

    ImGui::Checkbox("Gravity marker", &compass.showGravity);
    ImGui::SetItemTooltip("Red dot on the ring showing which way the field pulls -- 'down', for a sector "
                          "that has no one down. Off by default.");
    ImGui::BeginDisabled(!compass.showGravity);
    ImGui::SetNextItemWidth(220.f);
    ImGui::DragFloat("Min gravity", &compass.minGravity, 0.05f, 0.f, 20.f, "%.2f");
    ImGui::SetItemTooltip("Field strength below which the gravity marker is dropped. Default 0.4.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

} // namespace Gravitaris
