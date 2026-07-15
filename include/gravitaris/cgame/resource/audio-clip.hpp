#pragma once

#include <cstdint>
#include <vector>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/resource/common/iresource.hpp>
#include <gravitaris/game/resource/common/resource-ptr.hpp>

namespace Gravitaris {

// Decoded audio asset: mono PCM16, fully resident. Only for short one-shot/
// looping SFX (gunshot, hit, thruster loop) -- a multi-minute music track
// wants streaming decode, not a fully-decoded ResourceLoader resource; that's
// a separate abstraction for later (see IDEAS.md).
class AudioClip : public IResource {
private:
    std::vector<std::int16_t> m_samples;
    std::uint32_t m_sampleRate = 0;

    static ResourcePtr<const AudioClip> placeholder;

    // Only canonical RIFF/WAVE, PCM 16-bit, mono or stereo (stereo is
    // downmixed -- positional playback only ever sends mono to the backend).
    // Anything fancier should go through a real decoder later.
    [[nodiscard]] bool InitFromWav(const std::vector<std::uint8_t>& bytes);

public:
    AudioClip() = default;

    [[nodiscard]] std::size_t CalculateSize() const override;

    [[nodiscard]] const char* GetResourceName() const override
    { return "audio-clip"; }

    [[nodiscard]] const std::vector<std::int16_t>& GetSamples() const
    { return m_samples; }

    [[nodiscard]] std::uint32_t GetSampleRate() const
    { return m_sampleRate; }

    static ResourcePtr<const AudioClip> Placeholder();

    static ResourcePtr<const AudioClip> Create(id_t id, LoadingContext& context);
};

} // namespace Gravitaris
