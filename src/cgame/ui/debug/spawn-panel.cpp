#include <imgui.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

#include <gravitaris/cgame/cgame.hpp>

#include "spawn-panel.hpp"

namespace Gravitaris {

void DrawSpawnPanel(CGame& game)
{
    ImGui::SeparatorText("AI ships");

    if (ImGui::Button("Spawn AI fighter near player")) {
        Vector2d pos{300.0, 200.0};
        const std::optional<flecs::entity> player = game.GetPlayer();
        const Transform* transform = player ? player->try_get<Transform>() : nullptr;
        if (transform) {
            pos = transform->pos + Vector2d{250.0, 150.0};
        }
        game.GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, pos);
    }

    ImGui::Text("AI ships alive: %d", game.GetRegistry().count<AIPilot>());

    // TODO(debug-ui): generic spawning (model picker + position/velocity
    // inputs) for players / planets / bullets.
}

} // namespace Gravitaris
