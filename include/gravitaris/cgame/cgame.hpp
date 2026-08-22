#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/game.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/client-prediction.hpp>
#include <gravitaris/game/net/net-client.hpp>
#include <gravitaris/cgame/net/own-ship-sync.hpp>
#include <gravitaris/game/net/predicted-tick-clock.hpp>
#include <gravitaris/game/net/simulated-net-transport.hpp>
#include <gravitaris/game/net/webrtc-transport.hpp>

#include <gravitaris/cgame/camera.hpp>
#include <gravitaris/cgame/scene-view.hpp>
#include <gravitaris/cgame/net/cosmetic-bullet-despawner.hpp>
#include <gravitaris/cgame/net/net-diagnostics.hpp>
#include <gravitaris/cgame/net/remote-event-applier.hpp>
#include <gravitaris/cgame/net/remote-shot-applier.hpp>
#include <gravitaris/cgame/net/snapshot-applier.hpp>
#include <gravitaris/game/net/snapshot.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/cgame/net/snapshot-interpolator.hpp>
#include <gravitaris/cgame/camera-director.hpp>
#include <gravitaris/game/gnc/autopilot.hpp>
#include <gravitaris/cgame/renderer/laser-renderer.hpp>
#include <gravitaris/cgame/renderer/simple-model-renderer.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>
#include <gravitaris/cgame/renderer/starfield-renderer.hpp>
#include <gravitaris/cgame/renderer/minimap-renderer.hpp>
#include <gravitaris/cgame/renderer/compass-renderer.hpp>
#include <gravitaris/cgame/renderer/ship-view-renderer.hpp>
#include <gravitaris/cgame/audio/audio-system.hpp>
#include <gravitaris/cgame/fx/hit-flash-system.hpp>
#include <gravitaris/cgame/hud/indicator-renderer.hpp>
#include <gravitaris/cgame/hud/health-bar-renderer.hpp>

namespace Gravitaris {

// Which line renderer draws the scene. Mutually exclusive; switchable at
// runtime from the debug UI for A/B comparison.
enum class RendererKind {
    Simple, // SimpleModelRenderer  — GL LineStrip, no thickness control
    Baked,  // ModelRenderer2       — baked/instanced, pixel-space width
    Mirror, // Baked, but drawing the snapshot-mirror world (net debug, see below)
};

class CGame : public Game {
protected:
    SimpleModelRenderer m_simpleModelRenderer;
    ModelRenderer2 m_modelRenderer2;

    // Snapshot mirror (docs/networking-plan.md 2.5): a second, presentation
    // -only flecs world fed exclusively by serialize -> parse -> apply of the
    // live sim each rendered frame while RendererKind::Mirror is active,
    // drawn by its own ModelRenderer2 -- proves the whole replication path
    // with zero transport. Declared right after m_modelRenderer2 so the
    // mirror renderer's OnCreate<Model> subscription exists before any model
    // loads (models bake into both renderers; debug-only duplicate GL cost).
    flecs::world m_mirrorWorld;
    ModelRenderer2 m_mirrorRenderer2;
    SnapshotApplier m_snapshotApplier;
    ByteWriter m_snapshotScratch;
    std::uint32_t m_mirrorEventCursor = 0;
    std::uint32_t m_mirrorShotCursor = 0;

    StarfieldRenderer m_starfieldRenderer;
    MinimapRenderer m_minimapRenderer;
    CompassRenderer m_compassRenderer;
    ShipViewRenderer m_shipViewRenderer;
    AudioSystem m_audioSystem;
    HitFlashSystem m_hitFlashSystem;
    CameraDirector m_cameraDirector;
    IndicatorRenderer m_indicatorRenderer;
    HealthBarRenderer m_healthBarRenderer;

    // Beams are gathered from whichever worlds are being drawn this frame (in
    // MP that is both: the own predicted ship here, everyone else in the
    // mirror) and drawn in one pass, so the scratch outlives a single gather.
    LaserRenderer m_laserRenderer;
    std::vector<LaserRenderer::Beam> m_beams;
    // Gathered beside them and drawn in the same pass: a charging emitter has
    // one of these and no beam, a burning one has both.
    std::vector<LaserRenderer::Charge> m_charges;

    // What a beam can be stopped by, flattened to circles ahead of the walk
    // over the beams themselves -- see GatherBeams on why it cannot be a query.
    struct BeamTarget {
        flecs::entity entity;
        Magnum::Vector2d pos;
        double radius = 0.;
    };
    std::vector<BeamTarget> m_beamTargets;

    // Shortest a beam may be drawn, as a share of its reach.
    static constexpr double MIN_BEAM_SHARE = 0.05;

    // How far a beam's light carries before it is spent, as a share of the
    // reach that does its damage. Derived from the range rather than authored
    // so a tier that shoots further is seen further, and kept under 1 on
    // purpose: a beam is felt a long way past where it can be seen, and having
    // to fly in to where you can watch it work is the weapon.
    //
    // Three times what it started at -- at a twentieth of the reach the light
    // died so close to the muzzle that the weapon read as broken rather than as
    // short-ranged.
    static constexpr double BEAM_FADE_SHARE = 0.15;

    // The light at a mount at its brightest, in world units of radius: out of
    // nothing as the emitter charges, and held there while the beam burns.
    // Small on purpose -- it is a spark at the muzzle, not a ball on the wing --
    // but it is meant to be read by whoever the beam is pointed at as much as
    // by the pilot, so the renderer holds it to a pixel floor as well.
    static constexpr double BEAM_CHARGE_RADIUS = 3.5;

