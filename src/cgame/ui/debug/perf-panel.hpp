#pragma once

namespace Gravitaris {

class CGame;

// Draws the "Performance" debug tab: FPS and per-section frame time (physics,
// game logic, rendering, post-process, audio, UI, ...).
// Assumes an ImGui frame is active and a tab/child is already open.
void DrawPerformancePanel(CGame& game);

} // namespace Gravitaris
