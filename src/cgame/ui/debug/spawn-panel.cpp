#include <imgui.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/gnc/ai-personality-presets.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

#include <gravitaris/cgame/cgame.hpp>

#include "spawn-panel.hpp"

namespace Gravitaris {

namespace {

constexpr const char* PRESET_NAMES[] = {"Balanced", "Aggressive", "Cautious", "Sniper", "Reckless"};
constexpr AIPersonalityPreset PRESETS[] = {
        AIPersonalityPreset::Balanced, AIPersonalityPreset::Aggressive, AIPersonalityPreset::Cautious,
        AIPersonalityPreset::Sniper, AIPersonalityPreset::Reckless,
};

// Only for the dropdown-selected preset; the random button uses
// CGame::SpawnRandomAIShip() directly, shared with the J shortcut.
void SpawnAINearPlayer(CGame& game, AIPersonalityPreset preset)
{
    Vector2d pos{300.0, 200.0};
    const std::optional<flecs::entity> player = game.GetPlayer();
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;
    if (transform) {
        pos = transform->pos + Vector2d{250.0, 150.0};
    }
    game.GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, pos, preset);
}

} // namespace

void DrawSpawnPanel(CGame& game)
{
    ImGui::SeparatorText("AI ships");

    static int presetIndex = 0;
    ImGui::Combo("Personality", &presetIndex, PRESET_NAMES, IM_ARRAYSIZE(PRESET_NAMES));

    if (ImGui::Button("Spawn AI fighter near player")) {
        SpawnAINearPlayer(game, PRESETS[presetIndex]);
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
