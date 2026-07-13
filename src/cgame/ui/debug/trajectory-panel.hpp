#pragma once

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

class CGame;

// Tab contents: toggles, horizon/stride settings, and the live drift readout.
void DrawTrajectoryPanel(CGame& game);

// World-space overlay: predicts the player's trajectory and draws it as a
// polyline on ImGui's background draw list. Called every overlay frame
// (independent of which tab is selected). uiSize is ImGui's logical display
// size, used to map framebuffer pixels to ImGui coordinates.
void DrawTrajectoryOverlay(CGame& game, const Magnum::Vector2& uiSize);

} // namespace Gravitaris
