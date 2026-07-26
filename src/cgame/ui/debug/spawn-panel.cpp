#include <imgui.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/gnc/ai-personality-presets.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/gwell/faction-system.hpp>

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
// Game::SpawnRandomAIShip() directly, shared with the J shortcut. Same site
// rule as that one: Red launches from its own High Port/planet, and only
// falls back to the player's neighbourhood when it holds nothing.
void SpawnAI(CGame& game, AIPersonalityPreset preset)
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

    static int presetIndex = 0;
    ImGui::Combo("Personality", &presetIndex, PRESET_NAMES, IM_ARRAYSIZE(PRESET_NAMES));

    if (ImGui::Button("Spawn AI fighter near player")) {
        SpawnAI(game, PRESETS[presetIndex]);
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
