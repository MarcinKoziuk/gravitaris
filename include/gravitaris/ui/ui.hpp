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
    // Slot categories this fits into; empty belongs to no slot.
    std::vector<std::string> slots;
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
        return id == other.id && branch == other.branch && icon == other.icon && slots == other.slots
               && tab == other.tab && col == other.col && row == other.row
               && name == other.name && description == other.description && rank == other.rank
               && maxRank == other.maxRank && cap == other.cap && requiresId == other.requiresId
               && ranks == other.ranks;
    }
};

// Which primary lines the trigger works, for the cannon row's label. Spelled
// again here, matching ActiveWeapon's order, so ui/ stays free of the sim's
// headers.
enum class PrimaryMode {
    Both,
    Cannon,
    Guns,
};

// A slot that sits on no numbered hull hole -- a shield, the engine. Matches
// TechPick::NO_MOUNT; spelled again here so ui/ stays free of the sim's
// headers.
inline constexpr std::uint8_t NO_SLOT_MOUNT = 0xFF;

// Which family of holes a slot addresses. Matches the sim's SlotFamily, and
// spelled again for the same reason: a hull numbers its weapon mounts and its
// missile bays separately, so an index means nothing without one of these.
enum class SlotFamilyView : std::uint8_t {
    None,
    Weapon,
    MissileBay,
};

// One fitting position on the ship tab's schematic. `x`/`y` are 0..1 across
// the drawing with y down, so this layer places slots without knowing
// anything about model space or how the panel was fitted.
struct ShipSlotView {
    std::string name;                     // authored label, e.g. "gun+cannon_0"
    std::vector<std::string> categories;  // what the slot accepts
    float x = 0.f;
    float y = 0.f;
    // Which family of holes this slot addresses, and which of them it is --
    // together, what a pick names so the part goes here and not into the next
    // free one. None/NO_SLOT_MOUNT for a slot that sits on no hole.
    SlotFamilyView family = SlotFamilyView::None;
    std::uint8_t mount = NO_SLOT_MOUNT;
    // The node fitted in this hole right now, 0 for an empty one.
    std::uint32_t fittedId = 0;
    // Per-rank cost and state for each mounted line *in this hole*, keyed by
    // node id. The ship-wide states on TechNodeView answer a different
    // question: whether the hull owns a rank, not whether it can go here.
    std::vector<std::pair<std::uint32_t, std::vector<TechRankView>>> rankStates;

