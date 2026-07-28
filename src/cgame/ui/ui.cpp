#include <algorithm>
#include <cmath>
#include <cstdio>

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include <gravitaris/ui/ui.hpp>

#include "detail/system-interface.hpp"
#include "detail/file-interface.hpp"
#include "detail/render-interface-gl3.hpp"

namespace Gravitaris {

static constexpr float HULL_WARN_FRACTION = 0.5f;
static constexpr float HULL_CRITICAL_FRACTION = 0.25f;

static std::optional<TeamId> TeamIdFromOption(const Rml::String& value);

static void Assign(Rml::Element* element, std::string& cached, std::optional<float> value,
                   const char* format);

namespace {

// Routes one element's event to a std::function, so the handlers can live as
// lambdas next to the code they belong to instead of one class each.
class FunctionListener : public Rml::EventListener {
public:
    explicit FunctionListener(std::function<void(Rml::Event&)> handler)
            : m_handler(std::move(handler))
    {}

    void ProcessEvent(Rml::Event& event) override { m_handler(event); }

private:
    std::function<void(Rml::Event&)> m_handler;
};

} // namespace

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

    // Both of these have to be in place before the documents below load: RmlUi
    // lays a document out as it is loaded, so a context still holding the
    // placeholder size or ratio produces a layout that visibly jumps on the
    // first frame that corrects it. The caller sets them before calling Init().
    m_context = Rml::CreateContext("default", Rml::Vector2i(m_width, m_height));
    m_context->SetDensityIndependentPixelRatio(m_dpRatio);

    // Chakra Petch carries the chamfered-vector look and is the theme's base
    // face; Share Tech Mono is for readouts only, where proportional digits
    // would shift the layout as the values change. Lato stays loaded as the
    // fallback for anything neither covers.
    Rml::LoadFontFace("ui/ChakraPetch-Regular.ttf");
    Rml::LoadFontFace("ui/ChakraPetch-Bold.ttf");
    Rml::LoadFontFace("ui/ShareTechMono-Regular.ttf");
    Rml::LoadFontFace("ui/LatoLatin-Regular.ttf");
    Rml::LoadFontFace("ui/LatoLatin-Bold.ttf");
    Rml::LoadFontFace("ui/LatoLatin-BoldItalic.ttf");
    Rml::LoadFontFace("ui/LatoLatin-Italic.ttf");

    // HUD first so interactive windows loaded after it stack on top of it.
    if (Rml::ElementDocument* hud = m_context->LoadDocument("ui/hud.rml")) {
        hud->Show();
        m_hudStatus = hud->GetElementById("status_readout");
        m_hudPing = hud->GetElementById("ping_readout");
        m_sidebar = hud->GetElementById("sidebar");
        m_healthFill = hud->GetElementById("health_fill");
        m_healthValue = hud->GetElementById("health_value");
        m_missileTicks = hud->GetElementById("missile_ticks");
        m_missileValue = hud->GetElementById("missile_value");
        m_speedReadout = hud->GetElementById("speed_readout");
        m_headingReadout = hud->GetElementById("heading_readout");
        m_gwellReadout = hud->GetElementById("gwell_readout");
        m_minimap = hud->GetElementById("minimap");

        if (m_minimap) {
            // mousedown, not click: the camera should move on press rather
            // than on release. "drag" then keeps it following the cursor for
            // as long as the button is held (see the panel's `drag` property).
            Listen(*m_minimap, "mousedown", [this](Rml::Event& event) { HandleMinimapPoint(event); });
            Listen(*m_minimap, "drag", [this](Rml::Event& event) { HandleMinimapPoint(event); });
        }
        m_recenterButton = hud->GetElementById("recenter");
        if (m_recenterButton) {
            Listen(*m_recenterButton, "click", [this](Rml::Event&) {
                if (m_onRecenter) m_onRecenter();
            });
            SetRecenterVisible(false);
        }
    }

