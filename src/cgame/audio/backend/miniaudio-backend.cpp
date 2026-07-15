#include <gravitaris/game/logging.hpp>

#include <gravitaris/cgame/audio/backend/miniaudio-backend.hpp>

namespace Gravitaris {

namespace {

// Same tuning as MagnumOpenALBackend, in miniaudio's terms: min distance is
// the inverse-model's equivalent of OpenAL's reference distance.
constexpr float REFERENCE_DISTANCE = 250.f;
constexpr float MAX_DISTANCE = 3000.f;

void ConfigurePositional(ma_sound* sound)
{
    ma_sound_set_attenuation_model(sound, ma_attenuation_model_inverse);
    ma_sound_set_rolloff(sound, 1.f);
    ma_sound_set_min_distance(sound, REFERENCE_DISTANCE);
    ma_sound_set_max_distance(sound, MAX_DISTANCE);
}

} // namespace

MiniaudioBackend::~MiniaudioBackend()
{
    // Sounds (and the buffer refs they point to) reference the engine
    // internally; uninit them before tearing down the engine itself.
    for (VoiceSlot& voice : m_voices) {
        if (voice.initialized) {
            ma_sound_uninit(voice.sound.get());
            ma_audio_buffer_ref_uninit(voice.bufferRef.get());
        }
    }
    if (m_engineInitialized) ma_engine_uninit(&m_engine);
}

bool MiniaudioBackend::Init()
{
    ma_engine_config config = ma_engine_config_init();
    const ma_result result = ma_engine_init(&config, &m_engine);
    if (result != MA_SUCCESS) {
        LOG(warning) << "[audio][miniaudio] ma_engine_init failed: " << int(result);
        return false;
    }
    m_engineInitialized = true;
    return true;
}

SoundBufferHandle MiniaudioBackend::UploadBuffer(
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

    BufferSlot& slot = m_buffers[index];
    slot.samples.assign(samples, samples + sampleCount);
    slot.sampleRate = sampleRate;
    slot.initialized = true;
    return {index + 1};
}

void MiniaudioBackend::ReleaseBuffer(SoundBufferHandle buffer)
{
    if (buffer.id == 0) return;
    BufferSlot& slot = m_buffers[buffer.id - 1];
    slot.samples.clear();
    slot.samples.shrink_to_fit();
    slot.initialized = false;
    m_freeBufferSlots.push_back(buffer.id - 1);
}

VoiceHandle MiniaudioBackend::AcquireVoice()
{
    std::uint32_t index;
    if (!m_freeVoiceSlots.empty()) {
        index = m_freeVoiceSlots.back();
        m_freeVoiceSlots.pop_back();
    }
    else {
        m_voices.emplace_back();
        index = static_cast<std::uint32_t>(m_voices.size() - 1);
    }
    return {index + 1};
}

void MiniaudioBackend::ReleaseVoice(VoiceHandle voice)
{
    if (voice.id == 0) return;
    VoiceSlot& slot = m_voices[voice.id - 1];
    if (slot.initialized) {
        ma_sound_uninit(slot.sound.get());
        ma_audio_buffer_ref_uninit(slot.bufferRef.get());
        slot.initialized = false;
    }
    m_freeVoiceSlots.push_back(voice.id - 1);
}

bool MiniaudioBackend::RebindVoice(VoiceSlot& voice, const BufferSlot& buffer)
{
    if (!buffer.initialized) return false;

    if (voice.initialized) {
        ma_sound_uninit(voice.sound.get());
        ma_audio_buffer_ref_uninit(voice.bufferRef.get());
        voice.initialized = false;
    }

    // Fresh ref per play: its own cursor (starts at 0, see
    // ma_audio_buffer_ref_init), independent of any other voice reading the
    // same underlying `buffer.samples` -- this is the fix for one clip only
    // producing sound from its first concurrent instance (see class comment).
    ma_audio_buffer_ref_init(ma_format_s16, 1, buffer.samples.data(),
            static_cast<ma_uint64>(buffer.samples.size()), voice.bufferRef.get());
    voice.bufferRef->sampleRate = buffer.sampleRate; // not settable via _init(), see its implementation

    voice.initialized = ma_sound_init_from_data_source(&m_engine,
            reinterpret_cast<ma_data_source*>(voice.bufferRef.get()), 0, nullptr, voice.sound.get()) == MA_SUCCESS;
    if (!voice.initialized) {
        ma_audio_buffer_ref_uninit(voice.bufferRef.get());
    }
    return voice.initialized;
}

void MiniaudioBackend::PlayOneShot(VoiceHandle voice, SoundBufferHandle buffer, const Vector2& pos, float gain)
{
    if (voice.id == 0 || buffer.id == 0) return;
    VoiceSlot& v = m_voices[voice.id - 1];
    if (!RebindVoice(v, m_buffers[buffer.id - 1])) return;

    ConfigurePositional(v.sound.get());
    ma_sound_set_looping(v.sound.get(), MA_FALSE);
    ma_sound_set_volume(v.sound.get(), gain);
    ma_sound_set_position(v.sound.get(), pos.x(), pos.y(), 0.f);
    ma_sound_start(v.sound.get());
}

void MiniaudioBackend::PlayLooping(VoiceHandle voice, SoundBufferHandle buffer, const Vector2& pos, float gain)
{
    if (voice.id == 0 || buffer.id == 0) return;
    VoiceSlot& v = m_voices[voice.id - 1];
    if (!RebindVoice(v, m_buffers[buffer.id - 1])) return;

    ConfigurePositional(v.sound.get());
    ma_sound_set_looping(v.sound.get(), MA_TRUE);
    ma_sound_set_volume(v.sound.get(), gain);
    ma_sound_set_position(v.sound.get(), pos.x(), pos.y(), 0.f);
    ma_sound_start(v.sound.get());
}

void MiniaudioBackend::StopVoice(VoiceHandle voice)
{
    if (voice.id == 0) return;
    VoiceSlot& v = m_voices[voice.id - 1];
    if (v.initialized) ma_sound_stop(v.sound.get());
}

void MiniaudioBackend::SetVoicePosition(VoiceHandle voice, const Vector2& pos)
{
    if (voice.id == 0) return;
    VoiceSlot& v = m_voices[voice.id - 1];
    if (v.initialized) ma_sound_set_position(v.sound.get(), pos.x(), pos.y(), 0.f);
}

bool MiniaudioBackend::IsVoicePlaying(VoiceHandle voice) const
{
    if (voice.id == 0) return false;
    const VoiceSlot& v = m_voices[voice.id - 1];
    return v.initialized && ma_sound_is_playing(v.sound.get());
}

void MiniaudioBackend::SetListenerPosition(const Vector2& pos, float height)
{
    ma_engine_listener_set_position(&m_engine, 0, pos.x(), pos.y(), height);
}

} // namespace Gravitaris
