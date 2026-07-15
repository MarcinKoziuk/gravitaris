#include <cstring>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/resource/audio-clip.hpp>

namespace Gravitaris {

ResourcePtr<const AudioClip> AudioClip::placeholder = MakeResourcePtr<AudioClip>("audio-clip-placeholder"_id);

std::size_t AudioClip::CalculateSize() const
{
    return sizeof(*this) + PodContainerSize(m_samples);
}

ResourcePtr<const AudioClip> AudioClip::Placeholder()
{
    return AudioClip::placeholder;
}

bool AudioClip::InitFromWav(const std::vector<std::uint8_t>& bytes)
{
    auto u16 = [&](std::size_t o) { return std::uint16_t(bytes[o] | bytes[o + 1] << 8); };
    auto u32 = [&](std::size_t o) {
        return std::uint32_t(bytes[o] | bytes[o + 1] << 8 | bytes[o + 2] << 16 | bytes[o + 3] << 24);
    };

    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    std::uint16_t channels = 0, bitsPerSample = 0, format = 0;
    std::uint32_t sampleRate = 0;
    std::size_t dataOffset = 0, dataSize = 0;

    // Walk chunks; fmt may be followed by LIST/fact/etc. before data.
    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::uint32_t chunkSize = u32(pos + 4);
        if (std::memcmp(bytes.data() + pos, "fmt ", 4) == 0 && pos + 24 <= bytes.size()) {
            format = u16(pos + 8);
            channels = u16(pos + 10);
            sampleRate = u32(pos + 12);
            bitsPerSample = u16(pos + 22);
        }
        else if (std::memcmp(bytes.data() + pos, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataSize = chunkSize;
        }
        pos += 8 + chunkSize + (chunkSize & 1); // chunks are word-aligned
    }

    if (format != 1 || bitsPerSample != 16 || channels < 1 || channels > 2
        || dataOffset == 0 || dataOffset + dataSize > bytes.size()) {
        return false;
    }

    const auto* src = reinterpret_cast<const std::int16_t*>(bytes.data() + dataOffset);
    const std::size_t frames = dataSize / (2 * channels);

    m_sampleRate = sampleRate;
    m_samples.resize(frames);
    if (channels == 1) {
        std::memcpy(m_samples.data(), src, frames * 2);
    }
    else {
        for (std::size_t i = 0; i < frames; ++i) {
            m_samples[i] = static_cast<std::int16_t>((int(src[i * 2]) + int(src[i * 2 + 1])) / 2);
        }
    }
    return true;
}

ResourcePtr<const AudioClip> AudioClip::Create(id_t id, LoadingContext& context)
{
    std::vector<std::uint8_t> bytes;
    if (!context.filesystem.ReadBytes(id, &bytes)) {
        LOG(error) << "[audio-clip] could not read id " << id;
        return nullptr;
    }

    auto clip = MakeResourcePtr<AudioClip>(id);
    if (!clip->InitFromWav(bytes)) {
        LOG(error) << "[audio-clip] unsupported wav (want PCM16 mono/stereo), id " << id;
        return nullptr;
    }
    return clip;
}

} // namespace Gravitaris
