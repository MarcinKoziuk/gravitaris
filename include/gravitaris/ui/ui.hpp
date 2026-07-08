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
    void SetDimensions(int width, int height);

    // Input forwarding from the application. Returns true if the UI consumed
    // the event (so the caller can avoid also treating it as a game input).
    bool ProcessMouseMove(int x, int y);
    bool ProcessMouseButton(int rmlButtonIndex, bool down);

    void ToggleDebugger();
};

} // namespace Gravitaris