    // Share of the distance a beam has already faded over that survives a
    // bounce. Near zero would make every deflected leg as bright as a fresh
    // muzzle however far out it happened; 1 would keep the fade honestly
    // continuous and leave the whole effect invisible past a few hundred units,
    // which is what shipping it that way proved. A quarter keeps a bounce at
    // knife range bright, one across the sector dim but findable.
    static constexpr double BEAM_BOUNCE_RELIGHT = 0.25;

    // Both are needed, and in this order: every world being drawn contributes
    // targets before any world's beams are gathered against them. A beam and
    // what it hits are routinely in different worlds -- in a networked game the
    // own ship and everybody else always are.
    void CollectBeamTargets(flecs::world& world);
    void GatherBeams(flecs::world& world);
    void DrawBeams(const Camera& camera);
    void DrawHealthBars(const SceneView& view, const Camera& camera);
    [[nodiscard]] const Body* HullOf(flecs::entity ent);

    // Where one leg of a beam ends, and on what.
    struct BeamStop {
        // How far along the heading it got. The full length asked for when
        // `target` is empty, meaning it met nothing at all.
        double distance = 0.;
        flecs::entity target;
        // The surface it met, from the bounding circle it was tested against --
        // which is a real normal for a round hull and an approximation of one
        // for everything else. Only a deflection reads it.
        Magnum::Vector2d normal{};
    };

    // What stops a beam leaving `from`, ignoring up to two entities (the shooter
    // on the first leg, and whatever deflected it on the ones after). Bounding
    // circles rather than the collision polygons the sim sweeps: this decides
    // where to stop DRAWING, it runs in a mirror world where nothing has physics
    // at all, and being a few units generous at the edge of a silhouette is
    // invisible.
    [[nodiscard]] BeamStop BeamReach(flecs::entity ignoreA, flecs::entity ignoreB,
                                     const Magnum::Vector2d& from,
                                     const Magnum::Vector2d& heading, double range);

    // Share of a beam `ent` would take into itself rather than throw back, as
    // the DRAW sees it: one for anything that is not a charged, deflecting
    // shield. The sim decides this per plate element off the real geometry
    // (DamageSystem::BeamAbsorbShare) while this can only ask whether the hull
    // has any charge left at all, so a beam meeting the one spent plate on an
    // otherwise healthy ship is drawn bouncing where the sim let it through.
    // Visible only in that corner, and the alternative is replicating per-plate
    // geometry to the client purely to draw a kink.
    [[nodiscard]] float BeamAbsorbShareOf(flecs::entity ent);

    RendererKind m_activeRenderer = RendererKind::Baked;

    // Multiplayer client (docs/networking-plan.md 3.5.3): set by
    // ConnectToServer, null otherwise (single-player, unchanged behavior).
    // When set, Render() takes an entirely separate path -- no local sim
    // (Game::Update() must not be called; see IsNetClient()'s doc), remote
    // entities are fed into the mirror world from real snapshots (Phase 4
    // interpolation) instead of Render()'s usual local WriteSnapshot
    // round-trip, and the own ship is a real, locally-predicted m_registry
    // entity (Phase 5's ClientPrediction) rendered through the same
    // CameraDirector/ModelRenderer2/MinimapRenderer single-player uses.
    // Camera framing, the minimap and the HUD all read both worlds in this
    // mode, since every entity but the own ship lives in m_mirrorWorld -- one
    // SceneView (see CurrentSceneView) carries that split, so a consumer
    // can't be written for single-player only by accident.
    std::unique_ptr<WebRtcTransport> m_netTransport;
    // Sits between m_netTransport and m_netClient (constructed with a
    // reference to *this*, not directly to m_netTransport -- see
    // ConnectToServer) so lag/jitter/loss can be dialed in live from the Net
    // debug tab. Params default to SimulatedNetTransport::Params{}'s own
    // defaults (all zero = exact passthrough, negligible overhead), so this
    // is a no-op until the tab's sliders are touched.
    std::unique_ptr<SimulatedNetTransport> m_simulatedTransport;
    std::unique_ptr<NetClient> m_netClient;
    ClientPrediction m_clientPrediction;
    // Reset in OwnShipSync::SpawnIfConfirmed; see its own class doc comment
    // for why it's kept independent of NetClient's wall-clock tick estimate
    // between resyncs.
    PredictedTickClock m_predictedTickClock;
    // Constructed in ConnectToServer once m_netClient exists (OwnShipSync
    // needs a live NetClient&) -- always populated by the time TickNetClient/
    // ReconcileOwnShipIfNeeded/RenderNetClient run, since those are only ever
    // called once m_netClient is set (see Render()/IsNetClient()).
    std::optional<OwnShipSync> m_ownShipSync;

    CosmeticBulletDespawner m_cosmeticBulletDespawner;

    NetDiagnostics m_netDiagnostics;

    void ReconcileOwnShipIfNeeded();

    // Moves whatever the server sent into m_chatLog. Called from Render, so
    // chat keeps flowing on both the net-client and single-player paths.
    void DrainChat();

    void PushChatLine(std::string sender, TeamId team, std::string text);

    // Draws a filled team-colored square at the center of every owned planet
    // (Team != None) in `world`, via `renderer`'s overlay path -- immediate
    // conquest feedback, matching the original's claimed-planet marker (see
    // docs/gwell/screenshots). Planets and their ownership live in whichever
    // world the mode simulates them in; the view knows which.
    ResourcePtr<const Model> m_teamMarkerModel;
    void SubmitPlanetOwnershipMarkers(const SceneView& view);

    // The refit screen's cutaway. One hull for now: which schematic a ship
    // shows will follow its model once there is more than one to show. The
    // shape is held alongside the model for its '@slots' markers, which the
    // bake drops.
    ResourcePtr<const Model> m_shipSchematicModel;
    ResourcePtr<const Shape> m_shipSchematicShape;