    m_document = m_context->LoadDocument("ui/main.rml");
    if (m_document) {
        // The window template's title bar carries placeholder text; the
        // document's own <title> is the real one.
        if (Rml::Element* title = m_document->GetElementById("title")) {
            title->SetInnerRML(m_document->GetTitle());
        }

        if (Rml::Element* teamSelect = m_document->GetElementById("team_select")) {
            Listen(*teamSelect, "change", [this](Rml::Event& event) {
                const Rml::String value = event.GetParameter<Rml::String>("value", "");
                if (const std::optional<TeamId> team = TeamIdFromOption(value)) m_introTeam = *team;
            });
        }

        if (Rml::Element* button = m_document->GetElementById("dismiss_intro")) {
            Listen(*button, "click", [this](Rml::Event&) {
                m_document->Hide();
                if (m_onIntroConfirm) m_onIntroConfirm(m_introTeam);
            });
        }

        m_document->Show();
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
    if (ratio == m_dpRatio) return;

    m_dpRatio = ratio;
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

void UI::SetHudStatus(const std::string& build, const std::string& ping)
{
    if (m_hudStatus && build != m_hudStatusText) {
        m_hudStatusText = build;
        m_hudStatus->SetInnerRML(build);
    }
    if (m_hudPing && ping != m_hudPingText) {
        m_hudPingText = ping;
        m_hudPing->SetInnerRML(ping);
    }
}

void UI::SetHullFraction(float fraction)
{
    if (!m_healthFill || !m_healthValue) return;

    // Quantised to whole percent: that's the resolution both the bar width
    // and the label actually show, so anything finer only costs reflows.
    const float quantised = fraction < 0.f ? -1.f : std::round(std::clamp(fraction, 0.f, 1.f) * 100.f) / 100.f;
    if (quantised == m_hullFraction) return;

    m_hullFraction = quantised;

    if (quantised < 0.f) {
        m_healthFill->SetProperty("width", "0%");
        m_healthValue->SetInnerRML("--");
        return;
    }

    const int percent = static_cast<int>(std::lround(quantised * 100.f));
    m_healthFill->SetProperty("width", std::to_string(percent) + "%");
    m_healthValue->SetInnerRML(std::to_string(percent) + "%");

    m_healthFill->SetClass("warn", quantised <= HULL_WARN_FRACTION && quantised > HULL_CRITICAL_FRACTION);
    m_healthFill->SetClass("critical", quantised <= HULL_CRITICAL_FRACTION);
}

void UI::SetMissileAmmo(int ammo, int capacity)
{
    if (!m_missileTicks || !m_missileValue) return;
    if (ammo == m_missileAmmo) return;

    m_missileAmmo = ammo;

    if (ammo < 0) {
        m_missileValue->SetInnerRML("--");
        m_missileTicks->SetInnerRML("");
        return;
    }

    m_missileValue->SetInnerRML(std::to_string(ammo));

    // Rebuilt rather than toggling classes on persistent ticks: the row only
    // changes when a missile is fired or a rack is collected, and the count
    // gate above means that's the only time this runs at all.
    std::string ticks;
    for (int i = 0; i < capacity; ++i) {
        ticks += i < ammo ? "<span class=\"missile_tick\"></span>"
                          : "<span class=\"missile_tick empty\"></span>";
    }
    m_missileTicks->SetInnerRML(ticks);
}

void UI::SetHudTelemetry(std::optional<float> speed, std::optional<float> heading,
                         std::optional<float> gravityAccel)
{
    // Zero-padded so a turning ship doesn't shuffle the neighbouring cells
    // sideways as it crosses 100 and 10. Wrapped after rounding, not before:
    // 359.7 has to come out as 000 rather than a fourth digit.
    if (heading) heading = std::fmod(std::round(*heading), 360.f);
    Assign(m_headingReadout, m_headingText, heading, "%03.0f");
    Assign(m_speedReadout, m_speedText, speed, "%.0f");
    Assign(m_gwellReadout, m_gwellText, gravityAccel, "%.1f");
}

int UI::GetSidebarWidthPx() const
{
    return m_sidebar ? static_cast<int>(m_sidebar->GetOffsetWidth()) : 0;
}

void UI::SetIntroConfirmCallback(std::function<void(TeamId)> callback)
{
    m_onIntroConfirm = std::move(callback);
}

void UI::SetMinimapClickCallback(std::function<void(float, float)> callback)
{
    m_onMinimapClick = std::move(callback);
}

void UI::SetRecenterCallback(std::function<void()> callback)
{
    m_onRecenter = std::move(callback);
}

void UI::SetRecenterVisible(bool visible)
{
    if (!m_recenterButton || visible == m_recenterVisible) return;

    m_recenterVisible = visible;
    m_recenterButton->SetProperty("display", visible ? "block" : "none");
}

void UI::Listen(Rml::Element& element, const char* event, std::function<void(Rml::Event&)> handler)
{
    m_listeners.push_back(std::make_unique<FunctionListener>(std::move(handler)));
    element.AddEventListener(event, m_listeners.back().get());
}

// Maps the intro dropdown's option values. Offering another side is a new
// <option> in main.rml plus a line here -- and a world that actually has a
// starting complex for it (see Game::BuildWorld).
static std::optional<TeamId> TeamIdFromOption(const Rml::String& value)
{
    if (value == "blue") return TeamId::Blue;
    if (value == "red") return TeamId::Red;
    return std::nullopt;
}

// Writes a formatted number into a readout, or "--" for an empty one, skipping
// the SetInnerRML reflow when the text hasn't moved.
static void Assign(Rml::Element* element, std::string& cached, std::optional<float> value,
                   const char* format)
{
    if (!element) return;

    std::string text = "--";
    if (value) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), format, *value);
        text = buffer;
    }

    if (text == cached) return;
    cached = std::move(text);
    element->SetInnerRML(cached);
}

void UI::HandleMinimapPoint(Rml::Event& event)
{
    if (!m_onMinimapClick || !m_minimap) return;

    const Rml::Vector2f origin = m_minimap->GetAbsoluteOffset(Rml::BoxArea::Content);
    const float width = m_minimap->GetClientWidth();
    const float height = m_minimap->GetClientHeight();
    if (width <= 0.f || height <= 0.f) return;

    // Context-space click -> -1..1 across the panel, +Y up (the panel draws
    // the world's +Y upward, screen Y grows downward).
    const float nx = 2.f * (event.GetParameter("mouse_x", 0.f) - origin.x) / width - 1.f;
    const float ny = 1.f - 2.f * (event.GetParameter("mouse_y", 0.f) - origin.y) / height;

    m_onMinimapClick(std::clamp(nx, -1.f, 1.f), std::clamp(ny, -1.f, 1.f));
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

    RefreshActiveDocument();
}

void UI::RefreshActiveDocument()
{
    // The focused element is usually a control inside the document rather than
    // the document itself, so ask it which document it belongs to.
    Rml::Element* focused = m_context->GetFocusElement();
    Rml::ElementDocument* active = focused ? focused->GetOwnerDocument() : nullptr;

    if (active == m_activeDocument) return;
    m_activeDocument = active;

    for (int i = 0; i < m_context->GetNumDocuments(); ++i) {
        Rml::ElementDocument* document = m_context->GetDocument(i);
        document->SetClass("active", document == active);
    }
}

void UI::Render()
{
    m_renderInterfaceGl3->SetViewport(m_width, m_height);
    m_renderInterfaceGl3->BeginFrame();

    m_context->Render();

    m_renderInterfaceGl3->EndFrame();
}

} // namespace Gravitaris
