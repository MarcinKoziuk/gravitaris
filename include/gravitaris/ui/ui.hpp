#pragma once

#include <memory>
#include <string>

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

    void SetDimensions(int width, int height);

    void SetDensityIndependentPixelRatio(float ratio);

    bool ProcessMouseMove(int x, int y);
    bool ProcessMouseButton(int rmlButtonIndex, bool down);

    // Exposes an engine-owned GL texture to RML/RCSS as src="live://name"
    // (see RenderInterfaceGL3::RegisterLiveTexture). Register before the
    // first frame that renders a document referencing it.
    void RegisterLiveTexture(const std::string& name, unsigned glTextureId, int width, int height);

    void ToggleDebugger();
};

} // namespace Gravitaris
