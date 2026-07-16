#pragma once

#include <array>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace Gravitaris {

// Lightweight per-frame timing tracker for the dev performance overlay. Not a
// production profiler: wall-clock (not GPU) time via a plain unordered_map,
// fine at debug-UI section counts and refresh rates.
//
// Sections that only run some frames (e.g. the fixed-timestep physics step,
// skipped when the frame arrives early) simply advance their own history at
// their own rate -- each section's rolling stats stay internally consistent
// even though sections aren't all sampled the same number of times.
class PerfMonitor {
public:
    static constexpr std::size_t HISTORY_SIZE = 120;

    struct Section {
        float lastMs = 0.f;
        float avgMs = 0.f;
        float maxMs = 0.f;
        std::array<float, HISTORY_SIZE> history{};
        std::size_t writeIndex = 0;
        std::size_t sampleCount = 0; // capped at HISTORY_SIZE
    };

    // Records one timing sample (milliseconds) under `name`, updating that
    // section's rolling history/average/max (creating the section on first use).
    void Record(const std::string& name, float milliseconds);

    // Section names in first-Record()-call order, for stable panel layout.
    [[nodiscard]] const std::vector<std::string>& SectionNames() const { return m_order; }

    // Null if `name` has never been recorded.
    [[nodiscard]] const Section* GetSection(const std::string& name) const;

private:
    std::unordered_map<std::string, Section> m_sections;
    std::vector<std::string> m_order;
};

// RAII wall-clock scope timer; records elapsed time into `monitor` under
// `name` when the timer goes out of scope.
class ScopedPerfTimer {
public:
    ScopedPerfTimer(PerfMonitor& monitor, std::string name)
        : m_monitor(monitor)
        , m_name(std::move(name))
        , m_start(Clock::now())
    {}

    ~ScopedPerfTimer()
    {
        const float ms = std::chrono::duration<float, std::milli>(Clock::now() - m_start).count();
        m_monitor.Record(m_name, ms);
    }

    ScopedPerfTimer(const ScopedPerfTimer&) = delete;
    ScopedPerfTimer& operator=(const ScopedPerfTimer&) = delete;

private:
    using Clock = std::chrono::high_resolution_clock;

    PerfMonitor& m_monitor;
    std::string m_name;
    Clock::time_point m_start;
};

} // namespace Gravitaris
