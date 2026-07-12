#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/Math/Vector2.h>
#include <Magnum/ImGuiIntegration/Context.hpp>

namespace Gravitaris {

class CGame;
class GlowPostProcess;

// Dear ImGui-based developer overlay. Owns the ImGui context, hosts the
// tweakable panels (post-process, renderer, spawn) in a tabbed window, and
// forwards input events. Drawn last in the frame, on top of everything and
// bypassing the CRT/glow passes so the controls stay crisp.
//
// Toggle visibility with the app's debug key (F1). While hidden it captures
// no input and draws nothing.
//
// Lives here in ui/debug/ (impl detail, like ui/detail/) rather than the
// public include/ tree since it's dev-only tooling.
class DebugUi {
private:
    Magnum::ImGuiIntegration::Context m_imgui{Magnum::NoCreate};

    // Non-owning; both outlive DebugUi (owned by the application).
    CGame& m_game;
    GlowPostProcess& m_glow;

    bool m_visible = false;

    void BuildFrame();

public:
    // Must be constructed after the GL context exists. sizes: see
    // ImGuiIntegration::Context — uiSize is logical UI size, framebufferSize
    // the physical backing size (differ on HiDPI).
    DebugUi(CGame& game, GlowPostProcess& glow,
            const Magnum::Vector2& uiSize,
            const Magnum::Vector2i& windowSize,
            const Magnum::Vector2i& framebufferSize);

    void Toggle() { m_visible = !m_visible; }
    void SetVisible(bool visible) { m_visible = visible; }
    [[nodiscard]] bool IsVisible() const { return m_visible; }

    // Re-layout on window/framebuffer resize.
    void Relayout(const Magnum::Vector2& uiSize,
                  const Magnum::Vector2i& windowSize,
                  const Magnum::Vector2i& framebufferSize);

    // Build the widgets and render them to the currently bound framebuffer.
    // No-op while hidden.
    void Draw();

    // Whether ImGui currently wants the corresponding input, so the app can
    // withhold it from the game. All false while hidden.
    [[nodiscard]] bool WantsMouse() const;
    [[nodiscard]] bool WantsKeyboard() const;
    [[nodiscard]] bool WantsTextInput() const;

    // Event forwarding. Each returns true if ImGui consumed the event.
    // Templated to avoid coupling this header to the concrete SDL2 event
    // types (matches ImGuiIntegration's own API). No-ops (return false) while
    // hidden so gameplay input is unaffected.
    template<class Event> bool HandlePointerPress(Event& e)   { return m_visible && m_imgui.handlePointerPressEvent(e); }
    template<class Event> bool HandlePointerRelease(Event& e) { return m_visible && m_imgui.handlePointerReleaseEvent(e); }
    template<class Event> bool HandlePointerMove(Event& e)    { return m_visible && m_imgui.handlePointerMoveEvent(e); }
    template<class Event> bool HandleScroll(Event& e)         { return m_visible && m_imgui.handleScrollEvent(e); }
    template<class Event> bool HandleKeyPress(Event& e)       { return m_visible && m_imgui.handleKeyPressEvent(e); }
    template<class Event> bool HandleKeyRelease(Event& e)     { return m_visible && m_imgui.handleKeyReleaseEvent(e); }
    template<class Event> bool HandleTextInput(Event& e)      { return m_visible && m_imgui.handleTextInputEvent(e); }
};

} // namespace Gravitaris
