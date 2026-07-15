#include <gravitaris/game/logging.hpp>

#include <gravitaris/cgame/audio/audio-backend.hpp>
#include <gravitaris/cgame/audio/backend/magnum-openal-backend.hpp>
#include <gravitaris/cgame/audio/backend/miniaudio-backend.hpp>
#include <gravitaris/cgame/audio/audio-backend-factory.hpp>

namespace Gravitaris {

AudioBackendPreference ResolveAudioBackendPreference(AudioBackendPreference preference)
{
    if (preference != AudioBackendPreference::Auto) return preference;

#if defined(__APPLE__)
    // OpenAL's context-current problem was never confirmed fixed on macOS
    // (no Mac to test); miniaudio is the safe default there. See
    // docs/adr/0003-audio-backend.md.
    return AudioBackendPreference::PreferMiniaudio;
#else
    return AudioBackendPreference::PreferOpenAL;
#endif
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