    bool operator==(const ShipSlotView& other) const
    {
        return name == other.name && categories == other.categories
               && x == other.x && y == other.y
               && family == other.family && mount == other.mount && fittedId == other.fittedId
               && rankStates == other.rankStates;
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

    Rml::Element* m_cannonTicks = nullptr;
    Rml::Element* m_cannonValue = nullptr;
    Rml::Element* m_cannonMode = nullptr;
    int m_cannonAmmo = -2;
    int m_cannonCapacity = -1;
    PrimaryMode m_cannonPrimary = PrimaryMode::Both;

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
    Rml::Element* m_shipViewPanel = nullptr;
    Rml::Element* m_shipSlotLayer = nullptr;
    Rml::Element* m_installedList = nullptr;
    Rml::Element* m_availableList = nullptr;
    Rml::Element* m_installedCount = nullptr;
    Rml::Element* m_availableCount = nullptr;
    std::vector<ShipSlotView> m_shownSlots;
    // Which mount AVAILABLE SYSTEMS is listing for; empty until one is
    // picked, which is a state the list says out loud rather than guessing at.
    std::string m_selectedSlot;
    // One row of AVAILABLE SYSTEMS: the node, and which of its ranks the row
    // is about.
    struct SlotOffer {
        const TechNodeView* node = nullptr;
        std::size_t rankIndex = 0;
    };
    [[nodiscard]] std::vector<SlotOffer> OfferedForSelection() const;
    // The rank as the selected hole sees it: its own cost and state, which is
    // what decides whether a row can be picked. Falls back to the node's
    // ship-wide rank for anything that sits in no hole.
    [[nodiscard]] TechRankView RankInSelection(const TechNodeView& node, std::size_t rankIndex) const;

    // Which hull hole a slot or a staged pick names. Neither half identifies
    // one on its own: a hull numbers its weapon mounts and its missile bays
    // separately, so weapon 1 and bay 1 are both index 1 and are not the same
    // place.
    struct SlotRef {
        SlotFamilyView family = SlotFamilyView::None;
        std::uint8_t mount = NO_SLOT_MOUNT;

        [[nodiscard]] bool IsHole() const
        { return family != SlotFamilyView::None && mount != NO_SLOT_MOUNT; }

        bool operator==(const SlotRef& other) const
        { return family == other.family && mount == other.mount; }
    };
    [[nodiscard]] static SlotRef RefOf(const ShipSlotView& slot)
    { return SlotRef{slot.family, slot.mount}; }

    // A pick chosen but not yet committed. Nothing on this board applies on
    // the click that asks for it -- a refit is a plan you approve, not a
    // sequence of purchases. Ranks are absolute, so one entry per node is
    // enough: picking another rank of the same line replaces it.
    struct StagedPick {
        std::uint32_t id = 0;
        int tab = 0;
        int rank = 0;
        // Which hole it is going into, or none for a fitting that sits in no
        // hole. Two holes can be staged with the same line, so this is part of
        // what makes a staged pick unique.
        SlotRef hole;
        // A plan to pull what is there rather than to fit something. Carries
        // the node so the board knows what is being given up, and rank 0
        // because there is no rank to buy.
        bool strip = false;
    };
    std::vector<StagedPick> m_staged;
    Rml::Element* m_confirmButton = nullptr;
    Rml::Element* m_resetButton = nullptr;
    Rml::Element* m_stagedCost = nullptr;

    void StagePick(std::uint32_t id, int tab, int rank);
    // rank 0 un-stages. `hole` names where a mounted line is going; a pick for
    // that hole replaces whatever was planned there, which is what makes
    // picking a different line in the same slot a swap.
    // The default is spelled out rather than `{}`: a nested class's member
    // initializers aren't in scope yet in an enclosing-class default argument
    // (clang enforces this, MSVC doesn't).
    void SetStagedRank(std::uint32_t id, int tab, int rank,
                       SlotRef hole = SlotRef{SlotFamilyView::None, NO_SLOT_MOUNT});
    // Plans to pull what the selected slot carries. Replaces any fitting
    // staged there, for the same reason a fitting replaces a strip.
    void StageStrip(std::uint32_t id, SlotRef hole);
    // The one plan standing against a slot: which node, at what rank, and
    // whether it pulls rather than fits. Node 0 means the slot is being left
    // alone, which is not the same as being planned empty.
    struct SlotPlan {
        std::uint32_t id = 0;
        int rank = 0;
        bool strip = false;
    };
    [[nodiscard]] SlotPlan PlanFor(const ShipSlotView& slot) const;
    [[nodiscard]] bool SlotTakes(const ShipSlotView& slot, std::uint32_t nodeId) const;
    void CycleStagePick(std::uint32_t id, int tab);
    void ConfirmStaged();
    void ResetStaged();
    void RefreshConfirmButton();
    // The staged rank of a node, or 0 for one nothing is staged against.
    [[nodiscard]] int StagedRankFor(std::uint32_t id, int tab) const;
    // What the hull (or the faction) already holds of a line.
    [[nodiscard]] int HeldRankOf(std::uint32_t id, int tab) const;
    // Whether a plan may include this rank -- see the definition; the two
    // trees answer differently.
    [[nodiscard]] bool Stageable(const TechNodeView& node, int rank, int tab) const;

    // What a slot is carrying, and what is planned for it. A slot that sits on
    // a hull hole reads that hole; the rest are matched by category, since a
    // shield or an overburn belongs to exactly one slot on the drawing.
    [[nodiscard]] const TechNodeView* FittedIn(const ShipSlotView& slot) const;
    [[nodiscard]] const TechNodeView* StagedIn(const ShipSlotView& slot) const;
    // Whether this node goes into a hull hole at all, judged by the slots the
    // drawing authors: a line that does is listed once per hole that carries
    // it, and one that doesn't is listed once for the ship.
    [[nodiscard]] bool NodeIsMounted(const TechNodeView& node) const;

    // Written straight from the hover handlers -- the panel has a place of its
    // own, so there is no flicker to defer around and nothing to anchor.
    void ClearTechInfo();
    // Takes whatever was planned for the selected slot back off the plan --
    // which is not the same as planning to empty it (see StageStrip).
    void ClearStagedForSelection();
    [[nodiscard]] SlotRef SelectionHole() const;
    [[nodiscard]] const ShipSlotView* SelectedSlot() const;

    // A structural rebuild frees the element a click came from, so it can never
    // run from inside a handler: data changes ask for one here and Update()
    // does it between events. Staging does not rebuild at all.
    bool m_techRebuildPending = false;
    void RequestTechRebuild() { m_techRebuildPending = true; }

    // Last cursor position, replayed after a structural rebuild: RmlUi resolves
    // a click against the hover chain it last computed on a mouse *move*, so
    // markup replaced under a still cursor leaves what is beneath it
    // un-hovered, and the next click on it goes nowhere.
    int m_mouseX = 0;
    int m_mouseY = 0;

    // The elements a staged pick changes, resolved once when the board is
    // built. Staging edits these in place: regenerating the markup costs a
    // relayout the eye reads as a flash, and destroys the element the click
    // came from. Which node a click means never depended on that element.
    struct TileRefs {
        std::uint32_t id = 0;
        Rml::Element* tile = nullptr;
        Rml::Element* counter = nullptr;
        std::vector<Rml::Element*> pips;
        int maxRank = 0;
        int heldRank = 0;
    };
    std::vector<TileRefs> m_tileRefs;
    std::vector<Rml::Element*> m_slotElements;   // parallel to m_shownSlots
    std::vector<Rml::Element*> m_availableRows;  // parallel to OfferedForSelection()
    Rml::Element* m_noneRow = nullptr;           // the NOTHING choice above them

    // Repaints everything a staged pick shows in, touching no markup.
    void RefreshStagedVisuals();

    [[nodiscard]] static std::string SlotGlyph(const ShipSlotView& slot, const TechNodeView* shown);

    void RebuildShipSlots();
    void RebuildShipLists();
    void AttachShipListListeners();
    void SelectShipSlot(const std::string& name);
    void ShowSlotTip(const ShipSlotView& slot, Rml::Element* anchor);
    // One writer for the fixed info panel, so its markup lives in one place.
    void WriteTechInfo(const std::string& title, const std::string& body, const std::string& cost,
                       const char* costClass);
    Rml::Element* m_techFitted = nullptr;
    Rml::Element* m_techInfo = nullptr;
    Rml::Element* m_techValue = nullptr;
    Rml::Element* m_supplyValue = nullptr;
    Rml::Element* m_techNoticeElement = nullptr;
    std::vector<TechNodeView> m_shownNodes;
    int m_techTab = 0;
    int m_shownTech = -1;
    int m_shownSupplies = -1;
    std::function<void(std::uint32_t, int, int, std::uint8_t, bool)> m_onTechPick;
    std::string m_techNotice;
    // Held separately from m_listeners: the nodes they belong to are destroyed
    // and rebuilt on every tree change, so these are dropped with them rather
    // than accumulating for the session.
    std::vector<std::unique_ptr<Rml::EventListener>> m_techListeners;
    std::vector<std::unique_ptr<Rml::EventListener>> m_slotListeners;
    std::vector<std::unique_ptr<Rml::EventListener>> m_listListeners;

    // Rebuilds the grid from m_shownNodes for the active tab.
    void RebuildTechTree();

    // Re-attaches the click and hover handlers after a rebuild.
    void AttachTechListeners();

    // Raises / drops the hover tooltip for a node.
    void ShowTechTip(std::uint32_t id, Rml::Element* anchor);

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

    // The cannon's magazine, and which primary the trigger is on. `ammo`
    // negative means the hull carries no cannon at all, which blanks the row
    // rather than drawing an empty one.
    void SetCannonAmmo(int ammo, int capacity, PrimaryMode mode);

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

    // Fired for each pick a confirmed plan commits: the node's id, its tab,
    // the rank being bought, which hull hole it goes into, and whether the
    // pick pulls what is there rather than fitting something (rank 0 then).
    // Set before Init().
    void SetTechPickCallback(
            std::function<void(std::uint32_t, int, int, std::uint8_t, bool)> callback);

    // The tree is a window the player opens, not a panel that appears at them
    // -- so it is browsable in flight, and it is theirs to close.
    void SetTechTreeVisible(bool visible);

    // Which tree is showing: 0 the ship's, 1 the faction's. The client uses
    // this to put the ship tab up when it opens the window for a refit.
    // Where things bolt onto the hull, as the schematic authors them. Empty
    // for now in the sense that nothing is fitted into them yet -- what a slot
    // holds arrives with the loadout.
    void SetShipSlots(const std::vector<ShipSlotView>& slots);

    void SetTechTab(int tab);

    // The ship tab draws a live schematic, which the client only renders
    // while that panel is actually the one on screen.
    [[nodiscard]] int GetTechTab() const { return m_techTab; }

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
