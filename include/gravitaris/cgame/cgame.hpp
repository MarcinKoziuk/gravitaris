#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

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
#include <gravitaris/cgame/net/cosmetic-bullet-despawner.hpp>
#include <gravitaris/cgame/net/net-diagnostics.hpp>
#include <gravitaris/cgame/net/remote-event-applier.hpp>
#include <gravitaris/cgame/net/snapshot-applier.hpp>
#include <gravitaris/cgame/net/snapshot-interpolator.hpp>
#include <gravitaris/cgame/camera-director.hpp>
#include <gravitaris/game/gnc/autopilot.hpp>
#include <gravitaris/cgame/renderer/simple-model-renderer.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>
#include <gravitaris/cgame/renderer/starfield-renderer.hpp>
#include <gravitaris/cgame/renderer/minimap-renderer.hpp>
#include <gravitaris/cgame/audio/audio-system.hpp>
#include <gravitaris/cgame/fx/hit-flash-system.hpp>
#include <gravitaris/cgame/hud/indicator-renderer.hpp>

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

    StarfieldRenderer m_starfieldRenderer;
    MinimapRenderer m_minimapRenderer;
    AudioSystem m_audioSystem;
    HitFlashSystem m_hitFlashSystem;
    CameraDirector m_cameraDirector;
    IndicatorRenderer m_indicatorRenderer;

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
    // Enemy/planet camera framing and the minimap both sweep m_mirrorWorld
    // alongside m_registry for this (CameraDirector::Update/
    // MinimapRenderer::Render's remoteWorld parameter), since every entity
    // but the own ship lives there, not in m_registry, in this mode.
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

    // Draws a filled team-colored square at the center of every owned planet
    // (Team != None) in `world`, via `renderer`'s overlay path -- immediate
    // conquest feedback, matching the original's claimed-planet marker (see
    // docs/gwell/screenshots). Single-player sweeps m_registry via
    // m_modelRenderer2; net-client sweeps m_mirrorWorld via m_mirrorRenderer2
    // (planets/ownership live in whichever world the mode simulates them in).
    ResourcePtr<const Model> m_teamMarkerModel;
    void SubmitPlanetOwnershipMarkers(flecs::world& world, ModelRenderer2& renderer);

    // Constructed in ConnectToServer once m_netClient exists (RemoteEventApplier
    // needs a live NetClient&) -- always populated by the time ApplyRemoteEvents
    // runs, since that's only ever called from RenderNetClient, itself gated
    // on m_netClient being set (see Render()).
    std::optional<RemoteEventApplier> m_remoteEventApplier;
    void ApplyRemoteEvents();

    // Phase 4 tunables (Net debug tab): how far behind the estimated server
    // tick remote entities render (smooths jitter, at the cost of latency)
    // and how far past the newest received snapshot extrapolation is
    // allowed to guess before snapping to it instead.
    float m_interpDelaySeconds = 0.1f;
    SnapshotInterpolator::Params m_interpParams;
    // Diagnostics from the most recent RenderNetClient call, for the Net
    // debug tab (estimating/rendering happens every frame; the tab just
    // reads the last computed values rather than recomputing them itself).
    std::uint64_t m_lastEstimatedServerTick = 0;
    std::uint64_t m_lastRenderTick = 0;

    void RenderNetClient(float dtSeconds);

    Magnum::Vector2 m_viewportSize{1280.f, 720.f};

    // Wall-clock dt for the camera director and hit-flash decay -- both are
    // presentation-only and driven by real time, not the fixed sim tick (see
    // Render()). Clamped there so a stall doesn't snap the camera.
    std::chrono::steady_clock::time_point m_lastCameraTime{};
    bool m_cameraTimeValid = false;

protected:
    // framebuffer-pixels per logical-pixel; needed here (not just forwarded to
    // the renderers) to size the HiDPI-independent indicator ring/arrows.
    float m_pixelScale = 1.f;

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
        static constexpr float cameraZoom = 2.f;
    };

    static constexpr float MIN_LINE_WIDTH = 0.5f;
    static constexpr float MAX_LINE_WIDTH = 16.f;

    static constexpr float MIN_ZOOM_WIDTH_FACTOR = 0.f;
    static constexpr float MAX_ZOOM_WIDTH_FACTOR = 1.f;

    explicit CGame(IFilesystem& filesystem);

    void SetViewportSize(const Magnum::Vector2& size)
    {
        m_viewportSize = size;
        m_simpleModelRenderer.SetViewportSize(size);
        m_modelRenderer2.SetViewportSize(size);
        m_mirrorRenderer2.SetViewportSize(size);
        m_starfieldRenderer.SetViewportSize(size);
    }

    // framebuffer-pixels per logical-pixel; keeps line thickness constant in
    // logical units across HiDPI/Retina displays.
    void SetPixelScale(float scale)
    {
        m_pixelScale = scale;
        m_modelRenderer2.SetPixelScale(scale);
        m_mirrorRenderer2.SetPixelScale(scale);
        m_starfieldRenderer.SetPixelScale(scale);
    }

    StarfieldRenderer& GetStarfieldRenderer() { return m_starfieldRenderer; }
    MinimapRenderer& GetMinimapRenderer() { return m_minimapRenderer; }

    // Renders the minimap into its offscreen texture. Runs its own
    // framebuffer pass, so the app calls it before the glow pass claims the
    // scene target (not from within Render()).
    void RenderMinimap();

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

    // Mouse-wheel zoom: multiplicatively nudges a manual zoom target that
    // overrides the dynamic zoom until the player next thrusts/rotates (after
    // an initial CameraParams::manualHold grace period), then eases back.
    // `notches` is the scroll delta (positive = zoom in).
    void NudgeManualZoom(float notches) { m_cameraDirector.NudgeManualZoom(notches); }

    // Framebuffer pixels; world->screen mapping is ppu = zoom (renderers use
    // 1 px/unit at zoom 1), camera-centered.
    [[nodiscard]] const Magnum::Vector2& GetViewportSize() const { return m_viewportSize; }

    void ToggleCameraFollow() { m_cameraDirector.ToggleCameraFollow(); }

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
    void ConnectToServer(const std::string& wsUrl);

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
    void TickNetClient(const ControlFlags& flags);

    // Net debug tab (Phase 4 interpolation tunables + diagnostics).
    [[nodiscard]] float GetInterpDelaySeconds() const { return m_interpDelaySeconds; }
    void SetInterpDelaySeconds(float seconds) { m_interpDelaySeconds = std::max(seconds, 0.f); }
    [[nodiscard]] SnapshotInterpolator::Params& GetInterpParams() { return m_interpParams; }
    [[nodiscard]] std::size_t GetSnapshotHistorySize() const
    {
        return m_netClient ? m_netClient->GetSnapshotHistory().size() : 0;
    }
    [[nodiscard]] std::uint64_t GetLastEstimatedServerTick() const { return m_lastEstimatedServerTick; }
    [[nodiscard]] std::uint64_t GetLastRenderTick() const { return m_lastRenderTick; }

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
