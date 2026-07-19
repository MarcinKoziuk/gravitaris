#pragma once

namespace Gravitaris {

class CGame;

// Draws the "Net" debug tab: Phase 4 interpolation/extrapolation tunables
// and multiplayer-client diagnostics. Assumes an ImGui frame is active and
// a tab/child is already open.
void DrawNetPanel(CGame& game);

} // namespace Gravitaris
