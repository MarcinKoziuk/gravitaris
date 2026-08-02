#include <cstdio>

#include <imgui.h>

#include <Magnum/Math/Color.h>

#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/ai-strategy.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/transform.hpp>

#include <gravitaris/cgame/cgame.hpp>
#include <gravitaris/cgame/team-color.hpp>

#include "world-to-ui.hpp"
#include "ai-names.hpp"
#include "ai-label-overlay.hpp"

namespace Gravitaris {

namespace {

constexpr float LINE_HEIGHT = 15.f;
constexpr float SHIP_CLEARANCE = 22.f; // px between the ship's origin and the lowest line
constexpr float OFFSCREEN_MARGIN = 64.f;
constexpr int MAX_LINES = 2;

bool OnScreen(const ImVec2& p, const Magnum::Vector2& uiSize)
{
    return p.x > -OFFSCREEN_MARGIN && p.y > -OFFSCREEN_MARGIN
            && p.x < uiSize.x() + OFFSCREEN_MARGIN && p.y < uiSize.y() + OFFSCREEN_MARGIN;
}

void DrawLines(ImDrawList* drawList, const ImVec2& anchor, const Magnum::Color3& color,
               const char* const* lines, int count)
{
    const ImU32 fill = IM_COL32(static_cast<int>(color.r() * 255.f), static_cast<int>(color.g() * 255.f),
                                static_cast<int>(color.b() * 255.f), 230);
    const ImU32 shadow = IM_COL32(0, 0, 0, 170);

    for (int i = 0; i < count; ++i) {
        const ImVec2 size = ImGui::CalcTextSize(lines[i]);
        const ImVec2 at(anchor.x - size.x * 0.5f,
                        anchor.y - SHIP_CLEARANCE - LINE_HEIGHT * static_cast<float>(count - i));
        drawList->AddText(ImVec2(at.x + 1.f, at.y + 1.f), shadow, lines[i]);
        drawList->AddText(at, fill, lines[i]);
    }
}

} // namespace

void DrawAiLabelOverlay(CGame& game, const Magnum::Vector2& uiSize)
{
    const WorldToUi worldToUi(game, uiSize);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    game.GetRegistry().each([&](flecs::entity ent, const AIPilot& pilot, const Transform& transf,
                                const Team& team) {
        const ImVec2 anchor = worldToUi(transf.pos);
        if (!OnScreen(anchor, uiSize)) return;

        char tactic[64];
        if (pilot.order.kind == AIOrderKind::None) {
            std::snprintf(tactic, sizeof(tactic), "%s", AIBehaviorName(pilot.behavior));
        }
        else {
            std::snprintf(tactic, sizeof(tactic), "%s > %s", AIOrderKindName(pilot.order.kind),
                          AIBehaviorName(pilot.behavior));
        }

        const char* lines[MAX_LINES];
        int count = 0;
        if (const AIStrategy* strategy = ent.try_get<AIStrategy>()) {
            lines[count++] = AIGoalName(strategy->goal);
        }
        lines[count++] = tactic;

        DrawLines(drawList, anchor, TeamColor(team.id), lines, count);
    });

    game.GetRegistry().each([&](const Freighter& freighter, const Transform& transf, const Team& team) {
        const ImVec2 anchor = worldToUi(transf.pos);
        if (!OnScreen(anchor, uiSize)) return;

        char text[80];
        std::snprintf(text, sizeof(text), "%s > #%u (%s)", BuildOrderName(freighter.buildOrder),
                      freighter.targetPlanetNetId, freighter.arrived ? "unloading" : "in transit");

        const char* lines[1] = {text};
        DrawLines(drawList, anchor, TeamColor(team.id), lines, 1);
    });
}

} // namespace Gravitaris
