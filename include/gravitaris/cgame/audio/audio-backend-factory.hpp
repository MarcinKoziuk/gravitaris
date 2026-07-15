#pragma once

#include <memory>

namespace Gravitaris {

class IAudioBackend;

// Scaffolding for as long as both backends exist -- see
// docs/adr/0003-audio-backend.md. Once OpenAL is confirmed solid everywhere
// (or the Magnum context-current bug is fixed upstream), delete
// MagnumOpenALBackend and this enum/factory along with it.
enum class AudioBackendPreference {
    Auto,            // platform default -- miniaudio on Apple, OpenAL
                     // elsewhere. No fallback: if the resolved choice fails
                     // to initialize, audio is disabled.
    PreferOpenAL,
    PreferMiniaudio,
};

// Auto -> the platform-default concrete choice; anything else is unchanged.
// Exposed so callers can track/display what they actually got, not just what
// they asked for (AudioSystem uses this to keep GetBackendPreference()
// truthful after an Auto request).
[[nodiscard]] AudioBackendPreference ResolveAudioBackendPreference(AudioBackendPreference preference);

// Constructs exactly the one backend `preference` (or its platform-default
// resolution, for Auto) names. Deliberately does not fall back to a
// different backend on failure -- returns nullptr instead, so the caller
// disables audio rather than silently ending up on a backend the caller
// didn't ask for.
[[nodiscard]] std::unique_ptr<IAudioBackend> CreateAudioBackend(AudioBackendPreference preference);

} // namespace Gravitaris
