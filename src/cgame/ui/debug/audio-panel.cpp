#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>

#include "audio-panel.hpp"

namespace Gravitaris {

void DrawAudioPanel(CGame& game)
{
    ImGui::SeparatorText("Audio backend");
    ImGui::TextDisabled("Scaffolding while both backends exist -- see docs/adr/0003-audio-backend.md.");

    // Explicit choices only -- Auto is a construction-time convenience (platform
    // default), not something meaningful to select here once a backend is
    // already running.
    const AudioBackendPreference current = game.GetAudioBackendPreference();
    int choice = (current == AudioBackendPreference::PreferMiniaudio) ? 1 : 0;

    ImGui::RadioButton("OpenAL", &choice, 0);
    ImGui::RadioButton("miniaudio", &choice, 1);
    ImGui::TextDisabled("No fallback: if the selected backend fails to initialize, audio is disabled.");

    const AudioBackendPreference picked =
            (choice == 0) ? AudioBackendPreference::PreferOpenAL : AudioBackendPreference::PreferMiniaudio;
    if (picked != current) {
        game.SetAudioBackendPreference(picked);
    }

    ImGui::Separator();
    ImGui::Text("Active: %s", game.GetAudioBackendName());
    if (!game.IsAudioEnabled()) {
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Audio disabled (no usable backend).");
    }
}

} // namespace Gravitaris
