#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Debugger.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/ui/ui.hpp>

#include "detail/system-interface.hpp"
#include "detail/file-interface.hpp"
#include "detail/render-interface-gl3.hpp"

namespace Gravitaris {

static constexpr float HULL_WARN_FRACTION = 0.5f;
static constexpr float HULL_CRITICAL_FRACTION = 0.25f;

// Distinct shades a plating block is drawn in. Coarse on purpose: this is the
// change gate for rebuilding the row, and a plate regenerating continuously
// would otherwise rewrite it every frame for steps nobody can see.
static constexpr float SHIELD_SHADES = 12.f;

// Width of the divider between plating blocks, as a share of the whole track.
static constexpr float SEGMENT_GAP_PERCENT = 2.f;

// Shade a fully spent plate keeps, so it stays a visible dark slot rather than
// disappearing into the track -- the gap is the reading.
static constexpr float SEGMENT_FLOOR = 0.18f;

static Rml::Input::KeyIdentifier RmlKey(UiKey key);
static int RmlModifiers(int modifiers);

static std::optional<TeamId> TeamIdFromOption(const Rml::String& value);
static const char* TeamOptionValue(TeamId team);
static const char* TeamLabel(TeamId team);

static void Assign(Rml::Element* element, std::string& cached, std::optional<float> value,
                   const char* format);

// Markup-safe rendering of text somebody typed: SetInnerRML parses what it is
// given, so an unescaped '<' from another player is markup running inside the
// HUD.
static std::string EscapeRml(const std::string& text);

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
    Rml::LoadFontFace("ui/fonts/ChakraPetch-Regular.ttf");
    Rml::LoadFontFace("ui/fonts/ChakraPetch-Bold.ttf");
    Rml::LoadFontFace("ui/fonts/ShareTechMono-Regular.ttf");
    Rml::LoadFontFace("ui/fonts/LatoLatin-Regular.ttf");
    Rml::LoadFontFace("ui/fonts/LatoLatin-Bold.ttf");
    Rml::LoadFontFace("ui/fonts/LatoLatin-BoldItalic.ttf");
    Rml::LoadFontFace("ui/fonts/LatoLatin-Italic.ttf");

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
        m_cannonTicks = hud->GetElementById("cannon_ticks");
        m_cannonValue = hud->GetElementById("cannon_value");
        m_cannonMode = hud->GetElementById("cannon_mode");
        m_speedReadout = hud->GetElementById("speed_readout");
        m_gwellReadout = hud->GetElementById("gwell_readout");
        m_shieldFill = hud->GetElementById("shield_fill");
        m_shieldValue = hud->GetElementById("shield_value");
        m_shieldSegments = hud->GetElementById("shield_segments");
        m_chat = hud->GetElementById("chat");
        m_chatLog = hud->GetElementById("chat_log");
        m_chatInput = hud->GetElementById("chat_input");

        // ElementHandle drags by writing top/left and resizes by writing
        // width/height, so the window's bottom-left anchor has to go before
        // either lands -- otherwise a definite top and bottom stretch the box
        // between them, and a taller box grows upward out of the corner being
        // dragged. Pinning top to where it already is keeps it still meanwhile.
        for (const char* id : {"chat_grip", "chat_size"}) {
            Rml::Element* grip = hud->GetElementById(id);
            if (!grip) continue;
            Listen(*grip, "dragstart", [this](Rml::Event&) {
                if (!m_chat) return;
                const int top = static_cast<int>(std::lround(m_chat->GetOffsetTop()));
                m_chat->SetProperty("top", std::to_string(top) + "px");
                m_chat->SetProperty("bottom", "auto");
            });
        }
        m_boostFill = hud->GetElementById("boost_fill");
        m_boostValue = hud->GetElementById("boost_value");
        m_researchFill = hud->GetElementById("research_fill");
        m_researchValue = hud->GetElementById("research_value");
        m_hudTechValue = hud->GetElementById("tech_value");
        m_hudSuppliesValue = hud->GetElementById("supplies_value");
        if (Rml::Element* button = hud->GetElementById("open_tech_tree")) {
            Listen(*button, "click", [this](Rml::Event&) { SetTechTreeVisible(!IsTechTreeVisible()); });
        }
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