    // The shield passes both renderers draw after their standard ones: the
    // model's own '+shield' bubble, or one pass per '+plating' plate. Which is
    // up, how bright, and in what color are questions about a ship's loadout,
    // so the style callbacks live here rather than in the renderer.
    [[nodiscard]] std::vector<ModelRenderer2::ExtraPass> ShieldPasses();

public:
    // Text chat, global for now (a team channel is a flag on the wire and a
    // filter server-side; see ChatSendPacket). Every line the HUD shows comes
    // from the server in multiplayer -- including your own, so nobody sees a
    // different order than anybody else -- and locally in single-player.
    struct ChatLine {
        std::string sender;
        TeamId team = TeamId::None;
        std::string text;
    };

protected:
    // Scrollback depth. Lines are kept until they fall off the end rather than
    // ageing out: the HUD's chat is a window you can scroll, so a line that
    // erased itself would take the history with it.
    static constexpr std::size_t CHAT_HISTORY_LINES = 256;
    std::deque<ChatLine> m_chatLog;
    std::uint64_t m_chatRevision = 0;

    // The latest faction block off the wire, one entry per side that has ever
    // fielded anything. Empty in single-player, which reads FactionState out
    // of its own registry instead.
    std::vector<FactionSnapshot> m_factionSnapshots;
    // The own ship's Supplies and yard access, copied off the wire alongside
    // its loadout -- a predicted own ship never passes through SnapshotApplier,
    // so nothing else would deliver them.
    std::uint32_t m_ownSupplies = 0;
    // How far this client has read the event stream looking for refusals.
    // Its own cursor: ConsumeSince is non-destructive, so the audio system
    // and this walk the same events without either seeing the other.
    std::uint32_t m_lastDenialSeq = 0;
    bool m_ownAtLab = false;

    // Unit the camera is following instead of the own ship; empty = not
    // spectating. May live in either world (see CycleSpectate).
    flecs::entity m_spectateTarget;

    // The worlds and overlay renderer this frame reads and draws into --
    // single-player and multiplayer differ in exactly this and nothing else,
    // as far as the camera and HUD are concerned (see SceneView).
    [[nodiscard]] SceneView CurrentSceneView();

    // Gravity field at a world point: the same sum PhysicsSystem::ApplyGravity
    // builds, with the target mass divided back out. Read by the HUD readout,
    // the compass needle and the camera's trajectory look-ahead.
    [[nodiscard]] Magnum::Vector2d GravityAt(const Magnum::Vector2d& pos);

    // GravityAt evaluated at the camera subject, in the float vector the
    // renderers and the camera director speak. Zero when there is no subject.
    [[nodiscard]] Magnum::Vector2 SubjectGravity();

    // Constructed in ConnectToServer once m_netClient exists (RemoteEventApplier
    // needs a live NetClient&) -- always populated by the time ApplyRemoteEvents
    // runs, since that's only ever called from RenderNetClient, itself gated
    // on m_netClient being set (see Render()).
    std::optional<RemoteEventApplier> m_remoteEventApplier;
    std::optional<RemoteShotApplier> m_remoteShotApplier;
    void ApplyRemoteEvents();

    // Phase 4 tunables (Net debug tab): how far behind the estimated server
    // tick remote entities render (smooths jitter, at the cost of latency)
    // and how far past the newest received snapshot extrapolation is
    // allowed to guess before snapping to it instead.
    // 50ms = 3 snapshots at 60Hz. Was 100ms, which was sized to hide raw
    // packet-arrival jitter back when the render clock passed it straight
    // through; the smoothed clock absorbs that now, leaving this to cover
    // only real loss/reordering. Every tick of it is also a tick of skew
    // between the own ship and everyone else's (see Phase 10).
    float m_interpDelaySeconds = 0.05f;
    SnapshotInterpolator::Params m_interpParams;
    // Diagnostics from the most recent RenderNetClient call, for the Net
    // debug tab (estimating/rendering happens every frame; the tab just
    // reads the last computed values rather than recomputing them itself).
    std::uint64_t m_lastEstimatedServerTick = 0;
    double m_lastRenderTick = 0.0;

    // Where the player's own hull was when the camera was last placed, i.e. in
    // the frame currently on screen. What the aim is taken from, and it has to
    // be: the cursor is turned into a world point through THAT frame's camera,
    // while the ship has since stepped on -- mixing the two makes a shot miss by
    // however far the ship moved in between, which is nothing at rest and grows
    // with speed. Because the camera follows the hull, taking both from the same
    // frame very nearly cancels instead. Empty before the first frame.
    std::optional<Magnum::Vector2d> m_aimOrigin;

    // Real Transform::pos of everything RenderNetClient temporarily moves to
    // its sub-tick rendered position, restored right after the draw.
    std::vector<std::pair<flecs::entity, Magnum::Vector2d>> m_renderPosRestore;

    // `tickFraction` is the fixed-step accumulator's leftover, 0..1 (the
    // same `delta` Render takes) -- how far past the last predicted tick
    // this frame actually is. Locally simulated renderables are drawn that
    // far between their last two predicted positions; see RenderNetClient.
    void RenderNetClient(float dtSeconds, double tickFraction);

    Magnum::Vector2 m_viewportSize{1280.f, 720.f};

    // Bottom-left corner of the scene viewport within the framebuffer, in
    // framebuffer pixels. Non-zero because the HUD sidebar claims a fixed
    // strip of the window that the world is not rendered into.
    Magnum::Vector2 m_viewportOrigin{0.f, 0.f};

    // Wall-clock dt for the camera director and hit-flash decay -- both are
    // presentation-only and driven by real time, not the fixed sim tick (see
    // Render()). Clamped there so a stall doesn't snap the camera.
    std::chrono::steady_clock::time_point m_lastCameraTime{};
    bool m_cameraTimeValid = false;
    // Same dt, accumulated: what animated colors run off (ModelRenderer2::SetTime).
    float m_renderTimeSeconds = 0.f;

protected:
    // framebuffer-pixels per design unit; needed here (not just forwarded to
    // the renderers) to size the display-independent indicator ring/arrows.
    float m_contentScale = 1.f;

