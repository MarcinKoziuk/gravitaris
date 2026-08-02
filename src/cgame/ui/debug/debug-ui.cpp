#include <imgui.h>

#include <Magnum/GL/Renderer.h>

#include "debug-ui.hpp"
#include "post-process-panel.hpp"
#include "renderer-panel.hpp"
#include "audio-panel.hpp"
#include "spawn-panel.hpp"
#include "trajectory-panel.hpp"
#include "flight-panel.hpp"
#include "perf-panel.hpp"
#include "starfield-panel.hpp"
#include "camera-panel.hpp"
#include "hud-panel.hpp"
#include "physics-panel.hpp"
#include "net-panel.hpp"
#include "ai-panel.hpp"
#include "ai-label-overlay.hpp"

namespace Gravitaris {

using namespace Magnum;

namespace {

// Full labels with scroll arrows rather than the default shrink-to-fit,
// which ellipsizes names away exactly when there are enough to need them.
constexpr ImGuiTabBarFlags INNER_TABS = ImGuiTabBarFlags_FittingPolicyScroll;

// Keeps a leaf tab to one line, so the grouping above stays readable as a
// table of contents.
template <typename F>
void Tab(const char* name, const F& draw)
{
    if (ImGui::BeginTabItem(name)) {
        draw();
        ImGui::EndTabItem();
    }
}

} // namespace

DebugUi::DebugUi(CGame& game, GlowPostProcess& glow,
                 const Vector2& uiSize, const Vector2i& windowSize, const Vector2i& framebufferSize)
    : m_imgui(uiSize, windowSize, framebufferSize)
    , m_game(game)
    , m_glow(glow)
    , m_uiSize(uiSize)
{}

void DebugUi::Relayout(const Vector2& uiSize, const Vector2i& windowSize, const Vector2i& framebufferSize)
{
    m_imgui.relayout(uiSize, windowSize, framebufferSize);
    m_uiSize = uiSize;
}

bool DebugUi::WantsMouse() const
{
    return m_visible && ImGui::GetIO().WantCaptureMouse;
}

bool DebugUi::WantsKeyboard() const
{
    return m_visible && ImGui::GetIO().WantCaptureKeyboard;
}

bool DebugUi::WantsTextInput() const
{
    return m_visible && ImGui::GetIO().WantTextInput;
}

void DebugUi::BuildFrame()
{
    if (m_aiLabels) DrawAiLabelOverlay(m_game, m_uiSize);
    if (!m_visible) return;

    ImGui::SetNextWindowSize(ImVec2(460.f, 520.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(24.f, 24.f), ImGuiCond_FirstUseEver);

    // Passing &m_visible gives the window a close button that hides the overlay.
    if (ImGui::Begin("Gravitaris Dev", &m_visible)) {
        ImGui::TextDisabled("Ctrl+click or double-click any slider to type an exact value.");
        ImGui::Separator();

        // Two levels, not thirteen tabs: a single bar's worth of names
        // stopped being readable well before the window is wide enough to
        // show them all.
        if (ImGui::BeginTabBar("##debug-groups")) {
            if (ImGui::BeginTabItem("Video")) {
                if (ImGui::BeginTabBar("##video", INNER_TABS)) {
                    Tab("Post-process", [&] { DrawPostProcessPanel(m_glow); });
                    Tab("Renderer", [&] { DrawRendererPanel(m_game); });
                    Tab("Starfield", [&] { DrawStarfieldPanel(m_game); });
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("View")) {
                if (ImGui::BeginTabBar("##view", INNER_TABS)) {
                    Tab("Camera", [&] { DrawCameraPanel(m_game); });
                    Tab("HUD", [&] { DrawHudPanel(m_game); });
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Sim")) {
                if (ImGui::BeginTabBar("##sim", INNER_TABS)) {
                    Tab("Spawn", [&] { DrawSpawnPanel(m_game); });
                    Tab("AI", [&] { DrawAiPanel(m_game); });
                    Tab("Flight", [&] { DrawFlightPanel(m_game); });
                    Tab("Physics", [&] { DrawPhysicsPanel(m_game); });
                    Tab("Trajectory", [&] { DrawTrajectoryPanel(m_game); });
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("System")) {
                if (ImGui::BeginTabBar("##system", INNER_TABS)) {
                    Tab("Net", [&] { DrawNetPanel(m_game); });
                    Tab("Performance", [&] { DrawPerformancePanel(m_game); });
                    Tab("Audio", [&] { DrawAudioPanel(m_game); });
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    // World-space overlays draw every frame the dev UI is up, regardless of
    // which tab is selected.
    DrawTrajectoryOverlay(m_game, m_uiSize);
    DrawFlightOverlay(m_game, m_uiSize);
}

void DebugUi::Draw()
{
    if (!m_visible && !m_aiLabels) return;

    m_imgui.newFrame();
    BuildFrame();

    // ImGui needs alpha blending + scissor, and must not be culled/depth-tested.
    GL::Renderer::enable(GL::Renderer::Feature::Blending);
    GL::Renderer::enable(GL::Renderer::Feature::ScissorTest);
    GL::Renderer::disable(GL::Renderer::Feature::FaceCulling);
    GL::Renderer::disable(GL::Renderer::Feature::DepthTest);
    GL::Renderer::setBlendEquation(GL::Renderer::BlendEquation::Add, GL::Renderer::BlendEquation::Add);
    GL::Renderer::setBlendFunction(GL::Renderer::BlendFunction::SourceAlpha,
                                   GL::Renderer::BlendFunction::OneMinusSourceAlpha);

    m_imgui.drawFrame();

    // Restore the defaults the rest of the frame expects.
    GL::Renderer::disable(GL::Renderer::Feature::ScissorTest);
    GL::Renderer::disable(GL::Renderer::Feature::Blending);
}

} // namespace Gravitaris
