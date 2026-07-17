#include <gravitaris/game/logging.hpp>

#include <gravitaris/cgame/audio/audio-backend.hpp>
#include <gravitaris/cgame/audio/backend/magnum-openal-backend.hpp>
#include <gravitaris/cgame/audio/backend/miniaudio-backend.hpp>
#include <gravitaris/cgame/audio/audio-backend-factory.hpp>

namespace Gravitaris {

AudioBackendPreference ResolveAudioBackendPreference(AudioBackendPreference preference)
{
    if (preference != AudioBackendPreference::Auto) return preference;

    // miniaudio everywhere by default: the OpenAL backend still emits sharp
    // high-pitched pops on this machine (root cause unresolved -- possibly a
    // memory-safety issue rather than a backend bug; investigation open), and
    // miniaudio is clean. OpenAL stays available via an explicit
    // PreferOpenAL from the debug UI's Audio tab for A/B comparison.
    // See docs/adr/0003-audio-backend.md.
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
