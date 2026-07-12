#pragma once

namespace Gravitaris {

class CGame;

// Draws the "Renderer" debug tab: pick the active line renderer.
// Assumes an ImGui frame is active and a tab/child is already open.
void DrawRendererPanel(CGame& game);

} // namespace Gravitaris
