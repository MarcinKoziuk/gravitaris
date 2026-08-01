#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gravitaris/game/fs/ifilesystem.hpp>
#include <gravitaris/game/component/team.hpp>

namespace Rml {
class Context;
class Element;
class ElementDocument;
class Event;
class EventListener;
}

namespace Gravitaris {

// One entry of the research draft as the HUD needs it -- already resolved to
// display text, so ui/ stays independent of the upgrade catalog.
struct UpgradeOfferView {
    std::string name;
    std::string description;
    // Currently held / how far it goes; maxLevel 0 marks a repeatable
    // restock, which gets no tier line at all.
    int level = 0;
    int maxLevel = 1;

    bool operator==(const UpgradeOfferView& other) const
    {
        return name == other.name && description == other.description && level == other.level
               && maxLevel == other.maxLevel;
    }
};

// One chat line as the HUD needs it: the name already resolved, and the colour
// to draw it in as a literal CSS value -- ui/ knows nothing about teams beyond
// the dropdown, so the caller does that lookup.
struct ChatLineView {
    std::string sender;
    std::string text;
    std::string senderColor = "#cff";

    bool operator==(const ChatLineView& other) const
    {
        return sender == other.sender && text == other.text && senderColor == other.senderColor;
    }
};

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

    // Owned for the context's lifetime, since the documents hold raw pointers
    // to them.
    std::vector<std::unique_ptr<Rml::EventListener>> m_listeners;

    std::function<void(TeamId)> m_onIntroConfirm;
    std::function<void(std::uint32_t)> m_onSeedApply;
    std::function<void()> m_onSeedRandomize;
    Rml::Element* m_seedRow = nullptr;
    Rml::Element* m_seedInput = nullptr;
    std::function<void(float, float)> m_onMinimapClick;
    std::function<void()> m_onRecenter;

    // The side picked in the intro dialog's dropdown; committed when OK is
    // clicked. Must match the option marked `selected` in main.rml.
    TeamId m_introTeam = TeamId::Blue;

    Rml::Element* m_minimap = nullptr;
    Rml::Element* m_recenterButton = nullptr;
    bool m_recenterVisible = true;

    Rml::Element* m_hudStatus = nullptr;
    std::string m_hudStatusText;

    Rml::Element* m_hudPing = nullptr;
    std::string m_hudPingText;

    Rml::Element* m_sidebar = nullptr;
    Rml::Element* m_healthFill = nullptr;
    Rml::Element* m_healthValue = nullptr;
    float m_hullFraction = -1.f;

    Rml::Element* m_missileTicks = nullptr;
    Rml::Element* m_missileValue = nullptr;
    int m_missileAmmo = -2; // no valid count, not even "no subject" (-1)

    Rml::Element* m_speedReadout = nullptr;
    Rml::Element* m_gwellReadout = nullptr;
    std::string m_speedText;
    std::string m_gwellText;

    // Whichever document owns focus; compared against so the class is only
    // rewritten when focus actually moves between documents.
    Rml::ElementDocument* m_activeDocument = nullptr;

    Rml::Element* m_shieldFill = nullptr;
    Rml::Element* m_shieldValue = nullptr;
    Rml::Element* m_shieldSegments = nullptr;
    float m_shieldFraction = -2.f;
    std::string m_shieldStyle;
    // Last drawn per-plate charges, quantised as they are written, so the
    // markup is only rebuilt when a block actually changes shade.
    std::vector<int> m_shownSegments;

    Rml::Element* m_researchFill = nullptr;
    Rml::Element* m_researchValue = nullptr;
    float m_researchFraction = -2.f;
    std::string m_researchText;

    Rml::Element* m_chatLog = nullptr;
    Rml::Element* m_chatInput = nullptr;
    std::vector<ChatLineView> m_shownChat;
    std::string m_shownChatInput;
    bool m_chatInputActive = false;

    Rml::Element* m_upgradeDraft = nullptr;
    Rml::Element* m_upgradeOffers = nullptr;
    std::vector<UpgradeOfferView> m_shownOffers;
    std::function<void(int)> m_onUpgradePick;
    // Held separately from m_listeners: the cards they belong to are destroyed
    // and rebuilt on every draft change, so these are dropped with them rather
    // than accumulating for the session.
    std::vector<std::unique_ptr<Rml::EventListener>> m_offerListeners;

    int m_width = 1280;
    int m_height = 720;
    float m_dpRatio = 1.f;

    // Attaches `handler` to `element`, keeping the listener alive in
    // m_listeners for as long as this UI exists.
    void Listen(Rml::Element& element, const char* event, std::function<void(Rml::Event&)> handler);

    // Marks the document holding focus with class "active", so the theme can
    // tell a live window from a backgrounded one. Can't be done in RCSS: :focus
    // lands on the focused element only, never its ancestors, so it leaves the
    // body as soon as a control inside the window takes focus.
    void RefreshActiveDocument();

    // Shared by the minimap's mousedown and drag events: both just report
    // where the cursor currently is on the panel.
    void HandleMinimapPoint(Rml::Event& event);

public:
    UI(IFilesystem& filesystem);

    ~UI();

    void Update();

    void Render();

    bool Init();

    // Both take effect immediately once Init() has run, and are remembered
    // until then. Call them before Init(): documents are laid out as they load,
    // so a context still on the placeholder size or ratio lays the HUD and the
    // intro dialog out wrong and they jump when the first frame corrects it.
    void SetDimensions(int width, int height);

    void SetDensityIndependentPixelRatio(float ratio);