    // The player's UI-size preference, multiplied into the content scale the
    // client computes. 1 = whatever the display's own scaling asks for.
    float m_uiScale = Defaults::uiScale;

    // Shared line-thickness setting (pixels), forwarded to whichever
    // renderer is active; each converts it to its own internal units.
    float m_lineWidthPixels = Defaults::lineWidth;

    // How much ModelRenderer2's line width grows with zoom: 0 = constant
    // pixel width, 1 = constant world-space width (scales linearly with zoom).
    float m_zoomWidthFactor = Defaults::zoomWidthFactor;

    Autopilot m_autopilot;

    std::unique_ptr<EntitySpawner> CreateEntitySpawner() override;
public:
    struct Defaults {
        static constexpr float lineWidth = 1.f;
        static constexpr float zoomWidthFactor = 0.5f;
        // Startup zoom, and the reference at which lineWidth is literal pixels.
        // Halved along with CameraParams' zoom range when the celestial bodies
        // doubled in size, so a planet frames the same as it used to.
        static constexpr float cameraZoom = 1.f;
        // Player-ship mass scale off the resource-authored base (Game's own
        // default is 1 = unmodified): a lighter ship reads better against the
        // solar system's gravity wells, and lifting off a planet by hand
        // needs it. Headless Games (sim-test) never apply this, so their
        // determinism is unaffected by the value.
        static constexpr float shipWeight = 0.75f;
        // Multiplier on top of the display's own scaling, not a replacement
        // for it -- see CGame::SetContentScale.
        static constexpr float uiScale = 1.f;
    };

    static constexpr float MIN_UI_SCALE = 0.5f;
    static constexpr float MAX_UI_SCALE = 4.f;

    static constexpr float MIN_LINE_WIDTH = 0.5f;
    static constexpr float MAX_LINE_WIDTH = 16.f;

    static constexpr float MIN_ZOOM_WIDTH_FACTOR = 0.f;
    static constexpr float MAX_ZOOM_WIDTH_FACTOR = 1.f;

    // `contentScale` only sizes the offscreen HUD textures, which are fixed at
    // construction (see MinimapRenderer::TextureSizeFor); everything else
    // follows SetContentScale per frame.
    explicit CGame(IFilesystem& filesystem, float contentScale = 1.f);

    void SetViewport(const Magnum::Vector2& origin, const Magnum::Vector2& size)
    {
        m_viewportOrigin = origin;
        SetViewportSize(size);
    }

    void SetViewportSize(const Magnum::Vector2& size)
    {
        m_viewportSize = size;
        m_simpleModelRenderer.SetViewportSize(size);
        m_modelRenderer2.SetViewportSize(size);
        m_mirrorRenderer2.SetViewportSize(size);
        m_starfieldRenderer.SetViewportSize(size);
        m_laserRenderer.SetViewportSize(size);
    }

    // framebuffer-pixels per design unit: the display's own scaling times the
    // UI-size preference. Everything sized to be seen rather than to fill the
    // window -- line thickness, star radius, world framing -- goes through it,
    // so a denser display renders the same view more finely.
    void SetContentScale(float scale)
    {
        m_contentScale = scale;
        m_simpleModelRenderer.SetContentScale(scale);
        m_modelRenderer2.SetContentScale(scale);
        m_mirrorRenderer2.SetContentScale(scale);
        m_starfieldRenderer.SetContentScale(scale);
        m_laserRenderer.SetContentScale(scale);
    }

    StarfieldRenderer& GetStarfieldRenderer() { return m_starfieldRenderer; }
    MinimapRenderer& GetMinimapRenderer() { return m_minimapRenderer; }
    CompassRenderer& GetCompassRenderer() { return m_compassRenderer; }
    ShipViewRenderer& GetShipViewRenderer() { return m_shipViewRenderer; }

    // A fitting position on the refit schematic, authored in the drawing's
    // '@slots' layer. `uv` is where it sits on the panel: 0..1 across the
    // drawing, y down, so the UI layer needs to know nothing about model
    // space. `categories` is what the slot accepts -- one entry for a plain
    // `engine_0`, several for a `gun+cannon_0`.
    // One buyable rank: what it costs, and why it is or isn't for sale.
    struct TechRank {
        int cost = 0;
        TechNodeState state = TechNodeState::Locked;
    };

    struct ShipSlot {
        std::string name;
        std::vector<std::string> categories;
        Magnum::Vector2 uv;
        // Which family of holes this slot addresses. A hull numbers its weapon
        // mounts and its missile bays separately, so `mount` alone does not say
        // which array it indexes -- `missile_1` is not `weapon_1`.
        SlotFamily family = SlotFamily::None;
        // Which hole of that family this is, from the label's index -- what a
        // refit pick names so the part goes into *this* one. Slots that
        // address no family carry it too; the catalog ignores it for anything
        // that isn't mounted.
        std::uint8_t mount = 0;
        // The def currently fitted here, or 0 for an empty hole. Read from the
        // loadout rather than inferred from what the ship owns: two holes can
        // hold different lines now.
        std::uint32_t fittedId = 0;
        // What each ship-tab node's ranks cost and whether they can go in
        // *this* hole, keyed by def id. Asked per hole because "already
        // held" is: a line in the nose is still for sale for a wing.
        std::vector<std::pair<std::uint32_t, std::vector<TechRank>>> rankStates;
    };

    // Empty until the schematic has loaded. Cheap enough to call per frame:
    // it walks a handful of authored markers.
    [[nodiscard]] std::vector<ShipSlot> GetShipSlots();

