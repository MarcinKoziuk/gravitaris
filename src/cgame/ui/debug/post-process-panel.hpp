#pragma once

namespace Gravitaris {

class GlowPostProcess;

// Draws the "Post-process" debug tab: glow (bloom) + CRT/scanline controls.
// Assumes an ImGui frame is active and a tab/child is already open.
void DrawPostProcessPanel(GlowPostProcess& glow);

} // namespace Gravitaris
