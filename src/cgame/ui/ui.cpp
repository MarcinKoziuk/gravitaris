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
        m_gwellReadout = hud->GetElementById("gwell_readout");
        m_shieldFill = hud->GetElementById("shield_fill");
        m_shieldValue = hud->GetElementById("shield_value");
        m_shieldSegments = hud->GetElementById("shield_segments");
        m_chat = hud->GetElementById("chat");
        m_chatLog = hud->GetElementById("chat_log");
        m_chatInput = hud->GetElementById("chat_input");

        if (Rml::Element* grip = hud->GetElementById("chat_grip")) {
            // ElementHandle drags by writing top/left, so the window's
            // bottom-left anchor has to go before the first of those lands --
            // otherwise a definite top and bottom stretch the box between them.
            // Pinning top to where it already is keeps it still meanwhile.
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
        m_refitHint = hud->GetElementById("refit_hint");
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
        m_techFitted = m_techTree->GetElementById("tech_fitted");
        m_techValue = m_techTree->GetElementById("tech_value");
        m_supplyValue = m_techTree->GetElementById("supply_value");
        // Loud rather than silent: without the container the board simply
        // never draws, which looks like a game bug rather than a typo.
        if (!m_techGrid) LOG(error) << "ui: tech-tree.rml has no #tech_branches; the board cannot draw";
        m_techTip = m_techTree->GetElementById("tech_tip");
        m_techNoticeElement = m_techTree->GetElementById("tech_notice");

        if (Rml::Element* grip = m_techTree->GetElementById("tech_grip")) {
            // ElementHandle drags by writing top/left, and the board is centred
            // with `margin: auto` -- so the margin has to go before the first
            // of those lands, or the box is stretched between the two. Pinning
            // it where it already is keeps it still meanwhile.
            Listen(*grip, "dragstart", [this](Rml::Event&) {
                if (!m_techTree) return;
                const int left = static_cast<int>(std::lround(m_techTree->GetAbsoluteLeft()));
                const int top = static_cast<int>(std::lround(m_techTree->GetAbsoluteTop()));
                m_techTree->SetProperty("margin", "0px");
                m_techTree->SetProperty("left", std::to_string(left) + "px");
                m_techTree->SetProperty("top", std::to_string(top) + "px");
            });
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

// The three role branches, in board order. Names and classes only -- what goes
// in them is whatever the pool says belongs there (TechNodeView::branch).
static constexpr const char* BRANCH_NAMES[] = {"WEAPONS", "MOBILITY", "DEFENSE"};
static constexpr const char* BRANCH_CLASSES[] = {"weapons", "mobility", "defense"};
static constexpr int BRANCH_COUNT = 3;

// Tooltip geometry, matching the RCSS it is positioned against.
static constexpr float TILE_SIZE = 54.f;
static constexpr float TIP_WIDTH = 226.f;
static constexpr float TIP_GAP = 14.f;

void UI::SetTechTree(const std::vector<TechNodeView>& nodes)
{
    if (nodes == m_shownNodes) return;
    m_shownNodes = nodes;
    RebuildTechTree();
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

            bool anyHeld = false;
            bool anyOffered = false;
            for (const TechRankView& rank : node.ranks) {
                anyHeld = anyHeld || rank.state == TechRankState::Held;
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

            // A restock has no rank to report, only that it can be bought
            // again. No loaded font is guaranteed to carry U+21BB, so this
            // says RE rather than risking a box.
            const std::string counterText = std::to_string(node.rank) + "/"
                                            + std::to_string(node.maxRank);

            rml += "<div class=\"" + rowClass + "\">";
            rml += "<div class=\"" + tileClass + "\">";
            rml += "<div class=\"glyph\">" + Upper(node.icon) + "</div>";
            rml += "<div class=\"counter\">" + counterText + "</div>";
            rml += "</div>";
            rml += "<div class=\"tile_text\"><span class=\"name\">" + node.name + "</span><div class=\"pips\">";
            for (const TechRankView& rank : node.ranks) {
                rml += "<div class=\"pip " + std::string(PipClass(rank.state)) + "\"></div>";
            }
            rml += "</div></div></div>";
        }
        rml += "</div>";
    }

    m_techGrid->SetInnerRML(rml);
    AttachTechListeners();
    RefreshTechFooter();
}

// SetInnerRML destroyed the previous elements, so the handlers go on the fresh
// ones -- and the old handler objects are dropped -- on every rebuild. A tile
// buys the next rank up; a pip buys the rank it is, which is the whole point
// of a price that is absolute rather than a step.
void UI::AttachTechListeners()
{
    m_techListeners.clear();
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

            // Hover anywhere on the row raises the tooltip for it.
            m_techListeners.push_back(std::make_unique<FunctionListener>([this, id](Rml::Event& event) {
                ShowTechTip(id, event.GetCurrentElement());
            }));
            row->AddEventListener("mouseover", m_techListeners.back().get());
            m_techListeners.push_back(std::make_unique<FunctionListener>([this](Rml::Event&) {
                HideTechTip();
            }));
            row->AddEventListener("mouseout", m_techListeners.back().get());

            // The tile: the next rank this track hasn't got.
            if (Rml::Element* tile = row->GetChild(0)) {
                int next = 0;
                for (std::size_t i = 0; i < node.ranks.size(); ++i) {
                    if (node.ranks[i].state == TechRankState::Available) {
                        next = static_cast<int>(i) + 1;
                        break;
                    }
                }
                if (next != 0) {
                    m_techListeners.push_back(
                            std::make_unique<FunctionListener>([this, id, next](Rml::Event&) {
                                if (m_onTechPick) m_onTechPick(id, m_techTab, next);
                            }));
                    tile->AddEventListener("click", m_techListeners.back().get());
                }
            }

            Rml::Element* text = row->GetChild(1);
            Rml::Element* pips = text ? text->GetChild(1) : nullptr;
            if (!pips) continue;
            for (int i = 0; i < pips->GetNumChildren(); ++i) {
                if (static_cast<std::size_t>(i) >= node.ranks.size()) break;
                if (node.ranks[static_cast<std::size_t>(i)].state != TechRankState::Available) continue;

                const int rank = i + 1;
                m_techListeners.push_back(
                        std::make_unique<FunctionListener>([this, id, rank](Rml::Event&) {
                            if (m_onTechPick) m_onTechPick(id, m_techTab, rank);
                        }));
                pips->GetChild(i)->AddEventListener("click", m_techListeners.back().get());
            }
        }
    }
}

