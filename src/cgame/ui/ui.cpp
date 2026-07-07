#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include <gravitaris/ui/ui.hpp>

#include "detail/system-interface.hpp"
#include "detail/file-interface.hpp"
#include "detail/render-interface-gl3.hpp"

namespace Gravitaris {


UI::UI(IFilesystem& filesystem)
    : m_context(nullptr)
    , m_systemInterface(std::make_unique<SystemInterface>())
    , m_fileInterface(std::make_unique<FileInterface>(filesystem))
    , m_renderInterfaceGl3(std::make_unique<RenderInterfaceGL3>())
{}

UI::~UI()
{
    if (m_context) {
        Rml::RemoveContext(m_context->GetName());

        Rml::Shutdown();
    }
}

bool UI::Init()
{
    Rml::SetSystemInterface(m_systemInterface.get());
    Rml::SetFileInterface(m_fileInterface.get());
    Rml::SetRenderInterface(m_renderInterfaceGl3.get());

    if (!Rml::Initialise()) return false;

    // Placeholder size; corrected on the first Render() call to the real
    // framebuffer size once the window/context is up.
    m_context = Rml::CreateContext("default", Rml::Vector2i(1280, 720));

    Rml::LoadFontFace("ui/LatoLatin-Regular.ttf");
    Rml::LoadFontFace("ui/LatoLatin-Bold.ttf");
    Rml::LoadFontFace("ui/LatoLatin-BoldItalic.ttf");
    Rml::LoadFontFace("ui/LatoLatin-Italic.ttf");

   // m_context->Update();//?

    Rml::ElementDocument* doc = m_context->LoadDocument("ui/demo.rml");
    if (doc) doc->Show();

    m_context->ProcessMouseMove(0, 0, 0);

    Rml::Debugger::SetVisible(true);

    // tODO unload

    return true;
}

void UI::Update()
{
    m_context->Update();
}

void UI::Render(int width, int height)
{
    const Rml::Vector2i dimensions{width, height};
    if (m_context->GetDimensions() != dimensions) {
        m_context->SetDimensions(dimensions);
    }

    m_renderInterfaceGl3->SetViewport(width, height);
    m_renderInterfaceGl3->BeginFrame();

    m_context->Render();

    m_renderInterfaceGl3->EndFrame();
}

} // namespace Gravitaris
