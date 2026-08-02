#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Debugger.h>

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
        m_chatLog = hud->GetElementById("chat_log");
        m_chatInput = hud->GetElementById("chat_input");
        m_boostFill = hud->GetElementById("boost_fill");
        m_boostValue = hud->GetElementById("boost_value");
        m_researchFill = hud->GetElementById("research_fill");
        m_researchValue = hud->GetElementById("research_value");
        m_researchTicks = hud->GetElementById("research_ticks");
        m_upgradeDraft = hud->GetElementById("upgrade_draft");
        m_upgradeOffers = hud->GetElementById("upgrade_offers");
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

        m_seedRow = m_document->GetElementById("seed_row");
        m_seedInput = m_document->GetElementById("seed_input");

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

void UI::SetResearchReadout(float fraction, int stocked, const std::string& text)
{
    if (!m_researchFill || !m_researchValue || !m_researchTicks) return;

    const float quantised =
            fraction < 0.f ? -1.f : std::round(std::clamp(fraction, 0.f, 1.f) * 100.f) / 100.f;
    if (quantised == m_researchFraction && stocked == m_researchStocked && text == m_researchText) return;

    const bool ticksChanged = stocked != m_researchStocked;
    m_researchFraction = quantised;
    m_researchStocked = stocked;
    m_researchText = text;

    if (quantised < 0.f) {
        m_researchFill->SetProperty("width", "0%");
        m_researchFill->SetClass("ready", false);
        m_researchValue->SetInnerRML("--");
        m_researchTicks->SetInnerRML("");
        return;
    }

    const int percent = static_cast<int>(std::lround(quantised * 100.f));
    m_researchFill->SetProperty("width", std::to_string(percent) + "%");
    m_researchFill->SetClass("ready", stocked > 0);
    m_researchValue->SetInnerRML(text);

    // Rebuilt only when the count moves: the bar behind it changes every tick,
    // and rewriting this row with it would reflow the panel for nothing.
    if (ticksChanged) {
        std::string ticks;
        for (int i = 0; i < stocked; ++i) ticks += "<span class=\"research_tick\"></span>";
        m_researchTicks->SetInnerRML(ticks);
    }
}

void UI::SetUpgradeOffers(const std::vector<UpgradeOfferView>& offers)
{
    if (!m_upgradeDraft || !m_upgradeOffers) return;
    if (offers == m_shownOffers) return;

    m_shownOffers = offers;
    m_upgradeDraft->SetProperty("display", offers.empty() ? "none" : "block");
    if (offers.empty()) {
        m_upgradeOffers->SetInnerRML("");
        return;
    }

    std::string rml;
    for (std::size_t i = 0; i < offers.size(); ++i) {
        const UpgradeOfferView& offer = offers[i];
        rml += "<div class=\"offer\">";
        rml += "<div class=\"offer_key\">" + std::to_string(i + 1) + "</div>";
        rml += "<div class=\"offer_name\">" + offer.name + "</div>";
        rml += "<div class=\"offer_desc\">" + offer.description + "</div>";
        // A restock (maxLevel 0) has no tier to report -- taking it twice
        // just means more rounds.
        if (offer.maxLevel > 0) {
            rml += "<div class=\"offer_tier\">Tier " + std::to_string(offer.level + 1) + " / "
                   + std::to_string(offer.maxLevel) + "</div>";
        }
        rml += "</div>";
    }
    m_upgradeOffers->SetInnerRML(rml);

    // SetInnerRML destroyed the previous cards, so the click handlers are
    // attached to the fresh ones -- and the old handler objects dropped -- on
    // every rebuild.
    m_offerListeners.clear();
    for (int i = 0; i < m_upgradeOffers->GetNumChildren(); ++i) {
        const int slot = i + 1;
        m_offerListeners.push_back(std::make_unique<FunctionListener>([this, slot](Rml::Event&) {
            if (m_onUpgradePick) m_onUpgradePick(slot);
        }));
        m_upgradeOffers->GetChild(i)->AddEventListener("click", m_offerListeners.back().get());
    }
}

void UI::SetUpgradePickCallback(std::function<void(int)> callback)
{
    m_onUpgradePick = std::move(callback);
}

int UI::GetSidebarWidthPx() const
{
    return m_sidebar ? static_cast<int>(m_sidebar->GetOffsetWidth()) : 0;
}

void UI::SetIntroConfirmCallback(std::function<void(TeamId)> callback)
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
