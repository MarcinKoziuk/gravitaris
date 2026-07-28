#include <algorithm>
#include <vector>

#include <imgui.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/ai/ai-preset-library.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/gwell/faction-system.hpp>

#include <gravitaris/cgame/cgame.hpp>

#include "spawn-panel.hpp"

namespace Gravitaris {

namespace {

// Only for the dropdown-selected preset; the random button uses
// Game::SpawnRandomAIShip() directly, shared with the J shortcut. Same site
// rule as that one: Red launches from its own High Port/planet, and only
// falls back to the player's neighbourhood when it holds nothing.
void SpawnAI(CGame& game, const AIPreset& preset)
{
    FactionSystem::SpawnPoint spawn;
    spawn.pos = Vector2d{300.0, 200.0};
    if (const std::optional<FactionSystem::SpawnPoint> site = game.GetFactionSystem().SpawnPosition(TeamId::Red)) {
        spawn = *site;
    }
    else {
        const std::optional<flecs::entity> player = game.GetPlayer();
        if (const Transform* transform = player ? player->try_get<Transform>() : nullptr) {
            spawn.pos = transform->pos + Vector2d{250.0, 150.0};
        }
    }
    game.GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, spawn.pos, preset, spawn.vel, spawn.rot);
}

} // namespace

void DrawSpawnPanel(CGame& game)
{
    ImGui::SeparatorText("AI ships");

    // Straight off the loaded library, so a preset added to
    // data/ai-presets.toml shows up here with no change to this panel.
    const std::vector<AIPreset>& presets = game.GetAIPresets().All();
    static int presetIndex = 0;
    presetIndex = std::clamp(presetIndex, 0, static_cast<int>(presets.size()) - 1);
    if (ImGui::BeginCombo("Personality", presets[presetIndex].name.c_str())) {
        for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
            if (ImGui::Selectable(presets[i].name.c_str(), i == presetIndex)) presetIndex = i;
            if (ImGui::IsItemHovered() && !presets[i].description.empty()) {
                ImGui::SetTooltip("%s", presets[i].description.c_str());
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button("Spawn AI fighter near player")) {
        SpawnAI(game, presets[presetIndex]);
    }

    if (ImGui::Button("Spawn AI fighter (random personality)")) {
        game.SpawnRandomAIShip();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(J)");

    ImGui::Text("AI ships alive: %d", game.GetRegistry().count<AIPilot>());

    // TODO(debug-ui): generic spawning (model picker + position/velocity
    // inputs) for players / planets / bullets.
}

} // namespace Gravitaris
