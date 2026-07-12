#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "renderer-panel.hpp"

namespace Gravitaris {

void DrawRendererPanel(CGame& game)
{
    ImGui::SeparatorText("Active line renderer");

    const RendererKind current = game.GetActiveRenderer();
    int choice = (current == RendererKind::Simple) ? 0 : 1;

    ImGui::RadioButton("SimpleModelRenderer (GL LineStrip)", &choice, 0);
    ImGui::SetItemTooltip("Fixed 1px GL lines, no thickness control.");
    ImGui::RadioButton("ModelRenderer2 (baked/instanced, px width)", &choice, 1);
    ImGui::SetItemTooltip("Pixel-space width, constant thickness across zoom.");

    const RendererKind picked = (choice == 0) ? RendererKind::Simple : RendererKind::Baked;
    if (picked != current) {
        game.SetActiveRenderer(picked);
    }

    ImGui::SeparatorText("Line width");
    ImGui::BeginDisabled(current == RendererKind::Simple);

    float width = game.GetLineWidth();
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::DragFloat("Pixels", &width, 0.1f, CGame::MIN_LINE_WIDTH, CGame::MAX_LINE_WIDTH, "%.1f")) {
        game.SetLineWidth(width);
    }
    ImGui::SetItemTooltip("Double-click to type an exact value.");

    ImGui::SameLine();
    if (ImGui::Button("-##linewidth")) {
        game.AddLineWidth(-0.5f);
    }
    ImGui::SameLine();
    if (ImGui::Button("+##linewidth")) {
        game.AddLineWidth(0.5f);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(also Numpad -/+)");

    if (ImGui::Button("Reset line width to default")) {
        game.SetLineWidth(CGame::Defaults::lineWidth);
    }

    ImGui::EndDisabled();
    if (current == RendererKind::Simple) {
        ImGui::TextDisabled("SimpleModelRenderer ignores line width (fixed 1px GL lines).");
    }
}

} // namespace Gravitaris
