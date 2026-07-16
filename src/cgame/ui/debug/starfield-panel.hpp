#pragma once

namespace Gravitaris {

class CGame;

// Draws the "Starfield" debug tab: toggle, global tuning and per-layer
// parallax/density/size/brightness/streak controls.
// Assumes an ImGui frame is active and a tab/child is already open.
void DrawStarfieldPanel(CGame& game);

} // namespace Gravitaris
