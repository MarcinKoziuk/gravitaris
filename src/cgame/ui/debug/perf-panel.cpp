#include <algorithm>
#include <cfloat>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include <gravitaris/cgame/cgame.hpp>
#include <gravitaris/game/perf-monitor.hpp>

#include "perf-panel.hpp"

namespace Gravitaris {

namespace {

constexpr float PI = 3.14159265358979323846f;

// Minecraft's F3 profiler pie derives each slice's color from a hash of its
// label instead of a hardcoded palette, so any section (present or future)
// gets a color for free and it stays the same slice-to-slice. Same idea here.
ImU32 ColorForSlice(const std::string& name)
{
    const std::size_t hash = std::hash<std::string>{}(name);
    const float hue = static_cast<float>(hash % 360) / 360.f;
    return ImGui::ColorConvertFloat4ToU32(static_cast<ImVec4>(ImColor::HSV(hue, 0.55f, 0.85f)));
}

struct Slice {
    std::string label;
    float ms;
    ImU32 color;
};

// Pie chart + color-keyed legend (name, %, ms), Minecraft-debug-screen style.
// `slices` need not be sorted; largest-first just reads better.
void DrawPieChart(const std::vector<Slice>& slices, const float radius)
{
    float total = 0.f;
    for (const Slice& slice : slices) total += slice.ms;
    if (total <= 0.f) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 topLeft = ImGui::GetCursorScreenPos();
    const ImVec2 center{topLeft.x + radius, topLeft.y + radius};

    float angle = -PI / 2.f; // start at 12 o'clock, sweep clockwise
    for (const Slice& slice : slices) {
        const float nextAngle = angle + (slice.ms / total) * 2.f * PI;

        drawList->PathLineTo(center);
        drawList->PathArcTo(center, radius, angle, nextAngle);
        drawList->PathFillConvex(slice.color);

        angle = nextAngle;
    }
    // Faint outline so adjacent similar-hued slices stay visually separated.
    drawList->AddCircle(center, radius, IM_COL32(0, 0, 0, 100), 0, 1.5f);

    ImGui::Dummy(ImVec2(radius * 2.f, radius * 2.f));
    ImGui::SameLine();

    ImGui::BeginGroup();
    for (const Slice& slice : slices) {
        ImGui::ColorButton(("##swatch-" + slice.label).c_str(), ImGui::ColorConvertU32ToFloat4(slice.color),
                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(12.f, 12.f));
        ImGui::SameLine();
        ImGui::Text("%-13s %5.1f%%  %5.2f ms", slice.label.c_str(), 100.f * slice.ms / total, slice.ms);
    }
    ImGui::EndGroup();
}

void DrawSectionRow(const PerfMonitor& perf, const std::string& name)
{
    const PerfMonitor::Section* section = perf.GetSection(name);
    if (!section) {
        ImGui::TextDisabled("%s: no data yet", name.c_str());
        return;
    }

    ImGui::Text("%-20s %6.2f ms  (avg %6.2f, max %6.2f)",
                name.c_str(), section->lastMs, section->avgMs, section->maxMs);

    // history is a ring buffer; the visual "jump" at the wrap point is an
    // acceptable tradeoff here for a dev overlay over reshuffling every frame.
    const std::string plotLabel = "##" + name;
    ImGui::PlotLines(plotLabel.c_str(), section->history.data(),
                      static_cast<int>(section->sampleCount), static_cast<int>(section->writeIndex),
                      nullptr, 0.f, FLT_MAX, ImVec2(0.f, 32.f));
}

} // namespace

void DrawPerformancePanel(CGame& game)
{
    const PerfMonitor& perf = game.GetPerfMonitor();

    const PerfMonitor::Section* frame = perf.GetSection("Frame");
    if (!frame || frame->avgMs <= 0.f) {
        ImGui::TextDisabled("Gathering data...");
        return;
    }

    ImGui::Text("FPS: %.1f", 1000.f / frame->avgMs);
    ImGui::SameLine();
    ImGui::TextDisabled("(frame %.2f ms avg, %.2f ms max)", frame->avgMs, frame->maxMs);

    const auto avgOf = [&perf](const std::string& name) -> float {
        const PerfMonitor::Section* section = perf.GetSection(name);
        return section ? section->avgMs : 0.f;
    };

    // Physics/Game Logic run at most once per real frame (see Game::Update's
    // callers), and everything else runs exactly once per frame, so summing
    // averages here is directly comparable to the "Frame" average below.
    std::vector<Slice> slices;
    const auto addSlice = [&](const std::string& label, const float ms) {
        if (ms > 0.f) slices.push_back(Slice{label, ms, ColorForSlice(label)});
    };
    addSlice("Physics", avgOf("Physics"));
    addSlice("Game Logic", avgOf("Game Logic"));
    addSlice("Rendering", avgOf("Rendering"));
    addSlice("Post-process", avgOf("Post-process Begin") + avgOf("Post-process Composite"));
    addSlice("Audio", avgOf("Audio"));
    addSlice("UI", avgOf("UI Update") + avgOf("UI Render"));
    addSlice("Debug UI", avgOf("Debug UI"));

    std::sort(slices.begin(), slices.end(), [](const Slice& a, const Slice& b) { return a.ms > b.ms; });

    float tracked = 0.f;
    for (const Slice& slice : slices) tracked += slice.ms;

    // Whatever the frame isn't spending in a tracked section -- mostly
    // vsync/present wait, same as "idle" on Minecraft's profiler pie.
    const float idle = std::max(0.f, frame->avgMs - tracked);
    if (idle > 0.f) {
        // Fixed neutral color: not a real section, and a hashed hue could
        // coincidentally clash with one.
        slices.push_back(Slice{"Idle", idle, IM_COL32(90, 90, 90, 255)});
    }

    ImGui::Spacing();
    DrawPieChart(slices, 70.f);

    if (ImGui::CollapsingHeader("Section details")) {
        DrawSectionRow(perf, "Physics");
        DrawSectionRow(perf, "Game Logic");
        DrawSectionRow(perf, "Rendering");
        DrawSectionRow(perf, "Post-process Begin");
        DrawSectionRow(perf, "Post-process Composite");
        DrawSectionRow(perf, "Audio");
        DrawSectionRow(perf, "UI Update");
        DrawSectionRow(perf, "UI Render");
        DrawSectionRow(perf, "Debug UI");
    }
}

} // namespace Gravitaris
