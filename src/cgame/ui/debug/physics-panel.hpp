#pragma once

namespace Gravitaris {

class CGame;

// Draws the "Physics" debug tab: temporary calibration multipliers for
// planet gravity and the player ship's weight (see CGame::SetGravity/
// ShipWeightMultiplier). Assumes an ImGui frame is active and a tab/child is
// already open.
void DrawPhysicsPanel(CGame& game);

} // namespace Gravitaris
