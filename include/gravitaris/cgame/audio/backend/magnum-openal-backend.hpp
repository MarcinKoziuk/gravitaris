#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <Magnum/Audio/Context.h>
#include <Magnum/Audio/Buffer.h>
#include <Magnum/Audio/Source.h>

#include <gravitaris/cgame/audio/audio-backend.hpp>

namespace Gravitaris {

// OpenAL Soft via Magnum::Audio. NOTE (see docs/adr/0003-audio-backend.md):
// OpenAL Soft is statically linked into both this exe and MagnumAudio.dll, so
// each has its own copy of OpenAL's global current-context state. The context
// MUST be created by Magnum's tryCreate() (which runs inside the DLL, the
// same copy the inline Buffer/Source/Renderer playback calls resolve
// against); creating it with raw alc* from this translation unit instead
// targets the exe's copy and yields silence. For the same reason, never probe
// alcGetCurrentContext() from here -- it reads the exe's copy and falsely
// reports null.
class MagnumOpenALBackend : public IAudioBackend {
private:
    // Declared first so it's destroyed LAST -- the Buffer/Source vectors below
    // call alDeleteBuffers/Sources in their destructors and need the context
    // still alive.
    //
    // Deferred (optional + NoCreate): Buffer/Source construction calls
    // alGenBuffers/alGenSources, which need a current context; Context has no
    // create-later mode other than NoCreate.
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
