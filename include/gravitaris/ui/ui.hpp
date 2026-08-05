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

// Why a rank is or isn't for sale, mirrored out of the catalog so ui/ can
// style a pip without knowing what an upgrade is.
enum class TechRankState {
    Locked,       // the prerequisite isn't met
    Held,         // already unlocked, or already fitted at this rank or better
    NotUnlocked,  // ship tree: above what the faction has learned
    Unaffordable,
    NeedsLanding, // ship tree: affordable, but the hull isn't at a lab
    Available,
};

// One buyable rank of one node. The ship tree sells a named rank outright, so
// the pips are the buttons and each carries its own price and its own reason
// for being greyed out.
struct TechRankView {
    int cost = 0;
    TechRankState state = TechRankState::Locked;

    bool operator==(const TechRankView& other) const
    { return cost == other.cost && state == other.state; }
};

// One node of one tree as the HUD needs it -- already resolved to display
// text and grid position, so ui/ stays independent of the upgrade catalog.
struct TechNodeView {
    std::uint32_t id = 0;
    int branch = 0;         // 0 weapons, 1 mobility, 2 defense
    std::string icon;       // three-letter placeholder code, later a sprite
    // 0 the ship's own tree, 1 the faction's. Matches TechTab's order.
    int tab = 0;
    int col = 0;
    int row = 0;
    std::string name;
    std::string description;
    int rank = 0;    // what this track holds
    int maxRank = 1;
    int cap = 0;     // ship tree: the faction's unlocked ceiling
    // The node this one hangs off, for the connector; 0 for a root.
    std::uint32_t requiresId = 0;
    std::vector<TechRankView> ranks;

    bool operator==(const TechNodeView& other) const
    {
        return id == other.id && branch == other.branch && icon == other.icon
               && tab == other.tab && col == other.col && row == other.row
               && name == other.name && description == other.description && rank == other.rank
               && maxRank == other.maxRank && cap == other.cap && requiresId == other.requiresId
               && ranks == other.ranks;
    }
};

// One run of a chat line's body, with the literal CSS colour to draw it in
// (empty inherits the line's own). A kill feed names two sides, so a body is
// not one colour.
struct ChatSpan {
    std::string text;
    std::string color;

    bool operator==(const ChatSpan& other) const
    { return text == other.text && color == other.color; }
};

// One chat line as the HUD needs it: the name already resolved, and every
// colour already a literal CSS value -- ui/ knows nothing about teams beyond
// the dropdown, so the caller does those lookups.
struct ChatLineView {
    std::string sender;
    std::vector<ChatSpan> body;
    std::string senderColor = "#cff";

    bool operator==(const ChatLineView& other) const
    {
        return sender == other.sender && body == other.body && senderColor == other.senderColor;
    }
};

// The keys a focused control needs that don't arrive as characters: editing,
// navigation, and the letters behind the ctrl-combos. Printable input comes
// through UI::ProcessTextInput instead, so this list stays short rather than
// mirroring a keyboard. Kept as our own enum so this header stays free of
// RmlUi's.
enum class UiKey {
    None,
    Backspace,
    Delete,
    Tab,
    Return,
    Escape,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    A,
    C,
    V,
    X,
    Y,
    Z,
};

enum UiKeyModifier {
    UiKeyModifier_Ctrl = 1 << 0,
    UiKeyModifier_Shift = 1 << 1,
    UiKeyModifier_Alt = 1 << 2,
    UiKeyModifier_Meta = 1 << 3,
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

    std::function<void(TeamId, const std::string&)> m_onIntroConfirm;
    Rml::Element* m_nameInput = nullptr;
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

    Rml::Element* m_boostFill = nullptr;
    Rml::Element* m_boostValue = nullptr;
    float m_boostFraction = -2.f;
    bool m_boostCooling = false;

    Rml::Element* m_researchFill = nullptr;
    Rml::Element* m_researchValue = nullptr;
    float m_researchFraction = -2.f;
    std::string m_researchText;

    // The sidebar's pair and the board's pair are different elements with
    // the same job, so they need handles of their own.
    Rml::Element* m_hudTechValue = nullptr;
    Rml::Element* m_hudSuppliesValue = nullptr;

    Rml::Element* m_chat = nullptr;
    Rml::Element* m_chatLog = nullptr;
    Rml::Element* m_chatInput = nullptr;
    std::vector<ChatLineView> m_shownChat;
    std::string m_shownChatInput;
    bool m_chatInputActive = false;
    // Set when a rebuild should leave the log pinned to its newest line, and
    // consumed by the next Update(): the scroll extents only exist once the
    // context has laid the fresh markup out.
    bool m_chatScrollToEnd = false;

