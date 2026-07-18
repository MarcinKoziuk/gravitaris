#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "audio-panel.hpp"

namespace Gravitaris {

void DrawAudioPanel(CGame& game)
{
    ImGui::SeparatorText("Audio backend");

    ImGui::Text("Active: %s", game.GetAudioBackendName());
    if (!game.IsAudioEnabled()) {
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Audio disabled (backend failed to initialize).");
    }
}

} // namespace Gravitaris
