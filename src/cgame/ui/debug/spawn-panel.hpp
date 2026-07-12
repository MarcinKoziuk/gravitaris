#pragma once

namespace Gravitaris {

class CGame;

// Draws the "Spawn" debug tab: create test entities at runtime.
// Placeholder for now (see spawn-panel.cpp); assumes an ImGui frame is active
// and a tab/child is already open.
void DrawSpawnPanel(CGame& game);

} // namespace Gravitaris