    // Renders the minimap into its offscreen texture. Runs its own
    // framebuffer pass, so the app calls it before the glow pass claims the
    // scene target (not from within Render()).
    void RenderMinimap();

    // Same contract as RenderMinimap: its own framebuffer pass, run before the
    // glow pass claims the scene target.
    void RenderCompass();

    // Same contract again. Unlike the other two this panel is only on screen
    // while the refit board is open, so the app calls it only then.
    void RenderShipView();

    // The solar system is laid out symmetrically around the origin (see
    // Game::BuildWorld), so that's the map's center -- static, not
    // player-centered, so flying doesn't scroll the map.
    [[nodiscard]] static Magnum::Vector2 MinimapCenter() { return {0.f, 0.f}; }

    // Parks the camera at the point the player clicked on the minimap.
    // `normalized` is -1..1 across the map in each axis, +Y up (the UI layer
    // knows the panel's pixels, this knows the map's world scale). Camera
    // follow stops until FocusCamera().
    void LookAtMapPoint(const Magnum::Vector2& normalized);

    void FocusCamera() { m_cameraDirector.FocusSubject(); }

    [[nodiscard]] bool IsCameraFollowing() const { return m_cameraDirector.IsFollowing(); }

    // The camera director owns all zoom/framing state and logic; these
    // forward to it so external callers (the client app, debug panels,
    // WorldToUi) don't need to know it exists as a separate object.
    CameraDirector& GetCameraDirector() { return m_cameraDirector; }
    Camera& GetCamera() { return m_cameraDirector.GetCamera(); }
    CameraDirector::CameraParams& GetCameraParams() { return m_cameraDirector.GetCameraParams(); }
    [[nodiscard]] float GetCameraZoom() const { return m_cameraDirector.GetCameraZoom(); }
    [[nodiscard]] bool IsManualZoomActive() const { return m_cameraDirector.IsManualZoomActive(); }

    IndicatorRenderer& GetIndicatorRenderer() { return m_indicatorRenderer; }
    IndicatorRenderer::Params& GetIndicatorParams() { return m_indicatorRenderer.GetParams(); }
    HealthBarRenderer::Params& GetHealthBarParams() { return m_healthBarRenderer.GetParams(); }

    // Mouse-wheel zoom: multiplicatively nudges a manual zoom target that
    // overrides the dynamic zoom until the player next thrusts/rotates (after
    // an initial CameraParams::manualHold grace period), then eases back.
    // `notches` is the scroll delta (positive = zoom in).
    void NudgeManualZoom(float notches) { m_cameraDirector.NudgeManualZoom(notches); }

    // Framebuffer pixels -- the size of the rect actually drawn to. World
    // extent is *not* this over zoom: the mapping is ppu = zoom * contentScale.
    // Anything asking how much world fits wants GetDesignViewportSize().
    [[nodiscard]] const Magnum::Vector2& GetViewportSize() const { return m_viewportSize; }

    [[nodiscard]] const Magnum::Vector2& GetViewportOrigin() const { return m_viewportOrigin; }

    [[nodiscard]] float GetContentScale() const { return m_contentScale; }

    // The scene viewport in design units rather than framebuffer pixels --
    // what anything reasoning about how much of the world fits on screen wants,
    // since world->design is exactly `zoom` with no scale left in it.
    [[nodiscard]] Magnum::Vector2 GetDesignViewportSize() const { return m_viewportSize / m_contentScale; }

    // World position under a point in the scene viewport, given in viewport
    // pixels with the origin at its bottom-left corner -- the same convention
    // GetViewportOrigin is expressed in. The inverse of what the renderers
    // project with.
    [[nodiscard]] Magnum::Vector2 ViewportToWorld(const Magnum::Vector2& viewportPixel);

    // Where the pilot is pointing, packed for the wire (ControlFlags::aim): the
    // world angle from the player's own hull to a place in the world.
    //
    // A PLACE, not a bearing off the cursor, is what a beam is aimed at: the
    // camera rides the hull, so a resting cursor holds a constant angle and a
    // ship flying sideways under one would drag its beam off whatever it was
    // pointed at. Re-derived from the same point every tick, the mounts swing
    // to keep it instead (see GravitarisApplication::UpdateAim, which is what
    // decides when the point stops following the cursor).
    //
    // The hull's gimbal arc is deliberately NOT folded in here -- clamping is
    // the sim's, applied identically on both sides of the wire, so what travels
    // is the request and not one client's idea of the answer. Empty when there
    // is nothing to aim from, or when the point is the hull itself -- in both
    // cases the caller should hold the angle it already had rather than snap the
    // mounts somewhere arbitrary.
    [[nodiscard]] std::optional<std::uint16_t> AimAtPoint(const Magnum::Vector2& worldPoint);


    [[nodiscard]] float GetUiScale() const { return m_uiScale; }
    void SetUiScale(float scale) { m_uiScale = std::clamp(scale, MIN_UI_SCALE, MAX_UI_SCALE); }

    // Hull integrity of the unit the HUD represents (the camera subject, so
    // it follows a spectated unit), 0..1. Empty when there's nothing to show.
    [[nodiscard]] std::optional<float> GetHullFraction();

    // Missiles that unit has on the rack. Empty when it has no rack at all
    // (nothing to show, as opposed to an empty one).
    [[nodiscard]] std::optional<int> GetMissileAmmo();

    // The cannon row: rounds left, what the magazine holds, and which lines
    // the trigger works. Matches ActiveWeapon's order.
    struct CannonReadout {
        int ammo = 0;
        int capacity = 0;
        int mode = 0;
    };
    [[nodiscard]] std::optional<CannonReadout> GetCannonReadout();

    // Speed in world units/s.
    [[nodiscard]] std::optional<float> GetSpeed();

