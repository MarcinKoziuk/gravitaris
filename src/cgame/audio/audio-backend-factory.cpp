#include <gravitaris/game/logging.hpp>

#include <gravitaris/cgame/audio/audio-backend.hpp>
#include <gravitaris/cgame/audio/backend/magnum-openal-backend.hpp>
#include <gravitaris/cgame/audio/backend/miniaudio-backend.hpp>
#include <gravitaris/cgame/audio/audio-backend-factory.hpp>

namespace Gravitaris {

AudioBackendPreference ResolveAudioBackendPreference(AudioBackendPreference preference)
{
    if (preference != AudioBackendPreference::Auto) return preference;

    // miniaudio on every platform, not just __APPLE__: MagnumOpenALBackend
    // is split across two link units (MagnumAudio-d.dll makes the real
    // Buffer/Source/Renderer AL calls; our own Init() -- after the ALC
    // rework -- makes its device-current calls from GravitarisNG.exe).
    // Static-linked OpenAL Soft means each unit gets its OWN copy of ALC's
    // global "current context" state, so the exe's context is never the one
    // the DLL's AL calls actually see -- Init() reports success while every
    // real playback call silently fails with AL_INVALID_OPERATION. This
    // reproduces on Windows, not just the macOS/CoreAudio setup the ADR
    // originally (and, it turned out, incorrectly) attributed the symptom
    // to. See docs/adr/0003-audio-backend.md's "Update" section.
    // OpenAL stays selectable via the debug UI's Audio tab for comparison/
    // debugging, just not as the automatic default.
    return AudioBackendPreference::PreferMiniaudio;
}

std::unique_ptr<IAudioBackend> CreateAudioBackend(AudioBackendPreference preference)
{
    preference = ResolveAudioBackendPreference(preference);

    if (preference == AudioBackendPreference::PreferOpenAL) {
        auto openal = std::make_unique<MagnumOpenALBackend>();
        if (!openal->Init()) {
            LOG(warning) << "[audio] OpenAL backend failed to initialize; audio disabled";
            return nullptr;
        }
        LOG(info) << "[audio] using backend: " << openal->Name();
        return openal;
    }

    auto miniaudio = std::make_unique<MiniaudioBackend>();
    if (!miniaudio->Init()) {
        LOG(warning) << "[audio] miniaudio backend failed to initialize; audio disabled";
        return nullptr;
    }
    LOG(info) << "[audio] using backend: " << miniaudio->Name();
    return miniaudio;
}

} // namespace Gravitaris
