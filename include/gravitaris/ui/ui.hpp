#pragma once

#include <gravitaris/game/fs/ifilesystem.hpp>

namespace Rml {
class Context;
}

namespace Gravitaris {

class SystemInterface;
class FileInterface;
class RenderInterfaceGL3;

class UI {
private:
    Rml::Context* m_context;

    std::unique_ptr<SystemInterface> m_systemInterface;

    std::unique_ptr<FileInterface> m_fileInterface;

    std::unique_ptr<RenderInterfaceGL3> m_renderInterfaceGl3;

public:
    UI(IFilesystem& filesystem);

    ~UI();

    void Update();

    // width/height are the real framebuffer size in pixels (not the
    // window's logical/point size), so the UI stays pixel-accurate on
    // HiDPI/Retina displays where the two differ.
    void Render(int width, int height);

    bool Init();
};

} // namespace Gravitaris
