#pragma once

namespace Gravitaris {

class CGame;

// Draws the "HUD" debug tab: off-screen target arrow indicators.
// Assumes an ImGui frame is active and a tab/child is already open.
void DrawHudPanel(CGame& game);

} // namespace Gravitaris
