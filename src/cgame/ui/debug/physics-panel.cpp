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
    ImGui::SetItemTooltip("Scales every planet's pull on every body. Applied live, every tick. Default 1.667.");
    if (ImGui::Button("Reset##gravity")) {
        game.SetGravityMultiplier(1.667f);
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
                          "about your own mass). Default 0.667.");
    if (ImGui::Button("Reset##weight")) {
        game.SetShipWeightMultiplier(0.667f);
    }

    ImGui::SeparatorText("Ship-vs-ship contact (networking-plan Phase 9)");
    ImGui::TextWrapped("Ships never push each other. A slow contact is an overlap; a fast one destroys. "
                       "Planets, structures and freighters are unaffected -- they keep real physics.");

    PhysicsSystem::ShipContactParams& contact = game.GetShipContactParams();
    const PhysicsSystem::ShipContactParams defaults;

    auto slider = [](const char* label, double& value, float min, float max, const char* fmt,
                     const char* tooltip) {
        auto v = static_cast<float>(value);
        ImGui::SetNextItemWidth(220.f);
        if (ImGui::SliderFloat(label, &v, min, max, fmt)) {
            value = static_cast<double>(v);
        }
        ImGui::SetItemTooltip("%s", tooltip);
    };

    slider("Ram threshold (speed)", contact.ramClosingSpeed, 0.f, 400.f, "%.0f",
           "Closing speed at which a contact stops being a harmless overlap and destroys instead. "
           "Below it two ships just slide through each other.");
    slider("Separation push", contact.separationAccel, 0.f, 400.f, "%.0f",
           "Acceleration easing overlapping ships apart. 0 = they pass through each other completely.");
    slider("Both-die momentum", contact.bothDieMomentum, 0.f, 4000.f, "%.0f",
           "Lighter ship's mass x closing speed past which neither survives, however tough. "
           "Below it the weaker of the two dies and the winner takes damage.");
    slider("Survivor damage scale", contact.survivorDamageScale, 0.f, 0.2f, "%.3f",
           "Damage the winner takes per unit of the loser's mass x closing speed. High enough values "
           "make most rams mutual.");

    if (ImGui::Button("Reset##shipcontact")) {
        contact = defaults;
    }
}

} // namespace Gravitaris
