#pragma once

#include <memory>

#include <gravitaris/game/fs/ifilesystem.hpp>

namespace Rml {
class Context;
class ElementDocument;
class EventListener;
}

namespace Gravitaris {

class SystemInterface;
class FileInterface;
class RenderInterfaceGL3;

class UI {
private:
    Rml::Context* m_context;
    Rml::ElementDocument* m_document = nullptr;

    std::unique_ptr<SystemInterface> m_systemInterface;

    std::unique_ptr<FileInterface> m_fileInterface;

    std::unique_ptr<RenderInterfaceGL3> m_renderInterfaceGl3;

    std::unique_ptr<Rml::EventListener> m_buttonListener;

    int m_width = 1280;
    int m_height = 720;

public:
    UI(IFilesystem& filesystem);

    ~UI();

    void Update();

    void Render();

    bool Init();

    // Keep the RmlUi context + render viewport in sync with the window, so
    // layout and mouse coordinates are correct (was previously hardcoded).
    // Callers should pass the real framebuffer size in pixels (not the
    // window's logical/point size), so the UI stays pixel-accurate on
    // HiDPI/Retina displays where the two differ.
    void SetDimensions(int width, int height);

    // RmlUi's dp-ratio: how many physical pixels one RCSS "px" covers.
    // Context dimensions are set in physical pixels (above) so the render
    // viewport covers the whole framebuffer, but without this, elements
    // sized in RCSS px render at their physical-pixel size, i.e. visually
    // shrunk by the display's DPI scale factor on HiDPI/Retina screens.
    // Pass Sdl2Application::dpiScaling() here to keep visual sizes
    // consistent across displays.
    void SetDensityIndependentPixelRatio(float ratio);

    // Input forwarding from the application. x/y must be in the same
    // physical-pixel space as SetDimensions (i.e. pre-scaled by dpiScaling()
    // if the caller's input events are in logical/window points).
    bool ProcessMouseMove(int x, int y);
    bool ProcessMouseButton(int rmlButtonIndex, bool down);

    void ToggleDebugger();
};

} // namespace Gravitaris
