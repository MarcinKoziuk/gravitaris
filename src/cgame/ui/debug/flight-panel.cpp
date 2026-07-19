#include <imgui.h>

#include <Magnum/Math/Angle.h>

#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/gnc/control/flight-controller.hpp>
#include <gravitaris/game/gnc/guidance/behaviors.hpp>
#include <gravitaris/game/system/landing-state-system.hpp>
#include <gravitaris/game/system/conquest-system.hpp>

#include <gravitaris/cgame/cgame.hpp>

#include "world-to-ui.hpp"
#include "flight-panel.hpp"

namespace Gravitaris {

using Magnum::Degd;
using Magnum::Radd;

static void DrawSliderD(const char* label, double& value, float min, float max, const char* fmt = "%.2f")
{
    float f = static_cast<float>(value);
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderFloat(label, &f, min, max, fmt)) {
        value = f;
    }
}

void DrawFlightPanel(CGame& game)
{
    ImGui::SeparatorText("Autopilot (K/P/G/O in game)");

    const AutopilotMode current = game.GetAutopilotMode();
    int choice = static_cast<int>(current);

    ImGui::RadioButton("Off", &choice, static_cast<int>(AutopilotMode::Off));
    ImGui::RadioButton("Kill velocity (K)", &choice, static_cast<int>(AutopilotMode::KillVelocity));
    ImGui::RadioButton("Hold position (P)", &choice, static_cast<int>(AutopilotMode::HoldPosition));
    ImGui::RadioButton("Goto point (G)", &choice, static_cast<int>(AutopilotMode::GotoPoint));
    ImGui::RadioButton("Orbit heaviest body (O)", &choice, static_cast<int>(AutopilotMode::Orbit));

    if (choice != static_cast<int>(current)) {
        game.SetAutopilotMode(static_cast<AutopilotMode>(choice));
    }

    {
        const Magnum::Math::Vector2<double>& target = game.GetGotoTarget();
        float xy[2] = {static_cast<float>(target.x()), static_cast<float>(target.y())};
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::InputFloat2("Goto target", xy)) {
            game.SetGotoTarget({static_cast<double>(xy[0]), static_cast<double>(xy[1])});
        }
    }

    FlightControllerParams& p = game.GetFlightParams();

    ImGui::SeparatorText("Controller gains");
    DrawSliderD("Heading Kp", p.headingKp, 0.f, 20.f, "%.1f");
    DrawSliderD("Heading Kd", p.headingKd, 0.f, 10.f);
    DrawSliderD("Turn deadband", p.turnDeadband, 0.f, 2.f);
    {
        float aimToleranceDeg = static_cast<float>(static_cast<double>(Degd(Radd(p.aimTolerance))));
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::SliderFloat("Aim tolerance (deg)", &aimToleranceDeg, 1.f, 90.f, "%.0f")) {
            p.aimTolerance = static_cast<double>(Radd(Degd(aimToleranceDeg)));
        }
    }
    DrawSliderD("Velocity deadband", p.velocityDeadband, 0.f, 10.f, "%.1f");
    DrawSliderD("Position Kp", p.positionKp, 0.f, 5.f);
    DrawSliderD("Max approach speed", p.maxApproachSpeed, 1.f, 200.f, "%.0f");

    GuidanceParams& g = game.GetGuidanceParams();

    ImGui::SeparatorText("Guidance");
    DrawSliderD("Max speed", g.maxSpeed, 5.f, 200.f, "%.0f");
    DrawSliderD("Flip time (s)", g.flipTime, 0.f, 4.f);
    DrawSliderD("Arrive radius", g.arriveRadius, 0.5f, 20.f, "%.1f");
    DrawSliderD("Orbit radial Kp", g.orbitRadialKp, 0.f, 3.f);
    DrawSliderD("Max radial speed", g.maxRadialSpeed, 1.f, 60.f, "%.0f");
    ImGui::Text("Thrust accel: %.1f u/s^2 (from ship mass, set on engage)", g.accel);

    if (ImGui::Button("Reset gains to defaults")) {
        const double accel = g.accel;
        p = FlightControllerParams{};
        g = GuidanceParams{};
        g.accel = accel;
    }

    ImGui::SeparatorText("Telemetry");
    const std::optional<flecs::entity> player = game.GetPlayer();
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;
    if (!transform) {
        ImGui::TextDisabled("No player.");
        return;
    }

    ImGui::Text("Speed: %.2f  AngVel: %.2f rad/s", transform->vel.length(), transform->angVel);

    if (const LandingState* landing = player->try_get<LandingState>()) {
        const bool safeSpeed = transform->vel.length() < LandingStateSystem::SAFE_LANDING_SPEED;
        ImGui::Text("Landing: %s | speed %s (< %.0f)",
                    landing->landed ? "LANDED" : "in flight",
                    safeSpeed ? "safe" : "too fast",
                    LandingStateSystem::SAFE_LANDING_SPEED);
        if (landing->landed) {
            ImGui::Text("  on planet NetId %u, %u/%u ticks to claim",
                        landing->landedOnNetId,
                        landing->landedTicks > ConquestSystem::CLAIM_TICKS
                                ? ConquestSystem::CLAIM_TICKS : landing->landedTicks,
                        ConquestSystem::CLAIM_TICKS);
        }
    }

    switch (current) {
        case AutopilotMode::HoldPosition:
            ImGui::Text("Distance to anchor: %.2f",
                        (game.GetAutopilotAnchor() - transform->pos).length());
            break;
        case AutopilotMode::GotoPoint:
            ImGui::Text("Distance to target: %.2f",
                        (game.GetGotoTarget() - transform->pos).length());
            break;
        case AutopilotMode::Orbit: {
            const double dist = (transform->pos - game.GetOrbitCenter()).length();
            ImGui::Text("Radius: %.2f (target %.2f)", dist, game.GetOrbitRadius());
            break;
        }
        default:
            break;
    }
}

void DrawFlightOverlay(CGame& game, const Magnum::Vector2& uiSize)
{
    const AutopilotMode mode = game.GetAutopilotMode();
    if (mode == AutopilotMode::Off) return;

    const WorldToUi worldToUi(game, uiSize);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const ImU32 color = IM_COL32(255, 200, 80, 190);

    switch (mode) {
        case AutopilotMode::HoldPosition: {
            const ImVec2 c = worldToUi(game.GetAutopilotAnchor());
            drawList->AddCircle(c, 6.f, color, 0, 1.5f);
            break;
        }
        case AutopilotMode::GotoPoint: {
            const ImVec2 c = worldToUi(game.GetGotoTarget());
            drawList->AddLine(ImVec2(c.x - 8.f, c.y), ImVec2(c.x + 8.f, c.y), color, 1.5f);
            drawList->AddLine(ImVec2(c.x, c.y - 8.f), ImVec2(c.x, c.y + 8.f), color, 1.5f);
            break;
        }
        case AutopilotMode::Orbit: {
            const ImVec2 c = worldToUi(game.GetOrbitCenter());
            const float radiusPx = static_cast<float>(game.GetOrbitRadius()) * worldToUi.Scale();
            drawList->AddCircle(c, radiusPx, color, 0, 1.5f);
            break;
        }
        default:
            break;
    }
}

} // namespace Gravitaris