    // Loaded before main.rml so the intro dialog stacks over it, and hidden
    // until the player asks for it -- the tree is a window they open, not a
    // panel that appears at them.
    m_techTree = m_context->LoadDocument("ui/tech-tree.rml");
    if (m_techTree) {
        if (Rml::Element* title = m_techTree->GetElementById("title")) {
            title->SetInnerRML(m_techTree->GetTitle());
        }
        m_techGrid = m_techTree->GetElementById("tech_branches");
        m_shipViewPanel = m_techTree->GetElementById("ship_view_panel");
        m_shipSlotLayer = m_techTree->GetElementById("ship_slots");
        m_installedList = m_techTree->GetElementById("installed_list");
        m_availableList = m_techTree->GetElementById("available_list");
        m_installedCount = m_techTree->GetElementById("installed_count");
        m_availableCount = m_techTree->GetElementById("available_count");
        m_techFitted = m_techTree->GetElementById("tech_fitted");
        m_techValue = m_techTree->GetElementById("tech_value");
        m_supplyValue = m_techTree->GetElementById("supply_value");
        // Loud rather than silent: without the container the board simply
        // never draws, which looks like a game bug rather than a typo.
        if (!m_techGrid) LOG(error) << "ui: tech-tree.rml has no #tech_branches; the board cannot draw";
        m_techInfo = m_techTree->GetElementById("tech_info");
        m_techNoticeElement = m_techTree->GetElementById("tech_notice");
        m_stagedCost = m_techTree->GetElementById("tech_staged_cost");
        m_confirmButton = m_techTree->GetElementById("tech_confirm");
        if (m_confirmButton) {
            Listen(*m_confirmButton, "click", [this](Rml::Event&) { ConfirmStaged(); });
        }
        // Two ways out of the window, one path: whatever is staged is forgotten
        // rather than committed, which is what closing a plan means.
        for (const char* id : {"tech_close", "tech_titlebar_close"}) {
            if (Rml::Element* close = m_techTree->GetElementById(id)) {
                Listen(*close, "click", [this](Rml::Event&) {
                    ResetStaged();
                    SetTechTreeVisible(false);
                });
            }
        }
        m_resupplyButton = m_techTree->GetElementById("tech_resupply");
        if (m_resupplyButton) {
            Listen(*m_resupplyButton, "click", [this](Rml::Event&) { RequestResupply(); });
        }
        m_resetButton = m_techTree->GetElementById("tech_reset");
        if (m_resetButton) {
            Listen(*m_resetButton, "click", [this](Rml::Event&) { ResetStaged(); });
        }

        if (Rml::Element* tab = m_techTree->GetElementById("tab_ship")) {
            Listen(*tab, "click", [this](Rml::Event&) { SetTechTab(0); });
        }
        if (Rml::Element* tab = m_techTree->GetElementById("tab_permanent")) {
            Listen(*tab, "click", [this](Rml::Event&) { SetTechTab(1); });
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

        m_seedRow = m_document->GetElementById("seed_row");
        m_seedInput = m_document->GetElementById("seed_input");
        m_nameInput = m_document->GetElementById("name_input");

        if (Rml::Element* button = m_document->GetElementById("apply_seed")) {
            Listen(*button, "click", [this](Rml::Event&) {
                if (!m_onSeedApply || !m_seedInput) return;
                // strtoul rather than stoul: a field the player is free to
                // type anything into shouldn't throw, and a partial parse of
                // a fat-fingered seed is as good an arbitrary number as any.
                const Rml::String text = m_seedInput->GetAttribute<Rml::String>("value", "");
                m_onSeedApply(static_cast<std::uint32_t>(std::strtoul(text.c_str(), nullptr, 10)));
            });
        }

        if (Rml::Element* button = m_document->GetElementById("randomize_seed")) {
            Listen(*button, "click", [this](Rml::Event&) {
                if (m_onSeedRandomize) m_onSeedRandomize();
            });
        }

        if (Rml::Element* button = m_document->GetElementById("dismiss_intro")) {
            Listen(*button, "click", [this](Rml::Event&) {
                m_document->Hide();
                const Rml::String name =
                        m_nameInput ? m_nameInput->GetAttribute<Rml::String>("value", "") : "";
                if (m_onIntroConfirm) m_onIntroConfirm(m_introTeam, name);
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
    m_mouseX = x;
    m_mouseY = y;
    return !m_context->ProcessMouseMove(x, y, 0);
}

bool UI::ProcessMouseButton(int rmlButtonIndex, bool down)
{
    if (!m_context) return false;
    return down ? !m_context->ProcessMouseButtonDown(rmlButtonIndex, 0)
                : !m_context->ProcessMouseButtonUp(rmlButtonIndex, 0);
}

bool UI::ProcessMouseWheel(float delta)
{
    if (!m_context) return false;
    return !m_context->ProcessMouseWheel(delta, 0);
}

bool UI::ProcessKeyDown(UiKey key, int modifiers)
{
    if (!m_context) return false;

    const Rml::Input::KeyIdentifier identifier = RmlKey(key);
    if (identifier == Rml::Input::KI_UNKNOWN) return false;

    return !m_context->ProcessKeyDown(identifier, RmlModifiers(modifiers));
}

bool UI::ProcessTextInput(const std::string& text)
{
    if (!m_context) return false;
    return !m_context->ProcessTextInput(Rml::String(text));
}

bool UI::IsKeyboardCaptured() const
{
    if (!m_context) return false;

    const Rml::Element* focused = m_context->GetFocusElement();
    if (!focused) return false;

    const Rml::String& tag = focused->GetTagName();
    return tag == "input" || tag == "textarea" || tag == "select";
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
    // Capacity is part of the gate, not just the count: pulling the locker that
    // widened the rack leaves the same rounds aboard in fewer tubes, and the row
    // is one tick per tube.
    if (ammo == m_missileAmmo && capacity == m_missileCapacity) return;

    m_missileAmmo = ammo;
    m_missileCapacity = capacity;

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

// Rounds one tick on the cannon row stands for. A magazine runs to three
// figures, so a tick per round would be a hundred of them across a sidebar
// that fits a dozen.
static constexpr int CANNON_ROUNDS_PER_TICK = 10;

void UI::SetCannonAmmo(int ammo, int capacity, PrimaryMode mode)
{
    if (!m_cannonTicks || !m_cannonValue) return;

    if (mode != m_cannonPrimary && m_cannonMode) {
        m_cannonPrimary = mode;
        // Named on the row itself: which primary is up is the pilot's own
        // choice, and a HUD that made them infer it from which bar last moved
        // would be asking them to check in a fight.
        const char* label = mode == PrimaryMode::Cannon ? "ARMED"
                          : mode == PrimaryMode::Guns   ? "GUNS" : "BOTH";
        m_cannonMode->SetInnerRML(ammo < 0 ? "" : label);
        m_cannonMode->SetClass("gun", mode == PrimaryMode::Guns);
        m_cannonMode->SetClass("both", mode == PrimaryMode::Both);
    }
    if (ammo == m_cannonAmmo && capacity == m_cannonCapacity) return;

    m_cannonAmmo = ammo;
    m_cannonCapacity = capacity;

    if (ammo < 0) {
        m_cannonValue->SetInnerRML("--");
        m_cannonTicks->SetInnerRML("");
        if (m_cannonMode) m_cannonMode->SetInnerRML("");
        return;
    }

    m_cannonValue->SetInnerRML(std::to_string(ammo));

    // Ticks round up, so the last few rounds still light one: a bar that
    // emptied while the cannon could still fire would be a lie at exactly the
    // moment it matters.
    const int total = (capacity + CANNON_ROUNDS_PER_TICK - 1) / CANNON_ROUNDS_PER_TICK;
    const int lit = (ammo + CANNON_ROUNDS_PER_TICK - 1) / CANNON_ROUNDS_PER_TICK;

    std::string ticks;
    for (int i = 0; i < total; ++i) {
        ticks += i < lit ? "<span class=\"cannon_tick\"></span>"
                         : "<span class=\"cannon_tick empty\"></span>";
    }
    m_cannonTicks->SetInnerRML(ticks);
}

void UI::SetHudTelemetry(std::optional<float> speed, std::optional<float> gravityAccel)
{
    Assign(m_speedReadout, m_speedText, speed, "%.0f");
    Assign(m_gwellReadout, m_gwellText, gravityAccel, "%.1f");
}

void UI::SetShieldFraction(float fraction, const std::string& styleClass)
{
    if (!m_shieldFill || !m_shieldValue) return;

    // Same whole-percent quantisation the hull bar uses, for the same reason.
    const float quantised =
            fraction < 0.f ? -1.f : std::round(std::clamp(fraction, 0.f, 1.f) * 100.f) / 100.f;
    if (quantised == m_shieldFraction && styleClass == m_shieldStyle) return;

    if (!m_shieldStyle.empty() && m_shieldStyle != styleClass) {
        m_shieldFill->SetClass(m_shieldStyle, false);
    }
    m_shieldFraction = quantised;
    m_shieldStyle = styleClass;
    if (!styleClass.empty()) m_shieldFill->SetClass(styleClass, true);

    if (quantised < 0.f) {
        m_shieldFill->SetProperty("width", "0%");
        m_shieldValue->SetInnerRML("--");
        return;
    }

    const int percent = static_cast<int>(std::lround(quantised * 100.f));
    m_shieldFill->SetProperty("width", std::to_string(percent) + "%");
    m_shieldValue->SetInnerRML(std::to_string(percent) + "%");
}

void UI::SetShieldSegments(const std::vector<float>& charges)
{
    if (!m_shieldSegments || !m_shieldFill) return;

    // Shade steps, not raw floats: a plate regenerating continuously would
    // otherwise rewrite the markup every frame for changes nobody can see.
    std::vector<int> steps;
    steps.reserve(charges.size());
    for (const float charge : charges) {
        steps.push_back(static_cast<int>(std::lround(std::clamp(charge, 0.f, 1.f) * SHIELD_SHADES)));
    }
    if (steps == m_shownSegments) return;
    m_shownSegments = steps;

    // A bubble (or any hull with no plates) falls back to the continuous bar.
    // display, not visibility: both live in the same track, and a hidden-but-
    // laid-out fill still takes its own 14dp of a 14dp content box, which
    // pushes the blocks out through the track's bottom border.
    m_shieldSegments->SetClass("shown", !steps.empty());
    m_shieldFill->SetProperty("display", steps.empty() ? "block" : "none");
    if (steps.empty()) {
        m_shieldSegments->SetInnerRML("");
        return;
    }

    // Percentages throughout rather than dp gaps, so the blocks divide the
    // track exactly however many plates the hull carries.
    const auto count = static_cast<float>(steps.size());
    const float gap = steps.size() > 1 ? SEGMENT_GAP_PERCENT : 0.f;
    const float width = (100.f - gap * (count - 1.f)) / count;

    std::string markup;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        // A spent plate stays visible as a dark slot -- the gap in
        // [xxxx---xxxxxx] is the point, and an absent block would just make
        // the bar shorter.
        const float charge = static_cast<float>(steps[i]) / SHIELD_SHADES;
        const int level = static_cast<int>(std::lround((SEGMENT_FLOOR + (1.f - SEGMENT_FLOOR) * charge) * 255.f));

        char block[256];
        std::snprintf(block, sizeof(block),
                      "<span style=\"width: %.4f%%; margin-left: %.4f%%; "
                      "background-color: rgba(%d, %d, %d, 255);\"></span>",
                      width, i == 0 ? 0.f : gap,
                      level, static_cast<int>(std::lround(level * 0.80f)),
                      static_cast<int>(std::lround(level * 0.40f)));
        markup += block;
    }
    m_shieldSegments->SetInnerRML(markup);
}

void UI::SetChatLog(const std::vector<ChatLineView>& lines)
{
    if (!m_chatLog) return;
    if (lines == m_shownChat) return;

    m_shownChat = lines;

    // Somebody scrolled up is reading history, and yanking them back to the
    // bottom on the next line would make the scrollback useless. Anyone
    // already at the end keeps following. The slack absorbs the fractional
    // remainder a partly-visible line leaves.
    static constexpr float AT_END_SLACK = 2.f;
    m_chatScrollToEnd = m_chatLog->GetScrollTop() + m_chatLog->GetClientHeight()
                        >= m_chatLog->GetScrollHeight() - AT_END_SLACK;

    std::string markup;
    for (const ChatLineView& line : lines) {
        markup += "<div class=\"chat_line\"><span class=\"chat_sender\" style=\"color: "
                  + line.senderColor + ";\">" + EscapeRml(line.sender) + "</span> ";
        for (const ChatSpan& span : line.body) {
            if (span.color.empty()) markup += EscapeRml(span.text);
            else markup += "<span style=\"color: " + span.color + ";\">" + EscapeRml(span.text) + "</span>";
        }
        markup += "</div>";
    }
    m_chatLog->SetInnerRML(markup);
}

void UI::SetChatInput(bool active, const std::string& text)
{
    if (!m_chatInput) return;
    if (active == m_chatInputActive && text == m_shownChatInput) return;

    m_chatInputActive = active;
    m_shownChatInput = text;

    m_chatInput->SetClass("active", active);
    m_chatInput->SetInnerRML(active ? "say: " + EscapeRml(text) + "_" : "");
}

void UI::SetBoostReadout(float fraction, bool cooling)
{
    if (!m_boostFill || !m_boostValue) return;

    const float quantised =
            fraction < 0.f ? -1.f : std::round(std::clamp(fraction, 0.f, 1.f) * 100.f) / 100.f;
    if (quantised == m_boostFraction && cooling == m_boostCooling) return;

    m_boostFraction = quantised;
    m_boostCooling = cooling;
    m_boostFill->SetClass("cooling", cooling);

    if (quantised < 0.f) {
        m_boostFill->SetProperty("width", "0%");
        m_boostValue->SetInnerRML("--");
        return;
    }

    const int percent = static_cast<int>(std::lround(quantised * 100.f));
    m_boostFill->SetProperty("width", std::to_string(percent) + "%");
    // Three states off two inputs: the injectors are cooling (how far along),
    // a burn is running (nothing useful to count down to), or it is there to
    // be spent.
    if (cooling) m_boostValue->SetInnerRML(std::to_string(percent) + "%");
    else m_boostValue->SetInnerRML(quantised < 1.f ? "BURN" : "READY");
}

void UI::SetResearchReadout(float fraction, const std::string& text)
{
    if (!m_researchFill || !m_researchValue) return;

    const float quantised =
            fraction < 0.f ? -1.f : std::round(std::clamp(fraction, 0.f, 1.f) * 100.f) / 100.f;
    if (quantised == m_researchFraction && text == m_researchText) return;

    m_researchFraction = quantised;
    m_researchText = text;

    if (quantised < 0.f) {
        m_researchFill->SetProperty("width", "0%");
        m_researchValue->SetInnerRML("--");
        return;
    }

    const int percent = static_cast<int>(std::lround(quantised * 100.f));
    m_researchFill->SetProperty("width", std::to_string(percent) + "%");
    m_researchValue->SetInnerRML(text);
}

static const char* RankNumeral(int rank);

// Pip colours are their own vocabulary, not the footer's: a pip says whether
// this rank is owned, buyable, out of pocket or not researched, in one glance
// and no words.
static const char* PipClass(TechRankState state)
{
    switch (state) {
    case TechRankState::Held:         return "fitted";
    case TechRankState::Available:    return "buy";
    case TechRankState::Unaffordable:
    case TechRankState::NeedsLanding: return "poor";
    default:                          return "";
    }
}

// Roman on the pips and in the tooltip alike: a rank is a mark on a hull, not
// a quantity, and "II" reads as one where "2" reads as two of something.
static const char* RankNumeral(int rank)
{
    switch (rank) {
    case 1: return "I";
    case 2: return "II";
    case 3: return "III";
    case 4: return "IV";
    case 5: return "V";
    case 6: return "VI";
    case 7: return "VII";
    case 8: return "VIII";
    }
    return "";
}

static std::string Upper(std::string text)
{
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// Rounds left on an installed row, and nothing at all for a fitting that has no
// magazine to report. Empty is its own class rather than a colour picked here:
// a dry weapon is the one thing on this list worth reading across the room.
static std::string AmmoSpan(const TechNodeView& node)
{
    if (node.ammo < 0) return {};

    const std::string span = std::to_string(node.ammo) + " / " + std::to_string(node.ammoCapacity);
    return "<span class=\"entry_ammo" + std::string(node.ammo == 0 ? " dry" : "") + "\">" + span
           + "</span>";
}

// The three role branches, in board order. Names and classes only -- what goes
// in them is whatever the pool says belongs there (TechNodeView::branch).
static constexpr const char* BRANCH_NAMES[] = {"WEAPONS", "MOBILITY", "DEFENSE"};
static constexpr const char* BRANCH_CLASSES[] = {"weapons", "mobility", "defense"};
static constexpr int BRANCH_COUNT = 3;

static constexpr float TILE_SIZE = 54.f;

// Matches div#ship_view_frame's size in tech-tree.rml: slot positions arrive
// as a fraction of the drawing, and this is what turns them into offsets
// within it. A slot is a TILE_SIZE tile, the same one the branches use.
static constexpr float SHIP_VIEW_WIDTH = 640.f;
static constexpr float SHIP_VIEW_HEIGHT = 360.f;

// Clicking the same tile again asks for the rank above the one already
// staged, and falls off the top back to nothing staged. A pip still stages the
// rank it *is* -- that is what pips are for -- but a tile has no rank of its
// own to mean, so it means "one more".
void UI::CycleStagePick(std::uint32_t id, int tab)
{
    const TechNodeView* node = nullptr;
    for (const TechNodeView& candidate : m_shownNodes) {
        if (candidate.tab == tab && candidate.id == id) node = &candidate;
    }
    if (!node || node->ranks.empty()) return;

    const int staged = StagedRankFor(id, tab);
    const int from = staged > 0 ? staged : node->rank;

    for (int rank = from + 1; rank <= node->maxRank; ++rank) {
        if (rank > static_cast<int>(node->ranks.size())) break;
        if (Stageable(*node, rank, tab)) {
            SetStagedRank(id, tab, rank);
            return;
        }
    }
    SetStagedRank(id, tab, 0); // past the top: nothing staged
}

// Whether a plan may include this rank. The two trees answer differently, and
// the difference is the whole reason this isn't just a state check:
//
//   PERMANENT is a ladder. It only ever offers the rank one past what the
//   faction holds, so every rank above that reads Locked -- not because it is
//   out of reach, but because the rungs below it haven't been climbed yet. A
//   plan is allowed to stand on rungs it is itself planning to buy, so what
//   matters is that the *first* step is genuinely on offer.
//
//   SHIP sells a rank outright: III costs III's price whether or not the hull
//   ever carried I. So only a rank actually on offer can be staged, and
//   staging it is one purchase rather than a climb.
bool UI::Stageable(const TechNodeView& node, int rank, int tab) const
{
    const auto index = static_cast<std::size_t>(rank - 1);
    if (index >= node.ranks.size()) return false;

    if (tab != 1) return node.ranks[index].state == TechRankState::Available;

    const auto firstOffer = static_cast<std::size_t>(node.rank);
    if (firstOffer >= node.ranks.size()) return false; // the line is finished
    const TechRankState first = node.ranks[firstOffer].state;

    // Unaffordable is deliberately allowed through: the purse is checked
    // against the plan as a whole (see RefreshConfirmButton), not rank by rank.
    return first == TechRankState::Available || first == TechRankState::Unaffordable;
}

// Everything a staged pick shows in, repainted where it stands. No markup is
// written, so nothing is destroyed, nothing relayouts, and the element the
// click came from is still under the cursor when the next one arrives.
void UI::RefreshStagedVisuals()
{
    for (const TileRefs& refs : m_tileRefs) {
        const int staged = StagedRankFor(refs.id, m_techTab);

        if (refs.tile) refs.tile->SetClass("staged", staged > 0);
        if (refs.counter) {
            refs.counter->SetInnerRML(std::to_string(staged > 0 ? staged : refs.heldRank) + "/"
                                      + std::to_string(refs.maxRank));
        }
        // Both classes, not just the staged one: a plan may sit *below* the
        // rank on the hull, and a meter that only ever added lights would draw
        // a downgrade as the level it is leaving.
        for (std::size_t i = 0; i < refs.pips.size(); ++i) {
            const bool lit = static_cast<int>(i) < (staged > 0 ? staged : refs.heldRank);
            refs.pips[i]->SetClass("staged", staged > 0 && lit);
            refs.pips[i]->SetClass("fitted", staged == 0 && lit);
        }
    }

    for (std::size_t i = 0; i < m_slotElements.size() && i < m_shownSlots.size(); ++i) {
        const ShipSlotView& slot = m_shownSlots[i];
        const TechNodeView* fitted = FittedIn(slot);
        const TechNodeView* staged = StagedIn(slot);
        const bool stripping = PlanFor(slot).strip;

        Rml::Element* element = m_slotElements[i];
        element->SetClass("staged", staged != nullptr || stripping);
        element->SetClass("fitted", staged == nullptr && !stripping && fitted != nullptr);
        element->SetClass("selected", slot.name == m_selectedSlot);
        // Planned empty reads as empty: the frame says a plan stands against
        // it, and the code says what will be there when it lands, which is
        // nothing.
        element->SetClass("stripping", stripping);

        if (Rml::Element* glyph = element->GetChild(0)) {
            glyph->SetInnerRML(SlotGlyph(slot, stripping ? nullptr : staged ? staged : fitted));
        }
    }

    // What the hull carries, and whether it is about to stop carrying it. A
    // planned pull says so on the line itself: the ledger claiming a part is
    // fitted while the plan beside it is to take that part off is the panel
    // contradicting itself.
    for (const InstalledRowRef& row : m_installedRows) {
        const ShipSlotView* mount = SlotNamed(row.mount);
        const bool pulling = mount && PlanFor(*mount).strip;
        row.row->SetClass("pulling", pulling);
        row.note->SetInnerRML(pulling ? "PULLING" : row.fittedNote);
    }

    // A row is staged when it is the exact fitting planned for the selected
    // slot; CLEAR is staged when the plan is to empty it.
    const ShipSlotView* selected = SelectedSlot();
    const SlotPlan plan = selected ? PlanFor(*selected) : SlotPlan{};
    const std::vector<SlotOffer> offers = OfferedForSelection();
    for (std::size_t i = 0; i < m_availableRows.size() && i < offers.size(); ++i) {
        const SlotOffer& offer = offers[i];
        Rml::Element* row = m_availableRows[i];
        row->SetClass("staged", !plan.strip && plan.id == offer.node->id
                             && plan.rank == static_cast<int>(offer.rankIndex) + 1);

        // The one row that reads FITTED is the rank in the slot, so it is the
        // one a pull is about.
        if (RankInSelection(*offer.node, offer.rankIndex).state != TechRankState::Held) continue;
        row->SetClass("pulling", plan.strip);
        if (Rml::Element* note = row->GetChild(row->GetNumChildren() - 1)) {
            note->SetInnerRML(plan.strip ? "PULLING" : "FITTED");
        }
    }
    if (m_noneRow) m_noneRow->SetClass("staged", plan.strip);

    RefreshConfirmButton();
}

void UI::SetStagedRank(std::uint32_t id, int tab, int rank, SlotRef hole)
{
    // A hole holds one thing, so a pick for it replaces whatever was planned
    // there -- picking a cannon where a gun was staged is a swap, not a second
    // entry, and so is fitting something where a strip was planned. Everything
    // else is keyed by the slot it goes into and the tab: the same def id names
    // a node in each tree, and learning a rank is not the same plan as fitting
    // one. Only the ship tab has slots to share -- a faction learning the
    // bubble is not a faction declining the plating.
    for (std::size_t i = m_staged.size(); i > 0; --i) {
        const StagedPick& staged = m_staged[i - 1];
        const bool sameSlot = tab == 0 ? SharesSlot(staged.id, id) : staged.id == id;
        const bool same = hole.IsHole()
                                ? (staged.tab == tab && staged.hole == hole)
                                : (staged.tab == tab && sameSlot && !staged.hole.IsHole());
        if (!same) continue;

        // Clicking the same plan again takes it back off; anything else
        // replaces it.
        const bool repeat = staged.id == id && staged.rank == rank && !staged.strip;
        m_staged.erase(m_staged.begin() + static_cast<std::ptrdiff_t>(i - 1));
        if (repeat) rank = 0;
        break;
    }
    if (rank != 0) m_staged.push_back(StagedPick{id, tab, rank, hole});
    RefreshStagedVisuals();
}

void UI::StageStrip(std::uint32_t id, SlotRef hole)
{
    for (std::size_t i = m_staged.size(); i > 0; --i) {
        const StagedPick& staged = m_staged[i - 1];
        const bool same = hole.IsHole() ? (staged.tab == 0 && staged.hole == hole)
                                        : (staged.tab == 0 && SharesSlot(staged.id, id)
                                           && !staged.hole.IsHole());
        if (!same) continue;

        const bool repeat = staged.strip;
        m_staged.erase(m_staged.begin() + static_cast<std::ptrdiff_t>(i - 1));
        if (repeat) { // clicking CLEAR again puts the plan back to leaving it alone
            RefreshStagedVisuals();
            return;
        }
        break;
    }
    m_staged.push_back(StagedPick{id, 0, 0, hole, /*strip=*/true});
    RefreshStagedVisuals();
}

// The one plan standing against a slot. A slot on a hull hole is looked up by
// that hole; one that sits on no hole is looked up by what it accepts, since
// a shield or an overburn belongs to exactly one slot on the drawing and its
// pick carries no hole to key by.
UI::SlotPlan UI::PlanFor(const ShipSlotView& slot) const
{
    const SlotRef hole = RefOf(slot);
    for (const StagedPick& staged : m_staged) {
        if (staged.tab != 0) continue;

        if (hole.IsHole()) {
            if (staged.hole == hole) return SlotPlan{staged.id, staged.rank, staged.strip};
            continue;
        }
        if (staged.hole.IsHole()) continue;
        if (SlotTakes(slot, staged.id)) return SlotPlan{staged.id, staged.rank, staged.strip};
    }
    return SlotPlan{};
}

// Whether this slot accepts that node at all, by the category names the
// drawing and the pool both spell.
bool UI::SlotTakes(const ShipSlotView& slot, std::uint32_t nodeId) const
{
    for (const TechNodeView& node : m_shownNodes) {
        if (node.tab != 0 || node.id != nodeId) continue;
        for (const std::string& category : slot.categories) {
            for (const std::string& fits : node.slots) {
                if (fits == category) return true;
            }
        }
    }
    return false;
}

// Whether two ship-tree picks would land in the same place on the hull. Both
// ammo lockers name the `ammo` slot, as both shield emitters name theirs, and a
// slot that carries one part cannot stand two plans -- keying that by node id
// alone left both planned at once, with the older of them showing.
bool UI::SharesSlot(std::uint32_t a, std::uint32_t b) const
{
    if (a == b) return true;

    const TechNodeView* first = nullptr;
    const TechNodeView* second = nullptr;
    for (const TechNodeView& node : m_shownNodes) {
        if (node.tab != 0) continue;
        if (node.id == a) first = &node;
        if (node.id == b) second = &node;
    }
    if (!first || !second) return false;

    for (const std::string& category : first->slots) {
        for (const std::string& other : second->slots) {
            if (category == other) return true;
        }
    }
    return false;
}

void UI::StagePick(std::uint32_t id, int tab, int rank)
{
    // Clicking the staged rank again takes it back off the plan.
    SetStagedRank(id, tab, StagedRankFor(id, tab) == rank ? 0 : rank);
}

int UI::HeldRankOf(std::uint32_t id, int tab) const
{
    for (const TechNodeView& node : m_shownNodes) {
        if (node.id == id && node.tab == tab) return node.rank;
    }
    return 0;
}

int UI::StagedRankFor(std::uint32_t id, int tab) const
{
    for (const StagedPick& staged : m_staged) {
        if (staged.id == id && staged.tab == tab) return staged.rank;
    }
    return 0;
}

void UI::ConfirmStaged()
{
    if (m_staged.empty()) return;

    // Handed over in the order they were staged: a prerequisite picked before
    // the thing that needs it has to reach the sim in that order too. A
    // permanent rank is a rung on a ladder, so staging III there means buying
    // I, II and III -- ascending, since each is only on offer once the one
    // below it has landed. A ship rank is bought outright, so it is one pick.
    for (const StagedPick& staged : m_staged) {
        if (!m_onTechPick) break;
        if (staged.tab != 1) {
            m_onTechPick(staged.id, staged.tab, staged.rank, staged.hole.mount, staged.strip);
            continue;
        }
        for (int rank = HeldRankOf(staged.id, staged.tab) + 1; rank <= staged.rank; ++rank) {
            m_onTechPick(staged.id, staged.tab, rank, staged.hole.mount, /*strip=*/false);
        }
    }
    m_staged.clear();
    RefreshStagedVisuals();
}

void UI::ResetStaged()
{
    if (m_staged.empty()) return;
    m_staged.clear();
    RefreshStagedVisuals();
}

// Nothing staged and nothing to review: the click *is* the purchase. The server
// prices it again on arrival, so a button drawn from a stale offer buys nothing
// it shouldn't -- this check only keeps a pointless pick off the wire.
void UI::RequestResupply()
{
    if (!m_resupplyAvailable || m_resupplyCost <= 0 || !m_onResupply) return;
    m_onResupply();
}

void UI::RefreshResupplyButton()
{
    if (!m_resupplyButton) return;

    const bool live = m_resupplyAvailable && m_resupplyCost > 0;
    m_resupplyButton->SetClass("idle", !live);
    // The price is on the button rather than beside it: it is the whole of what
    // this one does, and it changes with every round fired.
    m_resupplyButton->SetInnerRML(m_resupplyCost > 0
                                          ? "RESUPPLY " + std::to_string(m_resupplyCost) + " SUP"
                                          : "FULL");
}

void UI::RefreshConfirmButton()
{
    if (m_confirmButton) m_confirmButton->SetClass("idle", m_staged.empty());
    if (m_resetButton) m_resetButton->SetClass("idle", m_staged.empty());

    if (!m_stagedCost) return;
    int supplies = 0;
    int tech = 0;
    int strips = 0;
    for (const StagedPick& staged : m_staged) {
        // Pulling a part costs nothing and buys nothing back, so it is counted
        // rather than priced -- the row still has to say the plan does
        // something, or CONFIRM would look inert.
        if (staged.strip) {
            ++strips;
            continue;
        }
        for (const TechNodeView& node : m_shownNodes) {
            // Both tabs are in this list and the same def id names a node in
            // each, so a pick that did not check the tab would be priced twice
            // -- once in supplies and once in Tech.
            if (node.id != staged.id || node.tab != staged.tab) continue;
            if (staged.tab == 0) {
                const auto rank = static_cast<std::size_t>(staged.rank - 1);
                if (rank < node.ranks.size()) supplies += node.ranks[rank].cost;
                continue;
            }
            // A permanent rank is a ladder, so a plan that stands on rungs it
            // is itself buying pays for every one of them.
            for (int rank = node.rank + 1; rank <= staged.rank; ++rank) {
                const auto index = static_cast<std::size_t>(rank - 1);
                if (index < node.ranks.size()) tech += node.ranks[index].cost;
            }
        }
    }

    std::string cost;
    if (supplies > 0) cost = std::to_string(supplies) + " SUP";
    if (tech > 0) cost += (cost.empty() ? "" : " + ") + std::to_string(tech) + " TECH";
    if (strips > 0) cost += (cost.empty() ? "" : " + ") + std::to_string(strips) + " PULLED";
    m_stagedCost->SetInnerRML(cost);

    // The purse against the plan as a whole, which is the check a rank-by-rank
    // one cannot make: each pick is affordable on its own and the pool is spent
    // down as they land, so the last of them is what a plan overruns on. Said
    // rather than enforced -- the port serves what it can and refuses the rest,
    // and CONFIRM staying live is what lets a pilot take that deal knowingly.
    m_stagedCost->SetClass("short", supplies > m_shownSupplies || tech > m_shownTech);
}

void UI::ClearTechInfo()
{
    if (!m_techInfo) return;
    m_techInfo->SetClass("idle", true);
    m_techInfo->SetInnerRML("<div class=\"info_text\"><span class=\"info_title\">SELECT A SYSTEM"
                            "</span><span class=\"info_body\">Hover a mount or a system for "
                            "details.</span></div><span class=\"info_cost\"></span>");
}

const TechNodeView* UI::FittedIn(const ShipSlotView& slot) const
{
    // A slot on a hull hole reports what is in *that hole*, as the loadout
    // placed it: two holes can carry different lines, so this cannot be
    // inferred from what the hull owns. An empty hole holds nothing, which is
    // not the same question as what the ship carries elsewhere.
    if (RefOf(slot).IsHole()) {
        if (slot.fittedId == 0) return nullptr;
        for (const TechNodeView& node : m_shownNodes) {
            if (node.tab == 0 && node.id == slot.fittedId) return &node;
        }
        return nullptr;
    }

    // Everything else is the hull's rather than a hole's -- a shield, the
    // overburn -- and each of those categories belongs to exactly one slot.
    for (const TechNodeView& node : m_shownNodes) {
        if (node.tab != 0 || node.rank <= 0) continue;
        if (SlotTakes(slot, node.id)) return &node;
    }
    return nullptr;
}

const TechNodeView* UI::StagedIn(const ShipSlotView& slot) const
{
    // A strip is a plan for the slot, but it is not something going *into* it.
    const SlotPlan plan = PlanFor(slot);
    if (plan.id == 0 || plan.strip) return nullptr;

    for (const TechNodeView& node : m_shownNodes) {
        if (node.tab == 0 && node.id == plan.id) return &node;
    }
    return nullptr;
}

bool UI::NodeIsMounted(const TechNodeView& node) const
{
    for (const ShipSlotView& slot : m_shownSlots) {
        if (RefOf(slot).IsHole() && SlotTakes(slot, node.id)) return true;
    }
    return false;
}

void UI::SetShipSlots(const std::vector<ShipSlotView>& slots)
{
    if (slots == m_shownSlots) return;
    m_shownSlots = slots;
    RebuildShipSlots();
}

// What a slot draws: the code of whatever is in it, or -- when it is empty --
// what it would take. A slot that accepts only one thing says so; one that
// takes either line has no single code to show, and saying "GUN" on an empty
// mount would read exactly like a gun fitted in it.
std::string UI::SlotGlyph(const ShipSlotView& slot, const TechNodeView* shown)
{
    if (shown) return Upper(shown->icon);
    if (slot.categories.size() != 1) return "---";
    return Upper(slot.categories.front().substr(0, 3));
}

void UI::RebuildShipSlots()
{
    if (!m_shipSlotLayer) return;

    std::string rml;
    for (const ShipSlotView& slot : m_shownSlots) {
        const int left = static_cast<int>(std::lround(slot.x * SHIP_VIEW_WIDTH - TILE_SIZE * 0.5f));
        const int top = static_cast<int>(std::lround(slot.y * SHIP_VIEW_HEIGHT - TILE_SIZE * 0.5f));

        const TechNodeView* fitted = FittedIn(slot);
        const TechNodeView* staged = StagedIn(slot);
        const std::string glyph = SlotGlyph(slot, staged ? staged : fitted);

        std::string slotClass = "slot";
        if (staged) slotClass += " staged";
        else if (fitted) slotClass += " fitted";
        if (slot.name == m_selectedSlot) slotClass += " selected";

        rml += "<div class=\"" + slotClass
               + "\" style=\"left: " + std::to_string(left) + "dp; top: "
               + std::to_string(top) + "dp;\">";
        rml += "<div class=\"glyph\">" + glyph + "</div>";
        // Last, so the glyph stays child 0 for RefreshStagedVisuals -- and so
        // the glow paints over the frame rather than under it.
        rml += "<div class=\"halo\"></div>";
        rml += "</div>";
    }

    m_shipSlotLayer->SetInnerRML(rml);

    // SetInnerRML destroyed the previous elements, so the handlers go on the
    // fresh ones, as AttachTechListeners does for the tiles.
    m_slotListeners.clear();
    m_slotElements.clear();
    for (int i = 0; i < m_shipSlotLayer->GetNumChildren() && i < static_cast<int>(m_shownSlots.size()); ++i) {
        Rml::Element* element = m_shipSlotLayer->GetChild(i);
        m_slotElements.push_back(element);
        const ShipSlotView& slot = m_shownSlots[i];

        m_slotListeners.push_back(std::make_unique<FunctionListener>([this, slot](Rml::Event& event) {
            ShowSlotTip(slot, nullptr);
        }));
        element->AddEventListener("mouseover", m_slotListeners.back().get());
        m_slotListeners.push_back(std::make_unique<FunctionListener>([this](Rml::Event&) {
            ClearTechInfo();
        }));
        element->AddEventListener("mouseout", m_slotListeners.back().get());

        const std::string name = slot.name;
        m_slotListeners.push_back(std::make_unique<FunctionListener>([this, name](Rml::Event&) {
            SelectShipSlot(name);
        }));
        element->AddEventListener("click", m_slotListeners.back().get());
    }

    RebuildShipLists();
}

// Clicking the selected mount again clears it, which is the only way back to
// "nothing picked" -- there is nowhere else on the drawing to click.
void UI::SelectShipSlot(const std::string& name)
{
    m_selectedSlot = name == m_selectedSlot ? std::string() : name;
    // A different mount offers different systems. The repaint at the end of
    // this is what moves the selection ring on the drawing.
    RebuildShipLists();
}

TechRankView UI::RankInSelection(const TechNodeView& node, std::size_t rankIndex) const
{
    if (const ShipSlotView* selected = SelectedSlot()) {
        for (const auto& [id, ranks] : selected->rankStates) {
            if (id != node.id) continue;
            if (rankIndex < ranks.size()) return ranks[rankIndex];
        }
    }
    return rankIndex < node.ranks.size() ? node.ranks[rankIndex] : TechRankView{};
}

// Every level of every system the selected slot will take, in the order both
// the markup and its click handlers walk -- computed once so a row and its
// listener cannot disagree about which fitting they mean.
//
// One row per level rather than per system: a refit is a choice between
// concrete fittings, and a hull that cannot afford III should be able to pick I
// off the same list rather than discovering it by clicking twice.
std::vector<UI::SlotOffer> UI::OfferedForSelection() const
{
    std::vector<SlotOffer> offers;

    const ShipSlotView* selected = SelectedSlot();
    if (!selected) return offers;

    for (const TechNodeView& node : m_shownNodes) {
        if (node.tab != 0 || node.ranks.empty()) continue;
        if (!SlotTakes(*selected, node.id)) continue;

        for (std::size_t i = 0; i < node.ranks.size(); ++i) {
            // A level the faction has not researched is not a fitting the port
            // can offer, and one gated behind another part is not one this hull
            // can take yet. Neither belongs on a list of choices.
            const TechRankState state = RankInSelection(node, i).state;
            if (state == TechRankState::NotUnlocked || state == TechRankState::Locked) continue;
            offers.push_back(SlotOffer{&node, i});
        }
    }
    return offers;
}

void UI::RebuildShipLists()
{
    if (!m_installedList || !m_availableList) return;

    // One line per hole rather than per system: a hull with a gun on each wing
    // carries two guns, and a list that folded them into one entry would be
    // describing a different ship. The slot's own name goes on the row, since
    // two identical entries are otherwise indistinguishable.
    std::string installed;
    int installedCount = 0;
    // Which node each row's round count belongs to, in row order (0 for a row
    // that shows none). Turned into element pointers once the markup is in, so
    // the per-frame refresh never searches the document.
    std::vector<std::uint32_t> rowAmmoNode;
    // Which mount each row's line sits in, and what its note says while no
    // pull is planned against that mount -- both resolved to elements below,
    // so a staged strip can say so without any of this being written again.
    std::vector<std::pair<std::string, std::string>> rowMount;
    for (const ShipSlotView& slot : m_shownSlots) {
        if (!RefOf(slot).IsHole() || slot.fittedId == 0) continue;
        for (const TechNodeView& node : m_shownNodes) {
            if (node.tab != 0 || node.id != slot.fittedId) continue;
            ++installedCount;
            const std::string ammo = AmmoSpan(node);
            const std::string note = Upper(slot.name) + " &#183; " + RankNumeral(node.rank);
            rowAmmoNode.push_back(ammo.empty() ? 0 : node.id);
            rowMount.emplace_back(slot.name, note);
            installed += "<div class=\"entry installed\"><span class=\"entry_name\">" + node.name
                       + "</span>" + ammo + "<span class=\"entry_note\">"
                       + note + "</span></div>";
        }
    }
    // Then what the hull carries that sits in no hole at all -- a shield, the
    // overburn. A line that *does* go in one is listed above or not at all: a
    // rank the hull has paid for but has in no hole is not fitted, whatever
    // the levels still say (see UpgradeCatalog::StripRank).
    for (const TechNodeView& node : m_shownNodes) {
        if (node.tab != 0 || node.rank <= 0 || NodeIsMounted(node)) continue;
        ++installedCount;

        const std::string ammo = AmmoSpan(node);
        const ShipSlotView* slot = SlotHolding(node);
        rowAmmoNode.push_back(ammo.empty() ? 0 : node.id);
        rowMount.emplace_back(slot ? slot->name : std::string(), RankNumeral(node.rank));
        installed += "<div class=\"entry installed\"><span class=\"entry_name\">" + node.name
                   + "</span>" + ammo + "<span class=\"entry_note\">"
                   + RankNumeral(node.rank) + "</span></div>";
    }

    if (installed.empty()) {
        installed = "<div class=\"list_empty\">Nothing fitted. The hull is stock.</div>";
    }

    std::string available;
    const ShipSlotView* selected = SelectedSlot();
    const std::vector<SlotOffer> offers = OfferedForSelection();
    if (selected) {
        // Emptying the slot is a choice like any other, so it sits on the list
        // rather than being the absence of one. On a slot that already carries
        // something it is the only way to get the part off the hull -- and it
        // says so, because pulling one buys nothing back.
        const bool carries = FittedIn(*selected) != nullptr;
        available += std::string("<div class=\"entry none\"><span class=\"entry_name\">")
                   + (carries ? "STRIP THIS MOUNT" : "NOTHING")
                   + "</span><span class=\"entry_note\">"
                   + (carries ? "NO REFUND" : "CLEAR") + "</span></div>";
    }
    for (const SlotOffer& offer : offers) {
        const TechRankView rank = RankInSelection(*offer.node, offer.rankIndex);

        const char* entryClass = "entry";
        std::string note = std::to_string(rank.cost) + " SUP";
        switch (rank.state) {
        case TechRankState::Available:    entryClass = "entry buy"; break;
        case TechRankState::Unaffordable: entryClass = "entry poor"; break;
        case TechRankState::NeedsLanding: entryClass = "entry poor"; note = "LAND"; break;
        case TechRankState::Held:         entryClass = "entry installed"; note = "FITTED"; break;
        default:                          note = "LOCKED"; break;
        }

        available += "<div class=\"" + std::string(entryClass) + "\"><span class=\"entry_name\">"
                   + offer.node->name + " " + RankNumeral(static_cast<int>(offer.rankIndex) + 1)
                   + "</span><span class=\"entry_note\">" + note + "</span></div>";
    }
    if (!selected) {
        available = "<div class=\"list_empty\">Select a mount on the hull to see what it takes.</div>";
    }
    else if (offers.empty()) {
        available += "<div class=\"list_empty\">Nothing the faction has researched fits this mount.</div>";
    }

    m_installedList->SetInnerRML(installed);
    m_availableList->SetInnerRML(available);

    // The round counts, resolved to elements now that the rows exist. Child 1 of
    // a row is its count span, which is why AmmoSpan is written second -- the
    // name is child 0 and the mount note is last.
    m_ammoSpans.clear();
    m_installedRows.clear();
    for (std::size_t row = 0; row < rowAmmoNode.size(); ++row) {
        Rml::Element* entry = m_installedList->GetChild(static_cast<int>(row));
        if (!entry) continue;
        if (rowAmmoNode[row] != 0) {
            if (Rml::Element* span = entry->GetChild(1)) {
                m_ammoSpans.push_back(AmmoSpanRef{span, rowAmmoNode[row], -1, -1});
            }
        }
        // The note is last, whether or not a count came before it.
        Rml::Element* note = entry->GetChild(entry->GetNumChildren() - 1);
        if (note) m_installedRows.push_back(InstalledRowRef{entry, note, rowMount[row].first,
                                                            rowMount[row].second});
    }

    if (m_installedCount) m_installedCount->SetInnerRML(std::to_string(installedCount));
    if (m_availableCount) {
        m_availableCount->SetInnerRML(selected ? std::to_string(offers.size()) : "--");
    }

    AttachShipListListeners();
    // The markup above is written from the hull, which knows nothing of a plan
    // standing against it -- so a refit staged before this rebuild would come
    // back unmarked. Last, once every element it repaints has been resolved.
    RefreshStagedVisuals();
}

// A row is one fitting: clicking it stages exactly that level, rather than
// stepping through them the way a tile does. CLEAR plans the slot empty, or
// takes back the plan on one that is already empty.
void UI::AttachShipListListeners()
{
    m_listListeners.clear();
    m_availableRows.clear();
    m_noneRow = nullptr;
    if (!m_availableList || m_selectedSlot.empty()) return;

    int child = 0;
    if (Rml::Element* none = m_availableList->GetChild(child++)) {
        m_noneRow = none;
        m_listListeners.push_back(std::make_unique<FunctionListener>([this](Rml::Event&) {
            ClearStagedForSelection();
        }));
        none->AddEventListener("click", m_listListeners.back().get());
    }

    for (const SlotOffer& offer : OfferedForSelection()) {
        Rml::Element* row = m_availableList->GetChild(child++);
        if (!row) break;
        m_availableRows.push_back(row);

        if (RankInSelection(*offer.node, offer.rankIndex).state != TechRankState::Available) continue;

        const std::uint32_t id = offer.node->id;
        const int rank = static_cast<int>(offer.rankIndex) + 1;
        const SlotRef hole = SelectionHole();
        m_listListeners.push_back(
                std::make_unique<FunctionListener>([this, id, rank, hole](Rml::Event&) {
            SetStagedRank(id, 0, rank, hole);
        }));
        row->AddEventListener("click", m_listListeners.back().get());
    }
}

const ShipSlotView* UI::SelectedSlot() const
{
    return SlotNamed(m_selectedSlot);
}

const ShipSlotView* UI::SlotNamed(const std::string& name) const
{
    if (name.empty()) return nullptr;
    for (const ShipSlotView& slot : m_shownSlots) {
        if (slot.name == name) return &slot;
    }
    return nullptr;
}

// Where a line that goes into no numbered hole sits on the drawing -- a shield,
// the drive. Exactly one slot takes each of those, by category.
const ShipSlotView* UI::SlotHolding(const TechNodeView& node) const
{
    for (const ShipSlotView& slot : m_shownSlots) {
        if (!RefOf(slot).IsHole() && SlotTakes(slot, node.id)) return &slot;
    }
    return nullptr;
}

UI::SlotRef UI::SelectionHole() const
{
    const ShipSlotView* selected = SelectedSlot();
    return selected ? RefOf(*selected) : SlotRef{};
}

// What CLEAR does. A slot carrying something is planned empty -- that is the
// only way to get a part off a hull. One that is already empty has nothing to
// pull, so CLEAR simply takes back whatever was planned for it.
void UI::ClearStagedForSelection()
{
    const ShipSlotView* selected = SelectedSlot();
    if (!selected) return;

    if (const TechNodeView* fitted = FittedIn(*selected)) {
        StageStrip(fitted->id, RefOf(*selected));
        return;
    }

    const SlotRef hole = RefOf(*selected);
    for (std::size_t i = m_staged.size(); i > 0; --i) {
        const StagedPick& staged = m_staged[i - 1];
        if (staged.tab != 0) continue;
        const bool same = hole.IsHole() ? staged.hole == hole
                                        : (!staged.hole.IsHole() && SlotTakes(*selected, staged.id));
        if (!same) continue;

        m_staged.erase(m_staged.begin() + static_cast<std::ptrdiff_t>(i - 1));
        RefreshStagedVisuals();
        return;
    }
}

void UI::SetTechTree(const std::vector<TechNodeView>& nodes)
{
    if (nodes == m_shownNodes) {
        // Same board, possibly different round counts (see TechNodeView's
        // operator==, which ignores them on purpose). Copy them across and move
        // the few spans that show them rather than rebuilding any markup.
        for (std::size_t i = 0; i < m_shownNodes.size() && i < nodes.size(); ++i) {
            m_shownNodes[i].ammo = nodes[i].ammo;
            m_shownNodes[i].ammoCapacity = nodes[i].ammoCapacity;
        }
        RefreshAmmoSpans();
        return;
    }
    m_shownNodes = nodes;
    RebuildTechTree();
}

// The installed list's round counts, updated in place. Cheap enough to call
// every frame: it writes only to spans whose number actually changed, and a hull
// carrying nothing that runs out has no spans to walk at all.
void UI::RefreshAmmoSpans()
{
    for (AmmoSpanRef& span : m_ammoSpans) {
        const TechNodeView* node = nullptr;
        for (const TechNodeView& candidate : m_shownNodes) {
            if (candidate.tab == 0 && candidate.id == span.node) { node = &candidate; break; }
        }
        if (!node || node->ammo < 0) continue;
        if (node->ammo == span.shownAmmo && node->ammoCapacity == span.shownCapacity) continue;

        span.shownAmmo = node->ammo;
        span.shownCapacity = node->ammoCapacity;
        span.element->SetInnerRML(std::to_string(node->ammo) + " / "
                                  + std::to_string(node->ammoCapacity));
        span.element->SetClass("dry", node->ammo == 0);
    }
}

void UI::RebuildTechTree()
{
    if (!m_techGrid) return;

    // Header count: how much of the whole board this hull (or this side) is
    // carrying. Computed, never authored -- a new [[upgrade]] moves it.
    int fittedTotal = 0;
    int rankTotal = 0;
    for (const TechNodeView& node : m_shownNodes) {
        if (node.tab != m_techTab) continue;
        fittedTotal += node.rank;
        rankTotal += node.maxRank;
    }
    if (m_techFitted) {
        m_techFitted->SetInnerRML("FITTED " + std::to_string(fittedTotal) + " / "
                                  + std::to_string(rankTotal));
    }

    std::string rml;
    for (int branch = 0; branch < BRANCH_COUNT; ++branch) {
        int branchFitted = 0;
        int branchTotal = 0;
        for (const TechNodeView& node : m_shownNodes) {
            if (node.tab != m_techTab || node.branch != branch) continue;
            branchFitted += node.rank;
            branchTotal += node.maxRank;
        }

        rml += "<div class=\"branch " + std::string(BRANCH_CLASSES[branch]) + "\">";
        rml += "<div class=\"branch_head\"><span class=\"role\">" + std::string(BRANCH_NAMES[branch])
               + "</span><span class=\"count\">" + std::to_string(branchFitted) + " / "
               + std::to_string(branchTotal) + "</span></div>";

        for (const TechNodeView& node : m_shownNodes) {
            if (node.tab != m_techTab || node.branch != branch) continue;

            bool anyOffered = false;
            for (const TechRankView& rank : node.ranks) {
                anyOffered = anyOffered || rank.state != TechRankState::Locked;
            }
            const bool fitted = node.rank > 0;
            const bool dim = !fitted && !anyOffered;

            // Only a node that genuinely hangs off another gets a run down to
            // it. That is also what keeps the two shields unconnected: they
            // are alternatives, and a line between them would read as one
            // being progress toward the other.
            if (node.requiresId != 0) {
                rml += std::string("<div class=\"connector") + (dim ? " dim" : "") + "\"></div>";
            }

            std::string rowClass = "tile_row";
            if (fitted) rowClass += " fitted";
            else if (dim) rowClass += " dim";

            std::string tileClass = "tile";
            if (fitted) tileClass += " fitted";
            else if (dim) tileClass += " dim";

            // A staged rank is what the tile reports, so the counter answers
            // the click that just happened rather than the state on the hull --
            // which does not move until CONFIRM.
            const int stagedRank = StagedRankFor(node.id, m_techTab);
            const std::string counterText = std::to_string(stagedRank > 0 ? stagedRank : node.rank)
                                            + "/" + std::to_string(node.maxRank);
            if (stagedRank > 0) tileClass += " staged";

            rml += "<div class=\"" + rowClass + "\">";
            rml += "<div class=\"" + tileClass + "\">";
            rml += "<div class=\"glyph\">" + Upper(node.icon) + "</div>";
            rml += "<div class=\"counter\">" + counterText + "</div>";
            // Last, so glyph and counter keep the child indices
            // AttachTechListeners resolves them by.
            rml += "<div class=\"halo\"></div>";
            rml += "</div>";
            rml += "<div class=\"tile_text\"><span class=\"name\">" + node.name + "</span><div class=\"pips\">";
            for (std::size_t i = 0; i < node.ranks.size(); ++i) {
                const TechRankView& rank = node.ranks[i];
                // Filled up to the rank held, or to the one staged: the bar is
                // a level meter, and "2/3" with only the second bar lit reads
                // as neither the rank nor the count. Off the rank rather than
                // off its state, since a rank under the one carried is on
                // offer as a downgrade and so does not read as held.
                const bool staged = static_cast<int>(i) < stagedRank;
                const bool held = stagedRank == 0 && static_cast<int>(i) < node.rank;
                rml += "<div class=\"pip "
                       + std::string(staged ? "staged" : held ? "fitted" : PipClass(rank.state))
                       + "\"></div>";
            }
            rml += "</div></div></div>";
        }
        rml += "</div>";
    }

    m_techGrid->SetInnerRML(rml);
    AttachTechListeners();
    RebuildShipSlots(); // which rebuilds the lists under it
    RefreshTechFooter();
    RefreshConfirmButton();
}

// SetInnerRML destroyed the previous elements, so the handlers go on the fresh
// ones -- and the old handler objects are dropped -- on every rebuild. This is
// also where the elements a staged pick repaints are resolved, so staging
// itself never has to touch the markup again (see RefreshStagedVisuals).
void UI::AttachTechListeners()
{
    m_techListeners.clear();
    m_tileRefs.clear();
    if (!m_techGrid) return;

    for (int b = 0; b < m_techGrid->GetNumChildren(); ++b) {
        Rml::Element* column = m_techGrid->GetChild(b);
        int cursor = 0;
        for (const TechNodeView& node : m_shownNodes) {
            if (node.tab != m_techTab || node.branch != b) continue;

            // The column holds a header, then a connector-and-row per node.
            Rml::Element* row = nullptr;
            for (int i = cursor + 1; i < column->GetNumChildren(); ++i) {
                if (column->GetChild(i)->GetTagName() == "div"
                    && column->GetChild(i)->IsClassSet("tile_row")) {
                    row = column->GetChild(i);
                    cursor = i;
                    break;
                }
            }
            if (!row) break;

            const std::uint32_t id = node.id;

            TileRefs refs;
            refs.id = id;
            refs.maxRank = node.maxRank;
            refs.heldRank = node.rank;

            // Hover anywhere on the row reads it out in the info panel.
            m_techListeners.push_back(std::make_unique<FunctionListener>([this, id](Rml::Event&) {
                ShowTechTip(id, nullptr);
            }));
            row->AddEventListener("mouseover", m_techListeners.back().get());
            m_techListeners.push_back(std::make_unique<FunctionListener>([this](Rml::Event&) {
                ClearTechInfo();
            }));
            row->AddEventListener("mouseout", m_techListeners.back().get());

            // The tile has no rank of its own to mean, so it means "one more".
            if (Rml::Element* tile = row->GetChild(0)) {
                refs.tile = tile;
                refs.counter = tile->GetChild(1);

                m_techListeners.push_back(
                        std::make_unique<FunctionListener>([this, id](Rml::Event&) {
                            CycleStagePick(id, m_techTab);
                        }));
                tile->AddEventListener("click", m_techListeners.back().get());
            }

            Rml::Element* text = row->GetChild(1);
            Rml::Element* pips = text ? text->GetChild(1) : nullptr;
            if (!pips) {
                m_tileRefs.push_back(std::move(refs));
                continue;
            }
            for (int i = 0; i < pips->GetNumChildren(); ++i) {
                if (static_cast<std::size_t>(i) >= node.ranks.size()) break;
                refs.pips.push_back(pips->GetChild(i));

                // A pip stages the rank it is -- the one place an exact rank
                // can be named rather than stepped to.
                const int rank = i + 1;
                if (!Stageable(node, rank, m_techTab)) continue;
                m_techListeners.push_back(
                        std::make_unique<FunctionListener>([this, id, rank](Rml::Event&) {
                            StagePick(id, m_techTab, rank);
                        }));
                pips->GetChild(i)->AddEventListener("click", m_techListeners.back().get());
            }
            m_tileRefs.push_back(std::move(refs));
        }
    }
}

void UI::ShowSlotTip(const ShipSlotView& slot, Rml::Element*)
{
    std::string accepts;
    for (const std::string& category : slot.categories) {
        if (!accepts.empty()) accepts += " / ";
        accepts += Upper(category);
    }
    if (accepts.empty()) accepts = "NOTHING";

    const TechNodeView* fitted = FittedIn(slot);
    const TechNodeView* staged = StagedIn(slot);
    const bool stripping = PlanFor(slot).strip;

    std::string body;
    if (stripping && fitted) body = fitted->name + ", to be pulled on confirmation. ";
    else if (staged) body = staged->name + ", pending confirmation. ";
    else if (fitted) body = fitted->name + " " + RankNumeral(fitted->rank) + ". ";
    body += "Accepts " + accepts + ".";

    WriteTechInfo(Upper(slot.name), body,
                  stripping ? "STRIP" : staged ? "STAGED" : fitted ? "FITTED" : "EMPTY",
                  stripping ? "short" : staged ? "ok" : fitted ? "paid" : "unres");
}

void UI::ShowTechTip(std::uint32_t id, Rml::Element*)
{
    if (!m_techInfo) return;

    const TechNodeView* node = nullptr;
    for (const TechNodeView& candidate : m_shownNodes) {
        if (candidate.tab == m_techTab && candidate.id == id) node = &candidate;
    }
    if (!node || node->ranks.empty()) return;

    // The rank being described: the one staged if there is one, else the one
    // past what is carried, else the top of the line -- the same rank the tile
    // is reporting. Counted off the rank rather than found by scanning for the
    // last held state, which no longer runs contiguously from the bottom.
    std::size_t index = std::min(static_cast<std::size_t>(std::max(node->rank, 0)),
                                 node->ranks.size() - 1);
    if (const int staged = StagedRankFor(node->id, m_techTab)) index = static_cast<std::size_t>(staged - 1);
    const TechRankView& rank = node->ranks[index];

    const char* costClass = "unres";
    switch (rank.state) {
    case TechRankState::Held:         costClass = "paid"; break;
    case TechRankState::Available:    costClass = "ok"; break;
    case TechRankState::Unaffordable:
    case TechRankState::NeedsLanding: costClass = "short"; break;
    default:                          costClass = "unres"; break;
    }

    std::string body = node->description;
    if (rank.state == TechRankState::Locked) body = "Locked -- fit what it bolts onto first.";
    else if (rank.state == TechRankState::NotUnlocked) body = "Not researched. See the TECH tab.";
    else if (rank.state == TechRankState::NeedsLanding) body = "Land at one of your labs to fit this.";

    WriteTechInfo(node->name + " " + RankNumeral(static_cast<int>(index) + 1), body,
                  std::to_string(rank.cost) + (m_techTab == 0 ? " SUP" : " TECH"), costClass);
}

void UI::WriteTechInfo(const std::string& title, const std::string& body, const std::string& cost,
                       const char* costClass)
{
    if (!m_techInfo) return;
    m_techInfo->SetClass("idle", false);
    m_techInfo->SetInnerRML("<div class=\"info_text\"><span class=\"info_title\">" + title
                            + "</span><span class=\"info_body\">" + body
                            + "</span></div><span class=\"info_cost " + costClass + "\">" + cost
                            + "</span>");
}

// The info panel carries every word about a system, so what is left down here
// is the port's answer when it turns a purchase away -- and nothing else.
void UI::RefreshTechFooter()
{
    if (!m_techNoticeElement) return;
    m_techNoticeElement->SetInnerRML(m_techNotice.empty()
                                             ? ""
                                             : "PORT AUTHORITY &#8212; " + m_techNotice);
}

void UI::SetTechNotice(const std::string& text)
{
    if (text == m_techNotice) return;
    m_techNotice = text;
    RefreshTechFooter();
}

void UI::SetTechTab(int tab)
{
    if (!m_techTree || tab == m_techTab) return;
    m_techTab = tab;
    if (Rml::Element* ship = m_techTree->GetElementById("tab_ship")) ship->SetClass("active", tab == 0);
    if (Rml::Element* permanent = m_techTree->GetElementById("tab_permanent")) {
        permanent->SetClass("active", tab == 1);
    }
    if (m_shipViewPanel) m_shipViewPanel->SetClass("active", tab == 0);
    if (m_techGrid) m_techGrid->SetClass("active", tab == 1);
    ClearTechInfo();
    RequestTechRebuild();
}

void UI::SetCurrencies(int tech, int supplies)
{
    if (tech == m_shownTech && supplies == m_shownSupplies) return;
    m_shownTech = tech;
    m_shownSupplies = supplies;

    const std::string techText = std::to_string(tech);
    const std::string supplyText = std::to_string(supplies);

    if (m_techValue) m_techValue->SetInnerRML(techText);
    if (m_supplyValue) m_supplyValue->SetInnerRML(supplyText);
    if (m_hudTechValue) m_hudTechValue->SetInnerRML(techText);
    if (m_hudSuppliesValue) m_hudSuppliesValue->SetInnerRML(supplyText);
}

void UI::SetTechPickCallback(
        std::function<void(std::uint32_t, int, int, std::uint8_t, bool)> callback)
{
    m_onTechPick = std::move(callback);
}

void UI::SetResupplyOffer(int cost, bool available)
{
    if (cost == m_resupplyCost && available == m_resupplyAvailable) return;
    m_resupplyCost = cost;
    m_resupplyAvailable = available;
    RefreshResupplyButton();
}

void UI::SetResupplyCallback(std::function<void()> callback)
{
    m_onResupply = std::move(callback);
}

void UI::SetTechTreeVisible(bool visible)
{
    if (!m_techTree) return;

    if (visible) {
        m_techTree->Show();
        // The boards come back with the next RefreshTechTree, which the toggle
        // runs on the frame it opens -- DiscardTechMarkup emptied the caches
        // the Set* guards compare against, so that call rebuilds rather than
        // deciding nothing changed.
        return;
    }

    m_techTree->Hide();
    DiscardTechMarkup();
}

// A hidden document is not a free one: RmlUi walks every element of every
// document each Context::Update, `display: none` included, so a board left
// standing behind a closed window is paid for on every frame of the round.
// Nothing here is state -- it is all redrawn from the sim when the window
// opens -- except the staged plan, which is deliberately kept: closing the
// window to go and look at something should not throw away a refit half
// planned.
void UI::DiscardTechMarkup()
{
    if (m_techGrid) m_techGrid->SetInnerRML("");
    m_techListeners.clear();
    m_tileRefs.clear();

    if (m_shipSlotLayer) m_shipSlotLayer->SetInnerRML("");
    m_slotListeners.clear();
    m_slotElements.clear();

    if (m_installedList) m_installedList->SetInnerRML("");
    if (m_availableList) m_availableList->SetInnerRML("");
    m_listListeners.clear();
    m_availableRows.clear();
    m_noneRow = nullptr;
    m_ammoSpans.clear();
    m_installedRows.clear();

    m_shownNodes.clear();
    m_shownSlots.clear();
    m_techRebuildPending = false;
}

bool UI::IsTechTreeVisible() const
{
    return m_techTree && m_techTree->IsVisible();
}

static const char* RankClass(TechRankState state)
{
    switch (state) {
    case TechRankState::Locked:       return "locked";
    case TechRankState::Held:         return "held";
    case TechRankState::NotUnlocked:  return "not_unlocked";
    case TechRankState::Unaffordable: return "unaffordable";
    case TechRankState::NeedsLanding: return "needs_landing";
    case TechRankState::Available:    return "available";
    }
    return "locked";
}

int UI::GetSidebarWidthPx() const
{
    return m_sidebar ? static_cast<int>(m_sidebar->GetOffsetWidth()) : 0;
}

void UI::SetIntroConfirmCallback(std::function<void(TeamId, const std::string&)> callback)
{
    m_onIntroConfirm = std::move(callback);
}

void UI::SetSeedApplyCallback(std::function<void(std::uint32_t)> callback)
{
    m_onSeedApply = std::move(callback);
}

void UI::SetSeedRandomizeCallback(std::function<void()> callback)
{
    m_onSeedRandomize = std::move(callback);
}

void UI::SetSeedDisplay(std::uint32_t seed)
{
    if (m_seedInput) m_seedInput->SetAttribute("value", std::to_string(seed));
}

void UI::SetIntroVisible(bool visible)
{
    if (!m_document) return;
    if (visible) m_document->Show();
    else m_document->Hide();
}

void UI::SetSeedRowVisible(bool visible)
{
    if (!m_seedRow) return;

    // An inline property outranks the stylesheet, so showing the row has to
    // drop the property rather than name a display mode -- naming one would
    // override the row's `display: flex` and stack its contents.
    if (visible) m_seedRow->RemoveProperty("display");
    else m_seedRow->SetProperty("display", "none");
}

void UI::SetTeamOptions(const std::vector<TeamId>& teams)
{
    if (!m_document || teams.empty()) return;

    // Must go through the select's own API: it keeps an option list beside
    // the DOM, so SetInnerRML adds to it rather than replacing it and the
    // roster stacks up on every rebuild.
    auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(m_document->GetElementById("team_select"));
    if (!select) return;

    // The picked side has to stay valid: a roster that no longer offers it
    // would leave m_introTeam naming a faction with no complex to launch from.
    if (std::find(teams.begin(), teams.end(), m_introTeam) == teams.end()) m_introTeam = teams.front();

    select->RemoveAll();
    for (TeamId team : teams) {
        const int index = select->Add(TeamLabel(team), TeamOptionValue(team));
        if (team == m_introTeam) select->SetSelection(index);
    }
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

static Rml::Input::KeyIdentifier RmlKey(UiKey key)
{
    switch (key) {
    case UiKey::Backspace: return Rml::Input::KI_BACK;
    case UiKey::Delete: return Rml::Input::KI_DELETE;
    case UiKey::Tab: return Rml::Input::KI_TAB;
    case UiKey::Return: return Rml::Input::KI_RETURN;
    case UiKey::Escape: return Rml::Input::KI_ESCAPE;
    case UiKey::Left: return Rml::Input::KI_LEFT;
    case UiKey::Right: return Rml::Input::KI_RIGHT;
    case UiKey::Up: return Rml::Input::KI_UP;
    case UiKey::Down: return Rml::Input::KI_DOWN;
    case UiKey::Home: return Rml::Input::KI_HOME;
    case UiKey::End: return Rml::Input::KI_END;
    case UiKey::PageUp: return Rml::Input::KI_PRIOR;
    case UiKey::PageDown: return Rml::Input::KI_NEXT;
    case UiKey::A: return Rml::Input::KI_A;
    case UiKey::C: return Rml::Input::KI_C;
    case UiKey::V: return Rml::Input::KI_V;
    case UiKey::X: return Rml::Input::KI_X;
    case UiKey::Y: return Rml::Input::KI_Y;
    case UiKey::Z: return Rml::Input::KI_Z;
    case UiKey::None: break;
    }
    return Rml::Input::KI_UNKNOWN;
}

static int RmlModifiers(int modifiers)
{
    int state = 0;
    if (modifiers & UiKeyModifier_Ctrl) state |= Rml::Input::KM_CTRL;
    if (modifiers & UiKeyModifier_Shift) state |= Rml::Input::KM_SHIFT;
    if (modifiers & UiKeyModifier_Alt) state |= Rml::Input::KM_ALT;
    if (modifiers & UiKeyModifier_Meta) state |= Rml::Input::KM_META;
    return state;
}

// The dropdown carries these as its option values, and SetTeamOptions writes
// them; the two must agree, which is why neither side hardcodes a list.
static const char* TeamOptionValue(TeamId team)
{
    switch (team) {
    case TeamId::Blue: return "blue";
    case TeamId::Red: return "red";
    case TeamId::Violet: return "violet";
    case TeamId::Yellow: return "yellow";
    case TeamId::Magenta: return "magenta";
    case TeamId::Cyan: return "cyan";
    case TeamId::None: break;
    }
    return "";
}

// Display text for the same option. Capitalized rather than the raw value,
// since this is what the player reads.
static const char* TeamLabel(TeamId team)
{
    switch (team) {
    case TeamId::Blue: return "Blue";
    case TeamId::Red: return "Red";
    case TeamId::Violet: return "Violet";
    case TeamId::Yellow: return "Yellow";
    case TeamId::Magenta: return "Magenta";
    case TeamId::Cyan: return "Cyan";
    case TeamId::None: break;
    }
    return "";
}

static std::optional<TeamId> TeamIdFromOption(const Rml::String& value)
{
    for (TeamId team : FACTION_ROSTER) {
        if (value == TeamOptionValue(team)) return team;
    }
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

static std::string EscapeRml(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
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

    if (m_techRebuildPending) {
        m_techRebuildPending = false;
        RebuildTechTree();
        // The markup the cursor was over is gone; hand RmlUi the same position
        // again so the replacement under it is hovered, and clickable, without
        // the player having to jiggle the mouse. Staging no longer rebuilds at
        // all (see RefreshStagedVisuals), so this is only for data changes.
        m_context->ProcessMouseMove(m_mouseX, m_mouseY, 0);
    }

    if (m_chatScrollToEnd && m_chatLog) {
        m_chatScrollToEnd = false;
        m_chatLog->SetScrollTop(m_chatLog->GetScrollHeight());
    }

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
