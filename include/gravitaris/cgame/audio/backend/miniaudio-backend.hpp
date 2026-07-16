#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <miniaudio/miniaudio.h>

#include <gravitaris/cgame/audio/audio-backend.hpp>

namespace Gravitaris {

// miniaudio (CoreAudio/WASAPI/ALSA/PulseAudio/AAudio/OpenSL, all in one
// dependency-free single header) -- see docs/adr/0003-audio-backend.md.
//
// Unlike OpenAL's Source (buffer-agnostic, rebind anytime), an ma_sound is
// bound to exactly one data source at ma_sound_init_from_data_source() time;
// there is no "set buffer" call. PlayOneShot/PlayLooping re-init the
// underlying ma_sound each time a voice is (re)assigned a buffer -- fine at
// our call rate (a handful of plays/second), and it keeps VoiceHandle's
// contract ("assign any buffer, anytime") identical across backends.
//
// A buffer (BufferSlot) is NOT an ma_audio_buffer: that type owns a single
// playback cursor, so every voice bound to the same ma_audio_buffer would
// fight over one read position -- confirmed the cause of "only the first
// bullet plays" / "thruster distorts with 2+ ships" (see
// docs/adr/0003-audio-backend.md). Instead BufferSlot owns the decoded PCM
// directly, and each VOICE gets its own ma_audio_buffer_ref (a lightweight,
// non-owning data source with an independent cursor) pointing at that same
// memory -- N concurrent voices playing one clip now advance independently.
class MiniaudioBackend : public IAudioBackend {
private:
    ma_engine m_engine{};
    bool m_engineInitialized = false;

    // Decoded PCM, backend-owned (independent of the AudioClip resource's
    // lifetime, matching "upload" semantics elsewhere). Referenced by every
    // ma_audio_buffer_ref that plays this clip -- see class comment. Not
    // mutated/reallocated once uploaded except via UploadBuffer/ReleaseBuffer
    // on this exact slot, which by construction only happens with no voice
    // still reading it (see PlayOneShot/PlayLooping's re-init-per-play and
    // AudioSystem::SetBackendPreference's release-voices-before-buffers
    // order); reusing a slot for a *different* clip while an old voice is
    // still mid-playback from it is a known, currently-unhandled edge case.
    struct BufferSlot {
        std::vector<std::int16_t> samples;
        std::uint32_t sampleRate = 0;
        bool initialized = false;
    };

    // ma_sound and ma_audio_buffer_ref are referenced by raw pointer once
    // initialized (the sound's internal data-source pointer, and reportedly
    // the engine's own internal sound-list linkage) -- like Chipmunk bodies
    // (docs/adr/0002-physics-ownership.md), they need a stable address for
    // their lifetime, so each slot heap-allocates its objects once and
    // reuses them via free-list recycling; the *slot* (this struct) can
    // still live in a plain growable vector since only unique_ptrs + a bool
    // move on growth.
    struct VoiceSlot {
        std::unique_ptr<ma_sound> sound = std::make_unique<ma_sound>();
        std::unique_ptr<ma_audio_buffer_ref> bufferRef = std::make_unique<ma_audio_buffer_ref>();
        bool initialized = false;
    };

    std::vector<BufferSlot> m_buffers;
    std::vector<std::uint32_t> m_freeBufferSlots;

    std::vector<VoiceSlot> m_voices;
    std::vector<std::uint32_t> m_freeVoiceSlots;

    // Shared by PlayOneShot/PlayLooping: (re)bind `voice` to a fresh
    // ma_audio_buffer_ref over `buffer`'s PCM and (re)init the underlying
    // ma_sound from it. Returns false (voice left uninitialized) if `buffer`
    // has no data or the sound failed to init.
    bool RebindVoice(VoiceSlot& voice, const BufferSlot& buffer);

public:
    ~MiniaudioBackend() override;

    [[nodiscard]] bool Init() override;
    [[nodiscard]] const char* Name() const override { return "miniaudio"; }

    [[nodiscard]] SoundBufferHandle UploadBuffer(
            const std::int16_t* samples, std::size_t sampleCount, std::uint32_t sampleRate) override;
    void ReleaseBuffer(SoundBufferHandle) override;

    [[nodiscard]] VoiceHandle AcquireVoice() override;
    void ReleaseVoice(VoiceHandle) override;

    void PlayOneShot(VoiceHandle voice, SoundBufferHandle buffer, const Vector2& pos, float gain) override;
    void PlayLooping(VoiceHandle voice, SoundBufferHandle buffer, const Vector2& pos, float gain) override;
    void StopVoice(VoiceHandle voice) override;
    void SetVoicePosition(VoiceHandle voice, const Vector2& pos) override;
    void SetVoiceGain(VoiceHandle voice, float gain) override;
    [[nodiscard]] bool IsVoicePlaying(VoiceHandle voice) const override;

    void SetListenerPosition(const Vector2& pos, float height) override;
};

} // namespace Gravitaris
