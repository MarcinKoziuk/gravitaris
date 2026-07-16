#include <algorithm>

#include <gravitaris/game/perf-monitor.hpp>

namespace Gravitaris {

void PerfMonitor::Record(const std::string& name, const float milliseconds)
{
    auto it = m_sections.find(name);
    if (it == m_sections.end()) {
        m_order.push_back(name);
        it = m_sections.emplace(name, Section{}).first;
    }

    Section& section = it->second;
    section.lastMs = milliseconds;

    section.history[section.writeIndex] = milliseconds;
    section.writeIndex = (section.writeIndex + 1) % HISTORY_SIZE;
    if (section.sampleCount < HISTORY_SIZE) section.sampleCount++;

    float sum = 0.f;
    float maxMs = 0.f;
    for (std::size_t i = 0; i < section.sampleCount; ++i) {
        sum += section.history[i];
        maxMs = std::max(maxMs, section.history[i]);
    }
    section.avgMs = section.sampleCount > 0 ? sum / static_cast<float>(section.sampleCount) : 0.f;
    section.maxMs = maxMs;
}

const PerfMonitor::Section* PerfMonitor::GetSection(const std::string& name) const
{
    const auto it = m_sections.find(name);
    return it != m_sections.end() ? &it->second : nullptr;
}

} // namespace Gravitaris