    // Strength of the gravity field at that unit's position, in multiples of
    // one planet's surface pull (see SURFACE_GRAVITY) -- the same sum
    // PhysicsSystem::ApplyGravity builds, with the target mass divided back
    // out. Reported for any subject, so spectating a kinematic body shows the
    // field it sits in rather than the acceleration it (never) picks up from it.
    [[nodiscard]] std::optional<float> GetGravityAccel();

    // The subject's rack size, so the sidebar knows how many empty ticks to
    // draw. Zero on a hull that has fitted no missile bay, which is the same
    // thing GetMissileAmmo reports nothing for.
    [[nodiscard]] int GetMissileCapacity();

    // Shield charge and capacity of that unit, both zero when it carries no
    // emitter (which blanks the bar rather than drawing an empty one).
    struct ShieldReadout {
        float charge = 0.f;
        float capacity = 0.f;
        ShieldType type = ShieldType::None;
        // Per-plate charge, each 0..1 of that plate's own capacity, in the
        // model's plate order. Empty for a bubble (or a plated hull with no
        // '+plating' authored), which is what tells the sidebar to draw one
        // continuous bar instead of a divided one.
        std::vector<float> segments;
    };
    [[nodiscard]] ShieldReadout GetShieldReadout();

    // The charge bank, for that same unit. `fitted` is false on a ship that
    // has not collected it, which blanks the row; `fraction` is how much
    // charge is in hand and `charging` whether it is filling back up (see
    // UI::SetCapacitorReadout).
    struct CapacitorReadout {
        bool fitted = false;
        float fraction = 0.f;
        bool charging = false;
    };
    [[nodiscard]] CapacitorReadout GetCapacitorReadout();

    // What the player can spend, and whether the yard is open to them. Only
    // the own ship's: a spectated unit's purse is not the player's to spend,
    // so the camera subject is deliberately not what this follows.
    struct TechReadout {
        std::uint32_t tech = 0;      // the faction's, shared
        std::uint32_t supplies = 0;  // this pilot's own
        bool atLab = false;
    };
    [[nodiscard]] TechReadout GetTechReadout();

    // What filling this hull's magazines would cost, and whether the yard will
    // do it: `cost` 0 means nothing is missing, and `available` folds together
    // standing at a lab and being able to pay for it. Priced by the catalog, the
    // same function the server charges from.
    struct ResupplyOffer {
        int cost = 0;
        bool available = false;
    };
    [[nodiscard]] ResupplyOffer GetResupplyOffer();

    // What the port said about a purchase it would not honour, or nullopt
    // when nothing has been refused since the last call. Own ship only -- a
    // teammate being turned away at their own port is not this player's
    // business.
    //
    // Writes the same line into the chat log on its way out, so the refusal is
    // answered in the world's voice as well as in the panel, and the two can
    // never word it differently.
    [[nodiscard]] std::optional<std::string> TakeRefitDenial();

    // One node of one tree, already resolved to what the panel draws -- so
    // ui/ stays independent of the upgrade catalog, exactly as the draft
    // panel's view did.
    struct TechNode {
        id_t id = 0;
        TechTab tab = TechTab::Ship;
        // 0 weapons, 1 mobility, 2 defense. Mapped from the def's kind rather
        // than authored, since three branches over six kinds needs no data.
        int branch = 0;
        std::string icon;
        // Which slots on the schematic will take it (data/upgrades.toml `slots`).
        std::vector<std::string> slots;
        int col = 0;
        int row = 0;
        std::string name;
        std::string description;
        int rank = 0;     // what this track holds
        int maxRank = 1;
        int cap = 0;      // ship tab: the faction's unlocked ceiling
        // What this node hangs off, for the connector; 0 for a root.
        id_t requiresId = 0;
        std::vector<TechRank> ranks;
        // Rounds left and rounds it holds, for the fittings that feed from a
        // magazine -- the cannon, the launcher, and the locker that deepens one
        // of them. -1 means this fitting has no magazine to report and the panel
        // draws no count: the light guns never run out, which is their case.
        //
        // A locker reports the pool it feeds, so the same numbers appear twice
        // on a hull carrying both. Deliberate: the question the row answers is
        // "how many rounds have I got", and it has one true answer.
        int ammo = -1;
        int ammoCapacity = 0;
    };

    // Both trees, every node, in catalog order. Empty only when the pool
    // failed to load -- the tree is browsable at any time, in flight
    // included, which is the point of it being a window rather than a panel.
    [[nodiscard]] std::vector<TechNode> GetTechTree();

    // The player faction's unlock track, from whichever side of the wire this
    // build is on. Both trees are drawn against it: the permanent one shows
    // what it holds, the ship one what it permits.
    [[nodiscard]] TechUnlocks OwnUnlocks();


    // The player faction's research bar, read off its Labs (FactionState is
    // server-only; ResearchSystem mirrors the pooled state onto each Lab).
    // Empty when that side holds no lab at all -- a stalled bar and no lab to
    // advance it are different things, and the sidebar says so.
    struct ResearchReadout {
        float progress = 0.f;        // 0..1
        float secondsRemaining = 0.f;
        int labs = 0;
    };
    [[nodiscard]] std::optional<ResearchReadout> GetResearchReadout();

    // Sends a composed line. Blank (or whitespace-only) input is ignored, so
    // pressing enter twice costs nothing.
    void SubmitChat(const std::string& text);

    // The scrollback, oldest first: the newest CHAT_HISTORY_LINES.
    [[nodiscard]] std::vector<ChatLine> GetChatLog() const;

    // Bumped by every line pushed. A scrollback this deep is not worth copying
    // and diffing per frame, so the HUD rebuilds only when this moves.
    [[nodiscard]] std::uint64_t GetChatRevision() const { return m_chatRevision; }

