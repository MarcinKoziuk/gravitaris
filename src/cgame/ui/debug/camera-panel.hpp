#pragma once

namespace Gravitaris {

class CGame;

// Draws the "Camera" debug tab: dynamic zoom (speed-driven + enemy framing)
// and manual-override tuning, plus live zoom readout.
// Assumes an ImGui frame is active and a tab/child is already open.
void DrawCameraPanel(CGame& game);

} // namespace Gravitaris
