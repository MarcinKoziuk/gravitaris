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
    m_context.emplace(Magnum::NoCreate);
    if (!m_context->tryCreate(Audio::Context::Configuration{})) {
        LOG(warning) << "[audio][openal] no OpenAL device";
        m_context.reset();
        return false;
    }

    // Context::tryCreate() calls alcMakeContextCurrent() but never checks its
    // return value (see class comment) -- verify directly rather than
    // trusting Magnum's own bookkeeping.
    if (alcGetCurrentContext() == nullptr) {
        LOG(warning) << "[audio][openal] context did not become current";
        m_context.reset();
        return false;
    }

    return true;
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
