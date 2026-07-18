#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace Gravitaris {

// Wire-format primitives (docs/networking-plan.md 2.1). Explicitly
// little-endian byte by byte, not memcpy-of-struct: the byte layout is the
// protocol, independent of host endianness/padding (native and wasm builds
// must interoperate).
class ByteWriter {
    std::vector<std::uint8_t> m_buffer;

public:
    void WriteU8(std::uint8_t v) { m_buffer.push_back(v); }

    void WriteU16(std::uint16_t v)
    {
        m_buffer.push_back(static_cast<std::uint8_t>(v));
        m_buffer.push_back(static_cast<std::uint8_t>(v >> 8));
    }

    void WriteU32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) m_buffer.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
    }

    void WriteU64(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i) m_buffer.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
    }

    void WriteF32(float v)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        WriteU32(bits);
    }

    // Clamps to [min, max], quantizes to 2^bits - 1 levels, writes the
    // smallest byte count that holds `bits` (1..32). Lossy by design; the
    // reader reconstructs the level midpoint-free lower value.
    void WriteQuantizedFloat(float v, float min, float max, int bits)
    {
        const std::uint32_t maxQ = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        float t = (v - min) / (max - min);
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        const std::uint32_t q = static_cast<std::uint32_t>(t * static_cast<float>(maxQ) + 0.5f);
        const int bytes = (bits + 7) / 8;
        for (int i = 0; i < bytes; ++i) m_buffer.push_back(static_cast<std::uint8_t>(q >> (i * 8)));
    }

    [[nodiscard]] const std::uint8_t* Data() const { return m_buffer.data(); }
    [[nodiscard]] std::size_t Size() const { return m_buffer.size(); }
    void Clear() { m_buffer.clear(); }
};

// Reads the ByteWriter format. Never reads past the end: an overrun read
// returns 0 and latches Ok() == false, so callers can parse a whole packet
// and check validity once at the end instead of after every field.
class ByteReader {
    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos = 0;
    bool m_overrun = false;

    bool Take(std::size_t n)
    {
        if (m_size - m_pos < n) {
            m_overrun = true;
            m_pos = m_size;
            return false;
        }
        return true;
    }

public:
    ByteReader(const std::uint8_t* data, std::size_t size)
            : m_data(data)
            , m_size(size)
    {}

    std::uint8_t ReadU8()
    {
        if (!Take(1)) return 0;
        return m_data[m_pos++];
    }

    std::uint16_t ReadU16()
    {
        if (!Take(2)) return 0;
        const std::uint16_t v = static_cast<std::uint16_t>(m_data[m_pos] | (m_data[m_pos + 1] << 8));
        m_pos += 2;
        return v;
    }

    std::uint32_t ReadU32()
    {
        if (!Take(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(m_data[m_pos + i]) << (i * 8);
        m_pos += 4;
        return v;
    }

    std::uint64_t ReadU64()
    {
        if (!Take(8)) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(m_data[m_pos + i]) << (i * 8);
        m_pos += 8;
        return v;
    }

    float ReadF32()
    {
        const std::uint32_t bits = ReadU32();
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    float ReadQuantizedFloat(float min, float max, int bits)
    {
        const int bytes = (bits + 7) / 8;
        if (!Take(static_cast<std::size_t>(bytes))) return min;
        std::uint32_t q = 0;
        for (int i = 0; i < bytes; ++i) q |= static_cast<std::uint32_t>(m_data[m_pos + i]) << (i * 8);
        m_pos += static_cast<std::size_t>(bytes);
        const std::uint32_t maxQ = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        return min + (static_cast<float>(q) / static_cast<float>(maxQ)) * (max - min);
    }

    [[nodiscard]] bool Ok() const { return !m_overrun; }
    [[nodiscard]] std::size_t Remaining() const { return m_size - m_pos; }
};

} // namespace Gravitaris
