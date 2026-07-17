#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/ui/ui.hpp>

#include "detail/system-interface.hpp"
#include "detail/file-interface.hpp"
#include "detail/render-interface-gl3.hpp"

namespace Gravitaris {

namespace {

// Minimal listener that logs button clicks. Placeholder hook — replace the
// body (or route to a callback) when there's actual UI-driven behaviour.
class ButtonListener : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& event) override
    {
        Rml::Element* el = event.GetCurrentElement();
        LOG(info) << "[UI] button clicked: " << (el ? el->GetId() : std::string("?"));
    }
};

} // namespace

UI::UI(IFilesystem& filesystem)
    : m_context(nullptr)
    , m_systemInterface(std::make_unique<SystemInterface>())
    , m_fileInterface(std::make_unique<FileInterface>(filesystem))
    , m_renderInterfaceGl3(std::make_unique<RenderInterfaceGL3>())
    , m_buttonListener(std::make_unique<ButtonListener>())
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

    // Placeholder size; corrected by the first SetDimensions() call once the
    // window/framebuffer is up.
    m_context = Rml::CreateContext("default", Rml::Vector2i(m_width, m_height));

    Rml::LoadFontFace("ui/LatoLatin-Regular.ttf");
    Rml::LoadFontFace("ui/LatoLatin-Bold.ttf");
    Rml::LoadFontFace("ui/LatoLatin-BoldItalic.ttf");
    Rml::LoadFontFace("ui/LatoLatin-Italic.ttf");

    // HUD first so interactive windows loaded after it stack on top of it.
    if (Rml::ElementDocument* hud = m_context->LoadDocument("ui/hud.rml")) {
        hud->Show();
    }

    m_document = m_context->LoadDocument("ui/main.rml");
    if (m_document) {
        m_document->Show();

        if (Rml::Element* button = m_document->GetElementById("launch_button")) {
            button->AddEventListener("click", m_buttonListener.get());
        }
    }

    m_context->ProcessMouseMove(0, 0, 0);

    Rml::Debugger::Initialise(m_context);
    Rml::Debugger::SetVisible(false);

    return true;
}

void UI::SetDimensions(int width, int height)
{
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;
    if (m_context) {
        m_context->SetDimensions(Rml::Vector2i(width, height));
    }
}

void UI::SetDensityIndependentPixelRatio(float ratio)
{
    if (m_context) {
        m_context->SetDensityIndependentPixelRatio(ratio);
    }
}

bool UI::ProcessMouseMove(int x, int y)
{
    if (!m_context) return false;
    return !m_context->ProcessMouseMove(x, y, 0);
}

bool UI::ProcessMouseButton(int rmlButtonIndex, bool down)
{
    if (!m_context) return false;
    return down ? !m_context->ProcessMouseButtonDown(rmlButtonIndex, 0)
                : !m_context->ProcessMouseButtonUp(rmlButtonIndex, 0);
}

void UI::RegisterLiveTexture(const std::string& name, unsigned glTextureId, int width, int height)
{
    m_renderInterfaceGl3->RegisterLiveTexture(name, glTextureId, Rml::Vector2i(width, height));
}

void UI::ToggleDebugger()
{
    Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
}

void UI::Update()
{
    m_context->Update();
}

void UI::Render()
{
    m_renderInterfaceGl3->SetViewport(m_width, m_height);
    m_renderInterfaceGl3->BeginFrame();

    m_context->Render();

    m_renderInterfaceGl3->EndFrame();
}

} // namespace Gravitaris
