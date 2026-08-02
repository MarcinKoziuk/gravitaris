#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/ai-strategy.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/transform.hpp>

#include <gravitaris/cgame/cgame.hpp>
#include <gravitaris/cgame/team-color.hpp>

#include "ai-names.hpp"
#include "ai-panel.hpp"

namespace Gravitaris {

namespace {

struct PilotRow {
    flecs::entity entity;
    std::uint32_t netId = 0;
    TeamId team = TeamId::None;
    const AIPilot* pilot = nullptr;
    const AIStrategy* strategy = nullptr;
    float hp = 0.f;
    Vector2d pos;
};

// Distance from `from` to an entity's transform, or -1 when it has none
// (dead, or a subject that never had one).
double DistanceTo(const Vector2d& from, flecs::entity target)
{
    if (!target.is_alive()) return -1.0;
    const Transform* transf = target.try_get<Transform>();
    return transf ? (transf->pos - from).length() : -1.0;
}

void DrawDistance(double distance)
{
    if (distance < 0.0) ImGui::TextDisabled("-");
    else ImGui::Text("%.0f", distance);
}

} // namespace

void DrawAiPanel(CGame& game)
{
    std::vector<PilotRow> rows;
    game.GetRegistry().each([&](flecs::entity ent, const AIPilot& pilot, const Transform& transf,
                                const Team& team, const Damageable& damageable, const NetId& netId) {
        rows.push_back(PilotRow{ent, netId.value, team.id, &pilot, ent.try_get<AIStrategy>(),
                                damageable.hp, transf.pos});
    });
    std::sort(rows.begin(), rows.end(),
              [](const PilotRow& a, const PilotRow& b) { return a.netId < b.netId; });

    ImGui::Text("AI pilots: %d", static_cast<int>(rows.size()));
    ImGui::SameLine();
    ImGui::TextDisabled("(leaders carry a strategy; the rest only dogfight)");

    if (rows.empty()) {
        // AIPilot/AIStrategy are server-only (ADR 0001), so a connected
        // client has nothing to show here however busy the round is.
        ImGui::TextDisabled("None in this world -- in multiplayer the AI lives on the server.");
        return;
    }

    if (game.IsSpectating() && ImGui::Button("Stop spectating")) {
        game.StopSpectating();
    }

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
            | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("##ai-pilots", 7, TABLE_FLAGS, ImVec2(0.f, 240.f))) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Net");
    ImGui::TableSetupColumn("Goal");
    ImGui::TableSetupColumn("Order");
    ImGui::TableSetupColumn("Flying");
    ImGui::TableSetupColumn("Subj");
    ImGui::TableSetupColumn("Tgt");
    ImGui::TableSetupColumn("HP");
    ImGui::TableHeadersRow();

    for (const PilotRow& row : rows) {
        ImGui::PushID(static_cast<int>(row.netId));
        ImGui::TableNextRow();

        // The NetId doubles as the spectate button: clicking a row is the
        // fastest way to see what it is actually doing out there.
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(TeamColor(row.team).r(), TeamColor(row.team).g(),
                                                    TeamColor(row.team).b(), 1.f));
        if (ImGui::SmallButton(std::to_string(row.netId).c_str())) {
            game.SetSpectateTarget(row.entity);
        }
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        if (row.strategy) ImGui::TextUnformatted(AIGoalName(row.strategy->goal));
        else ImGui::TextDisabled("-");

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(AIOrderKindName(row.pilot->order.kind));

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(AIBehaviorName(row.pilot->behavior));

        ImGui::TableNextColumn();
        DrawDistance(DistanceTo(row.pos, row.pilot->order.subject));

        ImGui::TableNextColumn();
        DrawDistance(DistanceTo(row.pos, row.pilot->target));

        ImGui::TableNextColumn();
        ImGui::Text("%.0f", row.hp);

        ImGui::PopID();
    }

    ImGui::EndTable();
    ImGui::TextDisabled("Subj/Tgt are distances. Click a Net id to spectate ([ and ] cycle).");
}

} // namespace Gravitaris