// All the prose lives here rather than on the board: the tiles carry an icon,
// a rank count and a row of pips, and nothing else competes for the glance.
void UI::ShowTechTip(std::uint32_t id, Rml::Element* anchor)
{
    if (!m_techTip || !anchor || !m_techTree) return;

    const TechNodeView* node = nullptr;
    for (const TechNodeView& candidate : m_shownNodes) {
        if (candidate.tab == m_techTab && candidate.id == id) node = &candidate;
    }
    if (!node) return;

    // The rank the tooltip is about: the next one on offer, or the last one
    // held if the line is finished.
    std::size_t index = 0;
    for (std::size_t i = 0; i < node->ranks.size(); ++i) {
        index = i;
        if (node->ranks[i].state != TechRankState::Held) break;
    }
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
    else if (rank.state == TechRankState::NotUnlocked) body = "Not researched. See the PERMANENT tab.";
    else if (rank.state == TechRankState::NeedsLanding) body = "Land at one of your labs to fit this.";

    std::string tip = "<div class=\"tip_head\"><span class=\"tip_title\">" + node->name + " "
                      + RankNumeral(static_cast<int>(index) + 1) + "</span>";
    tip += "<span class=\"tip_cost " + std::string(costClass) + "\">" + std::to_string(rank.cost)
           + (m_techTab == 0 ? " SUP" : " TECH") + "</span></div>";
    tip += "<div class=\"tip_body\">" + body + "</div>";

    m_techTip->SetInnerRML(tip);
    m_techTip->SetProperty("display", "block");

    // Beside the tile it belongs to rather than under the cursor: a tooltip
    // that follows the mouse jitters across a board this dense, and the thing
    // being described has a position of its own already.
    //
    // Absolute coordinates are context-space, so the panel's own origin comes
    // off them -- the tip is positioned within the document, not the screen.
    const float originX = m_techTree->GetAbsoluteLeft();
    const float originY = m_techTree->GetAbsoluteTop();
    const float tileX = anchor->GetAbsoluteLeft() - originX;
    const float tileY = anchor->GetAbsoluteTop() - originY;

    float left = tileX + TILE_SIZE + TIP_GAP;
    // Flipped to the near side when it would hang off the panel, which is what
    // the rightmost branch does at any sensible UI scale.
    if (left + TIP_WIDTH > m_techTree->GetOffsetWidth()) left = tileX - TIP_WIDTH - TIP_GAP;

    m_techTip->SetProperty("left", std::to_string(static_cast<int>(std::lround(left))) + "px");
    m_techTip->SetProperty("top", std::to_string(static_cast<int>(std::lround(tileY))) + "px");
}

void UI::HideTechTip()
{
    if (m_techTip) m_techTip->SetProperty("display", "none");
}

// The design puts every word in the tooltip, so what is left down here is the
// port's answer when it turns a purchase away -- and nothing else.
void UI::RefreshTechFooter()
{
    if (!m_techNoticeElement) return;
    m_techNoticeElement->SetInnerRML(m_techNotice.empty()
                                             ? ""
                                             : "PORT AUTHORITY &mdash; " + m_techNotice);
}

void UI::SetTechNotice(const std::string& text)
{
    if (text == m_techNotice) return;
    m_techNotice = text;
    RefreshTechFooter();
}

void UI::SetRefitHintVisible(bool visible)
{
    if (!m_refitHint || visible == m_refitHintShown) return;
    m_refitHintShown = visible;
    m_refitHint->SetProperty("display", visible ? "block" : "none");
}

void UI::SetTechTab(int tab)
{
    if (!m_techTree || tab == m_techTab) return;
    m_techTab = tab;
    if (Rml::Element* ship = m_techTree->GetElementById("tab_ship")) ship->SetClass("active", tab == 0);
    if (Rml::Element* permanent = m_techTree->GetElementById("tab_permanent")) {
        permanent->SetClass("active", tab == 1);
    }
    HideTechTip();
    RebuildTechTree();
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

void UI::SetTechPickCallback(std::function<void(std::uint32_t, int, int)> callback)
{
    m_onTechPick = std::move(callback);
}

void UI::SetTechTreeVisible(bool visible)
{
    if (!m_techTree) return;
    if (visible) m_techTree->Show();
    else m_techTree->Hide();
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
    if (m_seedRow) m_seedRow->SetProperty("display", visible ? "block" : "none");
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

    // After the update, not inside SetChatLog: the fresh markup has no scroll
    // extents to pin to until the context has laid it out.
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
