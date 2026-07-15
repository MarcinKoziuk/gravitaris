#pragma once

namespace Gravitaris {

class CGame;

// Draws the "Audio" debug tab: pick the active audio backend.
// Assumes an ImGui frame is active and a tab/child is already open.
void DrawAudioPanel(CGame& game);

} // namespace Gravitaris
