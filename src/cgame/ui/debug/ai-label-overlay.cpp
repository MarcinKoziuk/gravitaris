#include <cstdio>

#include <imgui.h>

#include <Magnum/Math/Color.h>

#include <gravitaris/game/ai/ai-preset-library.hpp>
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

// The temperament sits above the goal/tactic pair, smaller and dimmer than
// them: it is what this pilot *is* rather than what it is doing this second,
// so it should read as a caption on the busy lines rather than compete with
// them. Team colour is deliberately dropped -- the lines below already carry
// the side, and a third line in the same colour just thickens the block.
constexpr float PRESET_SCALE = 0.78f;
constexpr float PRESET_LINE_HEIGHT = LINE_HEIGHT * PRESET_SCALE;
const Magnum::Color3 PRESET_COLOR{0.62f, 0.72f, 0.78f};

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

// One smaller line sitting above whatever DrawLines just drew `below` lines of.
void DrawCaption(ImDrawList* drawList, const ImVec2& anchor, const char* text, int below)
{
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * PRESET_SCALE;
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);

    const ImVec2 at(anchor.x - extent.x * 0.5f,
                    anchor.y - SHIP_CLEARANCE - LINE_HEIGHT * static_cast<float>(below)
                            - PRESET_LINE_HEIGHT);

    const ImU32 fill = IM_COL32(static_cast<int>(PRESET_COLOR.r() * 255.f),
                                static_cast<int>(PRESET_COLOR.g() * 255.f),
                                static_cast<int>(PRESET_COLOR.b() * 255.f), 210);
    drawList->AddText(font, size, ImVec2(at.x + 1.f, at.y + 1.f), IM_COL32(0, 0, 0, 170), text);
    drawList->AddText(font, size, at, fill, text);
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

        // Named through the library rather than stored on the pilot: the
        // preset's knobs are copied into the pilot at spawn and can be edited
        // live, so the id is the only thing that still means "this
        // temperament".
        if (const AIPreset* preset = game.GetAIPresets().Find(pilot.presetId)) {
            DrawCaption(drawList, anchor, preset->name.c_str(), count);
        }
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
