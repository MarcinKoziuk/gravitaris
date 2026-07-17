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

namespace Gravitaris {

using namespace Magnum;

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
    ImGui::SetNextWindowSize(ImVec2(380.f, 460.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(24.f, 24.f), ImGuiCond_FirstUseEver);

    // Passing &m_visible gives the window a close button that hides the overlay.
    if (ImGui::Begin("Gravitaris Dev", &m_visible)) {
        ImGui::TextDisabled("Ctrl+click or double-click any slider to type an exact value.");
        ImGui::Separator();

        if (ImGui::BeginTabBar("##debug-tabs")) {
            if (ImGui::BeginTabItem("Post-process")) {
                DrawPostProcessPanel(m_glow);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Renderer")) {
                DrawRendererPanel(m_game);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio")) {
                DrawAudioPanel(m_game);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Spawn")) {
                DrawSpawnPanel(m_game);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Trajectory")) {
                DrawTrajectoryPanel(m_game);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Flight")) {
                DrawFlightPanel(m_game);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Physics")) {
                DrawPhysicsPanel(m_game);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Camera")) {
                DrawCameraPanel(m_game);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("HUD")) {
                DrawHudPanel(m_game);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Starfield")) {
                DrawStarfieldPanel(m_game);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Performance")) {
                DrawPerformancePanel(m_game);
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
    if (!m_visible) return;

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