    Rml::ElementDocument* m_techTree = nullptr;
    Rml::Element* m_techGrid = nullptr;
    Rml::Element* m_techFitted = nullptr;
    Rml::Element* m_techTip = nullptr;
    Rml::Element* m_techValue = nullptr;
    Rml::Element* m_supplyValue = nullptr;
    Rml::Element* m_techNoticeElement = nullptr;
    Rml::Element* m_refitHint = nullptr;
    bool m_refitHintShown = false;
    std::vector<TechNodeView> m_shownNodes;
    int m_techTab = 0;
    int m_shownTech = -1;
    int m_shownSupplies = -1;
    std::function<void(std::uint32_t, int, int)> m_onTechPick;
    std::string m_techNotice;
    // Held separately from m_listeners: the nodes they belong to are destroyed
    // and rebuilt on every tree change, so these are dropped with them rather
    // than accumulating for the session.
    std::vector<std::unique_ptr<Rml::EventListener>> m_techListeners;

    // Rebuilds the grid from m_shownNodes for the active tab.
    void RebuildTechTree();

    // Re-attaches the click and hover handlers after a rebuild.
    void AttachTechListeners();

    // Raises / drops the hover tooltip for a node.
    void ShowTechTip(std::uint32_t id);
    void HideTechTip();

    // Rewrites the footer for the current selection: what it is, what it
    // costs, and whether the button that buys it has any business existing.
    void RefreshTechFooter();


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

    // `delta` in notches, positive scrolling down (RmlUi's sign, which is the
    // opposite of the platform's). True if a document took it -- the chat
    // scrollback is the only thing that does today, and the caller must not
    // also zoom the camera with it.
    bool ProcessMouseWheel(float delta);

    // Editing and navigation keys for whatever holds focus; `modifiers` is a
    // mask of UiKeyModifier. UiKey::None is ignored, so an unmapped key can be
    // passed straight through.
    bool ProcessKeyDown(UiKey key, int modifiers);

    // Printable input for whatever holds focus, as UTF-8.
    bool ProcessTextInput(const std::string& text);

    // True while a control that wants the keyboard -- a text field or a
    // dropdown -- holds focus. The caller must then route keys here and keep
    // them away from gameplay entirely, including its own unmapped bindings:
    // typing "god" into the call sign field must not engage the autopilot.
    [[nodiscard]] bool IsKeyboardCaptured() const;

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

    // The overburn's readout: `fraction` is how much of a burn is available
    // (1 ready, 0 just spent), `cooling` whether that is a cooldown filling
    // rather than a burn running down. Negative blanks the row, for a ship
    // that hasn't collected the upgrade.
    void SetBoostReadout(float fraction, bool cooling);

    // The faction's research bar, 0..1, with `text` the countdown beside it --
    // this layer formats no times, the same way it holds no game state. A
    // negative fraction blanks the row (no lab). Unchanged values are ignored,
    // so calling it every frame is fine.
    void SetResearchReadout(float fraction, const std::string& text);

    // The chat window's scrollback, oldest first; an empty list clears the
    // log. A rebuild keeps the player's scroll position unless they were
    // already reading the newest line, in which case it follows. Only rebuilt
    // when the contents actually change, so calling it every frame is fine.
    void SetChatLog(const std::vector<ChatLineView>& lines);

    // The line being composed, shown under the log while `active`. The caret
    // is this layer's own decoration -- pass the text alone.
    void SetChatInput(bool active, const std::string& text);

    // Every node of both trees, in catalog order. Only rebuilt when the
    // contents actually change, so calling it every frame is fine.
    void SetTechTree(const std::vector<TechNodeView>& nodes);

    // The two counters in the tree window's header. Unchanged values are
    // ignored, so calling it every frame is fine.
    void SetCurrencies(int tech, int supplies);

    // Fired when a rank pip is clicked, with the node's id, its tab and the
    // rank being bought. Set before Init().
    void SetTechPickCallback(std::function<void(std::uint32_t, int, int)> callback);

    // The tree is a window the player opens, not a panel that appears at them
    // -- so it is browsable in flight, and it is theirs to close.
    void SetTechTreeVisible(bool visible);

    // Which tree is showing: 0 the ship's, 1 the faction's. The client uses
    // this to put the ship tab up when it opens the window for a refit.
    void SetTechTab(int tab);

    // Blinks a prompt in the sidebar while a refit is possible. The window
    // itself is never opened for the player: it asks, it doesn't interrupt.
    void SetRefitHintVisible(bool visible);

    // A line the footer shows over its usual contents -- what the port said
    // when it turned a purchase away. Empty restores the normal footer. The
    // caller decides how long it stays: this layer holds no timers.
    void SetTechNotice(const std::string& text);

    [[nodiscard]] bool IsTechTreeVisible() const;

    // Fired when the intro dialog's OK is clicked, with the side the player
    // picked and the call sign they typed (empty if they cleared the field --
    // naming the default is the caller's business, not this layer's). Set
    // before Init(), which is what shows the dialog.
    void SetIntroConfirmCallback(std::function<void(TeamId, const std::string&)> callback);

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

    // Dismisses (or restores) the intro dialog without going through its
    // button, which is what --autostart needs.
    void SetIntroVisible(bool visible);

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