    // Spectating: the camera (and everything framed off it) follows another
    // unit instead of your own ship. Cycles NetId order across both worlds,
    // so multiplayer sees the same roster single-player does; landing back on
    // your own ship stops spectating. Free to use for now -- watching a unit
    // that isn't yours is a cheat and belongs behind a cheat gate once one
    // exists (the player-facing version is docs/gravity-well-mode-plan.md
    // U3's unit list).
    void CycleSpectate(int direction);

    void SetSpectateTarget(flecs::entity unit) { m_spectateTarget = unit; }

    void StopSpectating() { m_spectateTarget = flecs::entity(); }

    [[nodiscard]] bool IsSpectating() const { return m_spectateTarget.is_alive(); }

    // The entity the camera is centered on: the spectated unit, or your own
    // ship (including when the spectated one just died).
    [[nodiscard]] std::optional<flecs::entity> CameraSubject();

    void SetActiveRenderer(RendererKind kind) { m_activeRenderer = kind; }
    [[nodiscard]] RendererKind GetActiveRenderer() const { return m_activeRenderer; }

    [[nodiscard]] const char* GetAudioBackendName() const { return m_audioSystem.GetBackendName(); }
    [[nodiscard]] bool IsAudioEnabled() const { return m_audioSystem.IsEnabled(); }


    [[nodiscard]] float GetLineWidth() const { return m_lineWidthPixels; }

    void SetLineWidth(float pixels)
    {
        m_lineWidthPixels = std::clamp(pixels, MIN_LINE_WIDTH, MAX_LINE_WIDTH);
    }

    void AddLineWidth(float deltaPixels) { SetLineWidth(m_lineWidthPixels + deltaPixels); }

    // --- Debug/tuning: gravity multiplier (see field comment); ship weight
    //     multiplier lives on Game itself now (see its field comment) ---

    [[nodiscard]] float GetGravityMultiplier() const { return m_physicsSystem.GetGravityMultiplier(); }
    void SetGravityMultiplier(float multiplier) { m_physicsSystem.SetGravityMultiplier(multiplier); }

    [[nodiscard]] bool GetProjectileGravity() const { return m_physicsSystem.GetProjectileGravity(); }
    void SetProjectileGravity(bool enabled) { m_physicsSystem.SetProjectileGravity(enabled); }
    [[nodiscard]] bool GetWeaponRecoil() const { return m_physicsSystem.GetWeaponRecoil(); }
    void SetWeaponRecoil(bool enabled) { m_physicsSystem.SetWeaponRecoil(enabled); }
    [[nodiscard]] PhysicsSystem::ShipContactParams& GetShipContactParams()
    {
        return m_physicsSystem.GetShipContactParams();
    }

    [[nodiscard]] float GetZoomWidthFactor() const { return m_zoomWidthFactor; }

    void SetZoomWidthFactor(float factor)
    {
        m_zoomWidthFactor = std::clamp(factor, MIN_ZOOM_WIDTH_FACTOR, MAX_ZOOM_WIDTH_FACTOR);
    }

    void ToggleDebugForceFacetedCircles()
    {
        m_modelRenderer2.SetDebugForceFacetedCircles(!m_modelRenderer2.GetDebugForceFacetedCircles());
    }

    // The autopilot is a client-side command producer (same seam as
    // keyboard input); these forward to it so external callers don't need
    // to know it's a separate object.
    Autopilot& GetAutopilot() { return m_autopilot; }
    [[nodiscard]] AutopilotMode GetAutopilotMode() const { return m_autopilot.GetMode(); }

    // Engaging HoldPosition captures the player's current position as anchor.
    void SetAutopilotMode(AutopilotMode mode) { m_autopilot.SetMode(mode, GetPlayer()); }

    void ToggleAutopilotMode(AutopilotMode mode) { m_autopilot.ToggleMode(mode, GetPlayer()); }

    [[nodiscard]] const Magnum::Math::Vector2<double>& GetAutopilotAnchor() const { return m_autopilot.GetAnchor(); }

    FlightControllerParams& GetFlightParams() { return m_autopilot.GetFlightParams(); }

    GuidanceParams& GetGuidanceParams() { return m_autopilot.GetGuidanceParams(); }

    [[nodiscard]] const Magnum::Math::Vector2<double>& GetGotoTarget() const { return m_autopilot.GetGotoTarget(); }

    void SetGotoTarget(const Magnum::Math::Vector2<double>& target) { m_autopilot.SetGotoTarget(target); }

    [[nodiscard]] const Magnum::Math::Vector2<double>& GetOrbitCenter() const { return m_autopilot.GetOrbitCenter(); }

    [[nodiscard]] double GetOrbitRadius() const { return m_autopilot.GetOrbitRadius(); }

    // This tick's autopilot command, or nullopt when off / no player. Fire
    // bits are false; the caller merges keyboard fire.
    std::optional<ControlFlags> ComputeAutopilotControls() { return m_autopilot.ComputeControls(GetPlayer()); }

    void Render(double delta);

    // Switches this CGame into multiplayer-client mode: connects over the
    // WebSocket signaling path to a gravitaris-server at wsUrl (e.g.
    // "ws://host:port"). Call instead of Game::Start() -- the local sim
    // never runs in this mode (see IsNetClient()), so starting it first
    // would spawn an uncontrolled, unreplicated local player ship.
    // `requestedTeam` rides along in ClientHello, which is built the instant
    // the transport reports Connected -- so the side has to be known by the
    // time this is called, not set afterwards. TeamId::None asks the server to
    // auto-assign.
    void ConnectToServer(const std::string& wsUrl, TeamId requestedTeam = TeamId::None);

