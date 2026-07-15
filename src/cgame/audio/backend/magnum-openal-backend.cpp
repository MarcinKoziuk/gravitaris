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
    // Let Magnum create the context -- do NOT open the device / make a context
    // current with raw alc* here. OpenAL Soft is statically linked into BOTH
    // this exe and MagnumAudio.dll (two private copies of OpenAL's global
    // "current context" state, since a shared OpenAL32.dll would collide with
    // the legacy Creative router in System32 -- see CMakeLists). The live
    // playback calls (Buffer::setData, Source::play, Renderer::* -- all inline
    // in Magnum's headers but reached through the same OpenAL Soft the DLL
    // bundles) resolve against the DLL's copy, so the context must be made
    // current in THAT copy: tryCreate(), compiled into the DLL, does exactly
    // that. A prior attempt to own the device/context via raw alc* in this TU
    // pointed a context at the exe's copy instead and produced dead silence.
    //
    // Likewise do not "verify" with alcGetCurrentContext() here: that call,
    // compiled into the exe, reads the exe's copy and always sees null even
    // when the DLL's context is live -- a cross-module false negative that
    // was previously misread as a Magnum/tryCreate bug. See
    // docs/adr/0003-audio-backend.md.
    m_context.emplace(Magnum::NoCreate);
    if (!m_context->tryCreate(Audio::Context::Configuration{})) {
        LOG(warning) << "[audio][openal] no OpenAL device";
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
