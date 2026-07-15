#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <Magnum/Audio/Context.h>
#include <Magnum/Audio/Buffer.h>
#include <Magnum/Audio/Source.h>

#include <gravitaris/cgame/audio/audio-backend.hpp>

namespace Gravitaris {

// OpenAL Soft via Magnum::Audio. See docs/adr/0003-audio-backend.md: on some
// macOS/CoreAudio setups Context::tryCreate() reports success while the
// context never actually becomes current (Magnum doesn't check
// alcMakeContextCurrent()'s return value), after which every AL call fails
// with AL_INVALID_OPERATION. Init() verifies this directly instead of
// trusting tryCreate().
class MagnumOpenALBackend : public IAudioBackend {
private:
    // Deferred: Buffer/Source objects call alGenBuffers/alGenSources at
    // construction, which need a current context -- Context itself needs the
    // same treatment (its default constructor has no create-later mode other
    // than NoCreate).
    std::optional<Magnum::Audio::Context> m_context;

    // Handles are 1-based indices into these (0 = invalid, matching
    // SoundBufferHandle/VoiceHandle's default). Safe to store Buffer/Source
    // directly in a growable vector and let it reallocate/move on growth --
    // unlike Chipmunk bodies (see docs/adr/0002-physics-ownership.md) these
    // just wrap a plain AL id, no raw-pointer registration to relocate under.
    std::vector<Magnum::Audio::Buffer> m_buffers;
    std::vector<std::uint32_t> m_freeBufferSlots;

    std::vector<Magnum::Audio::Source> m_voices;
    std::vector<std::uint32_t> m_freeVoiceSlots;

public:
    ~MagnumOpenALBackend() override = default;

    [[nodiscard]] bool Init() override;
    [[nodiscard]] const char* Name() const override { return "OpenAL (Magnum::Audio)"; }

    [[nodiscard]] SoundBufferHandle UploadBuffer(
            const std::int16_t* samples, std::size_t sampleCount, std::uint32_t sampleRate) override;
    void ReleaseBuffer(SoundBufferHandle) override;

    [[nodiscard]] VoiceHandle AcquireVoice() override;
    void ReleaseVoice(VoiceHandle) override;

    void PlayOneShot(VoiceHandle voice, SoundBufferHandle buffer, const Vector2& pos, float gain) override;
    void PlayLooping(VoiceHandle voice, SoundBufferHandle buffer, const Vector2& pos, float gain) override;
    void StopVoice(VoiceHandle voice) override;
    void SetVoicePosition(VoiceHandle voice, const Vector2& pos) override;
    [[nodiscard]] bool IsVoicePlaying(VoiceHandle voice) const override;

    void SetListenerPosition(const Vector2& pos, float height) override;
};

} // namespace Gravitaris
