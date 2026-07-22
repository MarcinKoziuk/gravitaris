#pragma once

#include <array>
#include <cstddef>

namespace Gravitaris {

// Fixed-size rolling sample buffer for the Net debug tab's graphs -- same
// ring-buffer shape as PerfMonitor::Section, kept separate rather than
// shoehorned into that (frame-timing-specific, ms-labeled) class for data
// that's ticks or world units, not milliseconds.
struct RollingHistory {
    static constexpr std::size_t SIZE = 120;
    std::array<float, SIZE> samples{};
    std::size_t writeIndex = 0;
    std::size_t sampleCount = 0;

    void Record(float value)
    {
        samples[writeIndex] = value;
        writeIndex = (writeIndex + 1) % SIZE;
        if (sampleCount < SIZE) ++sampleCount;
    }
};

} // namespace Gravitaris
