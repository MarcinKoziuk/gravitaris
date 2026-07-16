#include <string>

#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>
#include <gravitaris/cgame/renderer/starfield-renderer.hpp>

#include "starfield-panel.hpp"

namespace Gravitaris {

void DrawStarfieldPanel(CGame& game)
{
    StarfieldRenderer& starfield = game.GetStarfieldRenderer();

    bool enabled = starfield.IsEnabled();
    if (ImGui::Checkbox("Enabled", &enabled)) {
        starfield.SetEnabled(enabled);
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%d stars)", starfield.GetLastStarCount());

    ImGui::BeginDisabled(!enabled);

    float cellSize = starfield.GetCellSize();
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::DragFloat("Cell size", &cellSize, 1.f, 16.f, 512.f, "%.0f")) {
        starfield.SetCellSize(cellSize);
    }
    ImGui::SetItemTooltip("World units per grid cell. Smaller = denser field.");

    if (ImGui::Button("Reset cell size")) {
        starfield.SetCellSize(StarfieldRenderer::Defaults::cellSize);
    }

    ImGui::SeparatorText("Layers (far to near)");
    auto& layers = starfield.Layers();
    for (std::size_t i = 0; i < layers.size(); ++i) {
        StarfieldRenderer::Layer& layer = layers[i];
        const std::string id = "Layer " + std::to_string(i);
        if (ImGui::TreeNodeEx(id.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(220.f);
            ImGui::SliderFloat("Parallax", &layer.parallax, 0.f, 1.2f, "%.2f");
            ImGui::SetNextItemWidth(220.f);
            ImGui::SliderFloat("Density", &layer.density, 0.f, 4.f, "%.2f");
            ImGui::SetNextItemWidth(220.f);
            ImGui::DragFloatRange2("Size (px)", &layer.sizeMin, &layer.sizeMax, 0.05f, 0.2f, 12.f, "%.1f");
            ImGui::SetNextItemWidth(220.f);
            ImGui::SliderFloat("Brightness", &layer.brightness, 0.f, 1.5f, "%.2f");
            ImGui::PopID();
            ImGui::TreePop();
        }
    }

    ImGui::EndDisabled();
}

} // namespace Gravitaris
