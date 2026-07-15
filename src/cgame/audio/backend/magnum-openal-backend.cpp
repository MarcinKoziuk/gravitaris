#include <AL/alc.h>

#include <Corrade/Containers/ArrayView.h>

#include <Magnum/Audio/BufferFormat.h>
#include <Magnum/Audio/Renderer.h>

#include <gravitaris/game/logging.hpp>

#include <gravitaris/cgame/audio/backend/magnum-openal-backend.hpp>

namespace Gravitaris {

using namespace Magnum;

namespace {

// Distance at which a source plays at full gain; attenuation follows OpenAL's
// default inverse-clamped model beyond it. World scale: AI engage range 500.
constexpr float REFERENCE_DISTANCE = 250.f;
constexpr float MAX_DISTANCE = 3000.f;

void ConfigurePositional(Audio::Source& source)
{
    source.setReferenceDistance(REFERENCE_DISTANCE)
          .setRolloffFactor(1.f)
          .setMaxDistance(MAX_DISTANCE);
}

} // namespace

bool MagnumOpenALBackend::Init()
{
    // Deliberately not Magnum::Audio::Context::tryCreate(): instrumentation
    // this session proved it, not the platform, is the problem -- on this
    // machine tryCreate() reports success and alcMakeContextCurrent() is
    // even called with a genuinely valid context, yet alcGetCurrentContext()
    // reads back null immediately after, and every subsequent AL call then
    // fails with AL_INVALID_OPERATION (the same symptom the ADR originally
    // attributed to macOS/CoreAudio only -- it reproduces on Windows too).
    // A hand-rolled ALC sequence with the identical device specifier and
    // attribute list Magnum's tryCreate() uses does not exhibit this, so we
    // own the device/context ourselves; Audio::Buffer/Audio::Source don't go
    // through Context::tryCreate() at all and are unaffected.
    const ALCchar* deviceSpecifier = alcGetString(nullptr, ALC_DEFAULT_DEVICE_SPECIFIER);
    m_device = alcOpenDevice(deviceSpecifier);
    if (!m_device) {
        LOG(warning) << "[audio][openal] no OpenAL device";
        return false;
    }

    m_alContext = alcCreateContext(m_device, nullptr);
    if (!m_alContext) {
        LOG(warning) << "[audio][openal] cannot create context";
        alcCloseDevice(m_device);
        m_device = nullptr;
        return false;
    }

    alcMakeContextCurrent(m_alContext);
    if (alcGetCurrentContext() != m_alContext) {
        LOG(warning) << "[audio][openal] context did not become current";
        alcDestroyContext(m_alContext);
        alcCloseDevice(m_device);
        m_alContext = nullptr;
        m_device = nullptr;
        return false;
    }

    return true;
}

MagnumOpenALBackend::~MagnumOpenALBackend()
{
    // Buffers/Sources must be destroyed (they call alDeleteBuffers/Sources)
    // while their context is still current; both vectors are members
    // destructed before this body runs would be too late, so clear them
    // explicitly first.
    m_buffers.clear();
    m_voices.clear();

    if (m_alContext) {
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(m_alContext);
    }
    if (m_device) alcCloseDevice(m_device);
}

SoundBufferHandle MagnumOpenALBackend::UploadBuffer(
        const std::int16_t* samples, std::size_t sampleCount, std::uint32_t sampleRate)
{
    std::uint32_t index;
    if (!m_freeBufferSlots.empty()) {
        index = m_freeBufferSlots.back();
        m_freeBufferSlots.pop_back();
    }
    else {
        m_buffers.emplace_back();
        index = static_cast<std::uint32_t>(m_buffers.size() - 1);
    }

    m_buffers[index].setData(Audio::BufferFormat::Mono16,
            Corrade::Containers::ArrayView<const void>{samples, sampleCount * sizeof(std::int16_t)},
            static_cast<ALsizei>(sampleRate));

    return {index + 1};
}

void MagnumOpenALBackend::ReleaseBuffer(SoundBufferHandle buffer)
{
    if (buffer.id == 0) return;
    m_freeBufferSlots.push_back(buffer.id - 1);
}

VoiceHandle MagnumOpenALBackend::AcquireVoice()
{
    std::uint32_t index;
    if (!m_freeVoiceSlots.empty()) {
        index = m_freeVoiceSlots.back();
        m_freeVoiceSlots.pop_back();
    }
    else {
        ConfigurePositional(m_voices.emplace_back());
        index = static_cast<std::uint32_t>(m_voices.size() - 1);
    }
    return {index + 1};
}

void MagnumOpenALBackend::ReleaseVoice(VoiceHandle voice)
{
    if (voice.id == 0) return;
    m_voices[voice.id - 1].stop();
    m_freeVoiceSlots.push_back(voice.id - 1);
}

void MagnumOpenALBackend::PlayOneShot(VoiceHandle voice, SoundBufferHandle buffer, const Vector2& pos, float gain)
{
    if (voice.id == 0 || buffer.id == 0) return;
    m_voices[voice.id - 1].stop()
            .setBuffer(&m_buffers[buffer.id - 1])
            .setLooping(false)
            .setGain(gain)
            .setPosition(Vector3{pos.x(), pos.y(), 0.f})
            .play();
}

void MagnumOpenALBackend::PlayLooping(VoiceHandle voice, SoundBufferHandle buffer, const Vector2& pos, float gain)
{
    if (voice.id == 0 || buffer.id == 0) return;
    m_voices[voice.id - 1].stop()
            .setBuffer(&m_buffers[buffer.id - 1])
            .setLooping(true)
            .setGain(gain)
            .setPosition(Vector3{pos.x(), pos.y(), 0.f})
            .play();
}

void MagnumOpenALBackend::StopVoice(VoiceHandle voice)
{
    if (voice.id == 0) return;
    m_voices[voice.id - 1].stop();
}

void MagnumOpenALBackend::SetVoicePosition(VoiceHandle voice, const Vector2& pos)
{
    if (voice.id == 0) return;
    m_voices[voice.id - 1].setPosition(Vector3{pos.x(), pos.y(), 0.f});
}

bool MagnumOpenALBackend::IsVoicePlaying(VoiceHandle voice) const
{
    if (voice.id == 0) return false;
    return m_voices[voice.id - 1].state() == Audio::Source::State::Playing;
}

void MagnumOpenALBackend::SetListenerPosition(const Vector2& pos, float height)
{
    Audio::Renderer::setListenerPosition(Vector3{pos.x(), pos.y(), height});
}

} // namespace Gravitaris
