#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "renderer-panel.hpp"

namespace Gravitaris {

void DrawRendererPanel(CGame& game)
{
    ImGui::SeparatorText("Display scale");

    // This overlay is scaled by the very value being dragged, so committing
    // mid-drag moves the slider out from under the cursor and the drag can't
    // be finished. Held here until the drag ends instead. Negative = not held.
    static float heldUiScale = -1.f;

    float uiScale = heldUiScale >= 0.f ? heldUiScale : game.GetUiScale();
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("UI scale", &uiScale, CGame::MIN_UI_SCALE, CGame::MAX_UI_SCALE, "%.2fx",
                            ImGuiSliderFlags_AlwaysClamp)) {
        heldUiScale = uiScale;
    }
    if (ImGui::IsItemDeactivated()) {
        if (heldUiScale >= 0.f) game.SetUiScale(heldUiScale);
        heldUiScale = -1.f;
    }
    ImGui::SetItemTooltip(
            "Multiplies the display's own scaling, and applies to everything sized to be\n"
            "seen: HUD, this overlay, line widths, scanlines, and how much world fits on\n"
            "screen. The window keeps rendering at full resolution either way.\n"
            "Raise it on a dense display the OS is driving at 100%.\n"
            "\n"
            "Applied when the drag ends, since this overlay resizes with it.\n"
            "The minimap and compass textures are sized at startup, so they only\n"
            "sharpen on the next run.");

    ImGui::SameLine();
    if (ImGui::Button("Reset##uiscale")) {
        game.SetUiScale(CGame::Defaults::uiScale);
    }

    // InputScalar asserts on ImGuiInputTextFlags_EnterReturnsTrue, so Enter is
    // caught via deactivation instead -- and the value has to be held for the
    // same reason as the slider, since without that flag every keystroke
    // reports a change.
    static float heldTypedUiScale = -1.f;

    float typedUiScale = heldTypedUiScale >= 0.f ? heldTypedUiScale : game.GetUiScale();
    ImGui::SetNextItemWidth(160.f);
    // No step buttons: they'd apply on click and then move themselves away.
    if (ImGui::InputFloat("##uiscale_typed", &typedUiScale, 0.f, 0.f, "%.2f")) {
        heldTypedUiScale = typedUiScale;
    }
    if (ImGui::IsItemDeactivated()) {
        if (heldTypedUiScale >= 0.f) game.SetUiScale(heldTypedUiScale);
        heldTypedUiScale = -1.f;
    }
    ImGui::SetItemTooltip("Type an exact value and press Enter.");

    ImGui::SameLine();
    ImGui::Text("Effective: %.2fx (display %.2fx)", game.GetContentScale(),
                game.GetUiScale() > 0.f ? game.GetContentScale() / game.GetUiScale() : 0.f);

    ImGui::SeparatorText("Active line renderer");

    const RendererKind current = game.GetActiveRenderer();
    int choice = (current == RendererKind::Simple) ? 0 : (current == RendererKind::Baked) ? 1 : 2;

    ImGui::RadioButton("SimpleModelRenderer (GL LineStrip)", &choice, 0);
    ImGui::SetItemTooltip("Fixed 1px GL lines, no thickness control.");
    ImGui::RadioButton("ModelRenderer2 (baked/instanced, px width)", &choice, 1);
    ImGui::SetItemTooltip("Pixel-space width; thickness vs. zoom is tunable below.");
    ImGui::RadioButton("Snapshot mirror (net debug)", &choice, 2);
    ImGui::SetItemTooltip("Draws a second world fed only by serialize->apply of the live sim every "
                          "frame (networking-plan 2.5). Should look identical to ModelRenderer2; "
                          "cost shows as 'Snapshot Mirror' in the perf panel. HUD arrows are hidden "
                          "in this mode.");

    const RendererKind picked = (choice == 0) ? RendererKind::Simple
                              : (choice == 1) ? RendererKind::Baked
                                              : RendererKind::Mirror;
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

    float zoomWidthFactor = game.GetZoomWidthFactor();
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Zoom width factor", &zoomWidthFactor, CGame::MIN_ZOOM_WIDTH_FACTOR,
                            CGame::MAX_ZOOM_WIDTH_FACTOR, "%.2f")) {
        game.SetZoomWidthFactor(zoomWidthFactor);
    }
    ImGui::SetItemTooltip(
            "Width at the normal zoom level is always the line width above,\n"
            "regardless of this factor; it only sets how width tracks zoom.\n"
            "0 = pixel width constant across zoom.\n"
            "1 = pixel width scales linearly with zoom (constant world-space width).\n"
            "In between, lines get thicker zoomed in / thinner zoomed out in pixels,\n"
            "while staying relatively thinner zoomed in / thicker zoomed out in world space.");

    if (ImGui::Button("Reset zoom width factor to default")) {
        game.SetZoomWidthFactor(CGame::Defaults::zoomWidthFactor);
    }

    ImGui::EndDisabled();
    if (current == RendererKind::Simple) {
        ImGui::TextDisabled("SimpleModelRenderer ignores line width (fixed 1px GL lines).");
    }
}

} // namespace Gravitaris
