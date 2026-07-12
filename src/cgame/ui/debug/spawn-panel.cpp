#include <imgui.h>

#include "spawn-panel.hpp"

namespace Gravitaris {

// TODO(debug-ui): wire up actual spawning. The pieces already exist:
//   - EntitySpawner::SpawnPlayer / SpawnPlanet / SpawnBullet(id_t modelId, ...)
//   - CEntitySpawner is created by CGame::CreateEntitySpawner()
// To implement: expose the spawner (and a list of loaded model ids) from CGame,
// then add model picker + position/velocity inputs + spawn buttons here.
void DrawSpawnPanel(CGame& /*game*/)
{
    ImGui::SeparatorText("Spawn test objects");
    ImGui::TextDisabled("Coming soon.");
    ImGui::TextWrapped(
        "Will let you spawn players / planets / bullets at a chosen position "
        "from the loaded model set. Not wired up yet.");
}

} // namespace Gravitaris
