#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "physics-panel.hpp"

namespace Gravitaris {

void DrawPhysicsPanel(CGame& game)
{
    ImGui::TextDisabled("Temporary calibration knobs -- not meant to ship at non-1.0.");

    ImGui::SeparatorText("Gravity");
    float gravity = game.GetGravityMultiplier();
    ImGui::SetNextItemWidth(220.f);
    if (ImGui::SliderFloat("Gravity multiplier", &gravity, 0.f, 4.f, "%.2f")) {
        game.SetGravityMultiplier(gravity);
    }
    ImGui::SetItemTooltip("Scales every planet's pull on every body. Applied live, every tick.");
    if (ImGui::Button("Reset##gravity")) {
        game.SetGravityMultiplier(1.f);
    }

    ImGui::SeparatorText("Ship weight");
    float weight = game.GetShipWeightMultiplier();
    ImGui::SetNextItemWidth(220.f);
    if (ImGui::SliderFloat("Weight multiplier", &weight, 0.1f, 4.f, "%.2f")) {
        game.SetShipWeightMultiplier(weight);
    }
    ImGui::SetItemTooltip("Scales the player ship's mass off its resource-authored base. Heavier "
                          "= more sluggish under thrust and less speed change per impact; gravity's "
                          "own pull on the ship is unaffected (real physics: falling doesn't care "
                          "about your own mass).");
    if (ImGui::Button("Reset##weight")) {
        game.SetShipWeightMultiplier(1.f);
    }
}

} // namespace Gravitaris
