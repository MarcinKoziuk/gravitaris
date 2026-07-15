#pragma once

#include <cstdint>
#include <vector>

// ALCdevice/ALCcontext only; forward-declaring them ourselves would fight
// the real header's typedefs, so include it directly (small, C, no fallout).
#include <AL/alc.h>

#include <Magnum/Audio/Buffer.h>
#include <Magnum/Audio/Source.h>

#include <gravitaris/cgame/audio/audio-backend.hpp>

namespace Gravitaris {

// OpenAL Soft, driven directly via ALC rather than through
// Magnum::Audio::Context. See docs/adr/0003-audio-backend.md: instrumentation
// proved Context::tryCreate() can report success and even call
// alcMakeContextCurrent() with a valid context, yet alcGetCurrentContext()
// reads back null immediately after -- reproduced on Windows, not just the
// macOS/CoreAudio setup the ADR originally attributed it to. A hand-rolled
// ALC sequence with the identical device specifier and attributes does not
// exhibit this, so Init() owns the device/context itself; Audio::Buffer/
// Audio::Source only need *some* context current and don't go through
// Context::tryCreate() at all, so they're unaffected and still used as-is.
class MagnumOpenALBackend : public IAudioBackend {
private:
    ALCdevice* m_device = nullptr;
    ALCcontext* m_alContext = nullptr;

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
    ~MagnumOpenALBackend() override;

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