    // True once ConnectToServer has been called. While true, the caller
    // (GravitarisApplication) must not call Game::Update()/CGame's normal
    // FeedInput path -- there is no local sim to feed. Render() handles the
    // net-client path itself either way.
    [[nodiscard]] bool IsNetClient() const { return m_netClient != nullptr; }

    // One fixed PHYSICS_DELTA tick of multiplayer-client-side work: spawns
    // the locally-predicted own ship the first time a snapshot confirms
    // where it should appear (nothing to do before that -- no-op), then
    // predicts one more tick of its own movement and sends `flags` to the
    // server. Call from the same fixed-step accumulator loop single-player
    // drives Game::Update() from.
    //
    // False when no tick was actually sent -- before the welcome, between a
    // death and the snapshot that confirms the next hull, or when the clock is
    // running ahead of the server. `techPick` went nowhere in that case, so a
    // caller holding a queue of them must not drop the one it offered.
    bool TickNetClient(const ControlFlags& flags, const TechPick& techPick = {});

    // Net debug tab (Phase 4 interpolation tunables + diagnostics).
    [[nodiscard]] float GetInterpDelaySeconds() const { return m_interpDelaySeconds; }
    void SetInterpDelaySeconds(float seconds) { m_interpDelaySeconds = std::max(seconds, 0.f); }
    [[nodiscard]] SnapshotInterpolator::Params& GetInterpParams() { return m_interpParams; }
    [[nodiscard]] std::size_t GetSnapshotHistorySize() const
    {
        return m_netClient ? m_netClient->GetSnapshotHistory().size() : 0;
    }
    [[nodiscard]] std::uint64_t GetLastEstimatedServerTick() const { return m_lastEstimatedServerTick; }
    [[nodiscard]] double GetLastRenderTick() const { return m_lastRenderTick; }

    // Ticks between what this client is aiming AT and the tick it is aiming
    // FROM: everyone else is drawn at the interpolated render tick, while the
    // own ship is predicted at `commandTick`. Sent with every command so the
    // server can put the world back (LagCompensation); clamped there too, since
    // a peer's number is a peer's number.
    [[nodiscard]] std::uint16_t ViewDelayTicks(std::uint64_t commandTick) const;
    [[nodiscard]] std::size_t GetClockSnapCount() const
    {
        return m_netClient ? m_netClient->GetClockSnapCount() : 0;
    }
    [[nodiscard]] float GetPingJitterMs() const { return m_netClient ? m_netClient->GetPingJitterMs() : -1.f; }
    [[nodiscard]] std::uint64_t GetSuggestedInputLeadTicks() const
    {
        return m_netClient ? m_netClient->GetSuggestedInputLeadTicks() : 0;
    }
    [[nodiscard]] bool IsInputLeadAuto() const { return m_netClient && m_netClient->IsInputLeadAuto(); }
    void SetInputLeadAuto(bool on)
    {
        if (m_netClient) m_netClient->SetInputLeadAuto(on);
    }

    // Net debug tab (Phase 5 prediction/reconciliation tunable).
    [[nodiscard]] double GetPredictionEpsilon() const { return m_clientPrediction.GetPositionEpsilon(); }
    void SetPredictionEpsilon(double epsilon) { m_clientPrediction.SetPositionEpsilon(epsilon); }

    // Net debug tab: how far ahead of the estimated server tick this
    // client's own input is stamped (NetClient::GetInputLeadTicks's own doc
    // comment). Matters most on the no-client-prediction branch, where every
    // tick of it is felt directly as input lag.
    [[nodiscard]] std::uint64_t GetInputLeadTicks() const
    {
        return m_netClient ? m_netClient->GetInputLeadTicks() : NetClient::INPUT_LEAD_TICKS;
    }
    void SetInputLeadTicks(std::uint64_t ticks)
    {
        if (m_netClient) m_netClient->SetInputLeadTicks(ticks);
    }

    // Net debug tab: connection-health diagnostics, for telling a real
    // network gap (snapshot interval spikes) apart from a local main-thread
    // stall (drift/resync fires with snapshot interval unaffected).
    [[nodiscard]] const NetDiagnostics& GetNetDiagnostics() const { return m_netDiagnostics; }
    [[nodiscard]] float GetLastSnapshotIntervalMs() const
    {
        return m_netClient ? m_netClient->GetLastSnapshotIntervalMs() : 0.f;
    }
    [[nodiscard]] std::size_t GetAcceptedSnapshotCount() const
    {
        return m_netClient ? m_netClient->GetAcceptedSnapshotCount() : 0;
    }
    [[nodiscard]] std::size_t GetDroppedSnapshotCount() const
    {
        return m_netClient ? m_netClient->GetDroppedSnapshotCount() : 0;
    }

    // Real measured RTT (NetClient's Ping/Pong probe), not an estimate. -1
    // before the first Pong arrives.
    [[nodiscard]] float GetLastPingMs() const { return m_netClient ? m_netClient->GetLastPingMs() : -1.f; }
    [[nodiscard]] float GetAveragePingMs() const { return m_netClient ? m_netClient->GetAveragePingMs() : -1.f; }

    // The seed the served sector was generated from; 0 before the welcome
    // lands, and always 0 in single-player, where the client owns the seed
    // and never has to ask for it.
    [[nodiscard]] std::uint32_t GetServerSectorSeed() const
    { return m_netClient ? m_netClient->GetSectorSeed() : 0u; }

    // Net debug tab: live artificial delay/jitter/loss (SimulatedNetTransport,
    // sits below NetClient -- see m_simulatedTransport's own field comment).
    // nullptr before ConnectToServer runs; caller must check IsNetClient()
    // first, same convention as every other net accessor here.
    [[nodiscard]] SimulatedNetTransport::Params* GetSimulatedNetParams()
    {
        return m_simulatedTransport ? &m_simulatedTransport->GetParams() : nullptr;
    }
};

} // namespace Gravitaris
