#pragma once

#include <cstdint>

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

using Magnum::Vector2;

// Opaque handles only -- callers (AudioSystem) never touch a backend-native
// type (ALuint, ma_sound, ...) directly. `id == 0` means invalid/unset.
struct SoundBufferHandle { std::uint32_t id = 0; };
struct VoiceHandle       { std::uint32_t id = 0; };

// Backend-agnostic positional audio: gameplay code goes through this, never
// through Magnum::Audio or miniaudio directly, so backends can be swapped
// (see docs/adr/0003-audio-backend.md) without touching AudioSystem.
class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    // false means genuinely unusable, not "usable but degraded" -- the
    // caller falls back to a different backend or disables audio entirely,
    // it does not keep calling into a backend whose Init() returned false.
    [[nodiscard]] virtual bool Init() = 0;

    [[nodiscard]] virtual const char* Name() const = 0;

    [[nodiscard]] virtual SoundBufferHandle UploadBuffer(
            const std::int16_t* samples, std::size_t sampleCount, std::uint32_t sampleRate) = 0;
    virtual void ReleaseBuffer(SoundBufferHandle) = 0;

    [[nodiscard]] virtual VoiceHandle AcquireVoice() = 0;
    virtual void ReleaseVoice(VoiceHandle) = 0;

    virtual void PlayOneShot(VoiceHandle voice, SoundBufferHandle buffer, const Vector2& pos, float gain) = 0;
    virtual void PlayLooping(VoiceHandle voice, SoundBufferHandle buffer, const Vector2& pos, float gain) = 0;
    virtual void StopVoice(VoiceHandle voice) = 0;
    virtual void SetVoicePosition(VoiceHandle voice, const Vector2& pos) = 0;
    [[nodiscard]] virtual bool IsVoicePlaying(VoiceHandle voice) const = 0;

    virtual void SetListenerPosition(const Vector2& pos, float height) = 0;
};

} // namespace Gravitaris