    bool ProcessMouseMove(int x, int y);
    bool ProcessMouseButton(int rmlButtonIndex, bool down);

    // Sidebar footer: build identity on one line, ping on the next (empty
    // when there's no server). The caller formats both; this layer holds no
    // game or net state. Repeated identical text is ignored, so calling it
    // every frame is fine.
    void SetHudStatus(const std::string& build, const std::string& ping);

    // Hull bar fill, 0..1; negative means "no subject" and blanks the bar.
    // Unchanged values are ignored, so calling it every frame is fine.
    void SetHullFraction(float fraction);

    // Missiles on the rack: the count as a number, plus one tick per round
    // with the rest of `capacity` drawn empty. Negative blanks the row.
    // Unchanged values are ignored, so calling it every frame is fine.
    void SetMissileAmmo(int ammo, int capacity);

    // Sidebar telemetry row: speed in world units/s and gravity field strength
    // in multiples of a planet's surface pull. Heading has no cell of its own
    // -- the compass dial between the two is the heading readout. An empty
    // optional blanks that cell. Repeated identical text is ignored, so calling
    // it every frame is fine.
    void SetHudTelemetry(std::optional<float> speed, std::optional<float> gravityAccel);

    // Shield charge, 0..1. `styleClass` is an RCSS class toggled on the bar
    // to colour it per emitter type ("" for the default) -- a class name
    // rather than an enum, so adding a shield type is a data change here and
    // in hud.rml, not a change to this interface. Negative blanks the row.
    void SetShieldFraction(float fraction, const std::string& styleClass);

    // Per-plate charges, each 0..1, in the model's plate order -- ablative
    // plating draws one block per plate, shaded by that plate's own charge, so
    // a burned-through side reads as a gap. An empty list restores the plain
    // continuous bar SetShieldFraction drives, which is what a bubble wants.
    void SetShieldSegments(const std::vector<float>& charges);

    // The faction's research bar, 0..1, with `text` the countdown beside it --
    // this layer formats no times, the same way it holds no game state. A
    // negative fraction blanks the row (no lab). The bar takes a "ready" class
    // at full, so a waiting upgrade reads differently from a filling bar.
    // Unchanged values are ignored, so calling it every frame is fine.
    void SetResearchReadout(float fraction, const std::string& text);

    // Chat lines over the game view, oldest first; an empty list clears the
    // log. Only rebuilt when the contents actually change, so calling it every
    // frame is fine.
    void SetChatLog(const std::vector<ChatLineView>& lines);

    // The line being composed, shown under the log while `active`. The caret
    // is this layer's own decoration -- pass the text alone.
    void SetChatInput(bool active, const std::string& text);

    // The Lab's offers, in 1/2/3 order; an empty list hides the panel. Only
    // rebuilt when the contents actually change, so calling it every frame is
    // fine.
    void SetUpgradeOffers(const std::vector<UpgradeOfferView>& offers);

    // Fired when an offer card is clicked, with its 1-based slot -- the same
    // number the keyboard sends. Set before Init().
    void SetUpgradePickCallback(std::function<void(int)> callback);

    // Fired when the intro dialog's OK is clicked, with the side the player
    // picked. Set before Init(), which is what shows the dialog.
    void SetIntroConfirmCallback(std::function<void(TeamId)> callback);

    // Rebuilds the side dropdown to offer exactly `teams`, keeping the
    // current pick if it survives. Call after every world build -- the sides
    // worth offering are the ones that got a starting complex.
    void SetTeamOptions(const std::vector<TeamId>& teams);

    // Shows the seed the current sector was built from.
    void SetSeedDisplay(std::uint32_t seed);

    // Hides the whole seed row. A connected client has no business reseeding
    // a sector the server owns, and the server never sends its seed, so
    // leaving the row up would show a live-looking field holding a number
    // that isn't the one the world was built from.
    void SetSeedRowVisible(bool visible);

    // Fired by the setup dialog's Apply, with whatever is in the seed field
    // (an unparseable field is ignored rather than reported -- the value is
    // arbitrary, so there is no wrong answer to explain). Randomize carries
    // no value: the caller picks the seed and echoes it back through
    // SetSeedDisplay, keeping randomness out of this layer.
    void SetSeedApplyCallback(std::function<void(std::uint32_t)> callback);

    void SetSeedRandomizeCallback(std::function<void()> callback);

    // Fired while the minimap panel is pressed or dragged, with the cursor as
    // -1..1 across the map in each axis, +Y up. The UI knows the panel's
    // pixels; mapping that to the world is the caller's job.
    void SetMinimapClickCallback(std::function<void(float, float)> callback);

    // Fired by the recenter button.
    void SetRecenterCallback(std::function<void()> callback);

    // The recenter button only earns its place while the view is off the
    // player's ship. Repeated identical values are ignored.
    void SetRecenterVisible(bool visible);

    // Laid-out width of the gameplay sidebar in context pixels, which the
    // caller insets the game viewport by so the scene never renders under it.
    // The width itself is declared in ui/hud.rml; this reads it back rather
    // than duplicating the number in code. Zero until the first Update().
    [[nodiscard]] int GetSidebarWidthPx() const;

    // Exposes an engine-owned GL texture to RML/RCSS as src="live://name"
    // (see RenderInterfaceGL3::RegisterLiveTexture). Register before the
    // first frame that renders a document referencing it.
    void RegisterLiveTexture(const std::string& name, unsigned glTextureId, int width, int height);

    void ToggleDebugger();
};

} // namespace Gravitaris
