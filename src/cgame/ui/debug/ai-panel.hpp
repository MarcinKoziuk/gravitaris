#pragma once

namespace Gravitaris {

class CGame;

// Draws the "AI" debug tab: one row per AI pilot -- what the strategy layer
// decided, what the tactical layer is flying, and what it is aimed at -- so a
// misbehaving opponent reads as a bad goal choice or a bad execution of a
// good one, which is the distinction the weights are tuned against.
// Assumes an ImGui frame is active and a tab/child is already open.
void DrawAiPanel(CGame& game);

} // namespace Gravitaris
