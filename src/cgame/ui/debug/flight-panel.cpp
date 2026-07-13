#include <imgui.h>

#include <Magnum/Math/Angle.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/control/flight-controller.hpp>

#include <gravitaris/cgame/cgame.hpp>

#include "flight-panel.hpp"

namespace Gravitaris {

using Magnum::Degd;
using Magnum::Radd;

void DrawFlightPanel(CGame& game)
{
    ImGui::SeparatorText("Autopilot (K / P in game)");

    const AutopilotMode current = game.GetAutopilotMode();
    int choice = static_cast<int>(current);

    ImGui::RadioButton("Off", &choice, static_cast<int>(AutopilotMode::Off));
    ImGui::RadioButton("Kill velocity (K)", &choice, static_cast<int>(AutopilotMode::KillVelocity));
    ImGui::SetItemTooltip("Continuously retro-burn toward zero velocity (fights gravity).");
    ImGui::RadioButton("Hold position (P)", &choice, static_cast<int>(AutopilotMode::HoldPosition));
    ImGui::SetItemTooltip("Anchor at the current position and hover there.");

    if (choice != static_cast<int>(current)) {
        game.SetAutopilotMode(static_cast<AutopilotMode>(choice));
    }

    FlightControllerParams& p = game.GetFlightParams();

    ImGui::SeparatorText("Controller gains");
    float headingKp = static_cast<float>(p.headingKp);
    float headingKd = static_cast<float>(p.headingKd);
    float turnDeadband = static_cast<float>(p.turnDeadband);
    float aimToleranceDeg = static_cast<float>(static_cast<double>(Degd(Radd(p.aimTolerance))));
    float velocityDeadband = static_cast<float>(p.velocityDeadband);

    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Heading Kp", &headingKp, 0.f, 20.f, "%.1f")) p.headingKp = headingKp;
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Heading Kd", &headingKd, 0.f, 10.f, "%.2f")) p.headingKd = headingKd;
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Turn deadband", &turnDeadband, 0.f, 2.f, "%.2f")) p.turnDeadband = turnDeadband;
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Aim tolerance (deg)", &aimToleranceDeg, 1.f, 90.f, "%.0f")) {
        p.aimTolerance = static_cast<double>(Radd(Degd(aimToleranceDeg)));
    }
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Velocity deadband", &velocityDeadband, 0.f, 10.f, "%.1f")) {
        p.velocityDeadband = velocityDeadband;
    }

    ImGui::SeparatorText("Hold-position guidance");
    float positionKp = static_cast<float>(p.positionKp);
    float maxApproach = static_cast<float>(p.maxApproachSpeed);
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Position Kp", &positionKp, 0.f, 5.f, "%.2f")) p.positionKp = positionKp;
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat("Max approach speed", &maxApproach, 1.f, 200.f, "%.0f")) {
        p.maxApproachSpeed = maxApproach;
    }

    if (ImGui::Button("Reset gains to defaults")) {
        p = FlightControllerParams{};
    }

    ImGui::SeparatorText("Telemetry");
    const std::optional<flecs::entity> player = game.GetPlayer();
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;
    if (!transform) {
        ImGui::TextDisabled("No player.");
        return;
    }

    ImGui::Text("Speed: %.2f  AngVel: %.2f rad/s", transform->vel.length(), transform->angVel);
    if (current == AutopilotMode::HoldPosition) {
        const double dist = (game.GetAutopilotAnchor() - transform->pos).length();
        ImGui::Text("Distance to anchor: %.2f", dist);
    }
}

} // namespace Gravitaris
