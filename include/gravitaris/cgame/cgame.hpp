#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/game.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/gnc/control/flight-controller.hpp>
#include <gravitaris/game/gnc/guidance/behaviors.hpp>

#include <gravitaris/cgame/camera.hpp>
#include <gravitaris/cgame/renderer/simple-model-renderer.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>
#include <gravitaris/cgame/renderer/starfield-renderer.hpp>
#include <gravitaris/cgame/renderer/minimap-renderer.hpp>
#include <gravitaris/cgame/audio/audio-system.hpp>

namespace Gravitaris {

// Which line renderer draws the scene. Mutually exclusive; switchable at
// runtime from the debug UI for A/B comparison.
enum class RendererKind {
    Simple, // SimpleModelRenderer  — GL LineStrip, no thickness control
    Baked,  // ModelRenderer2       — baked/instanced, pixel-space width
};

// Player pilot assist; produces commands like a human would, the sim only
// sees the InputQueue.
enum class AutopilotMode {
    Off,
    KillVelocity, // retro-burn toward zero velocity
    HoldPosition, // hover at the position where engaged
    GotoPoint,    // fly to the goto target and stop
    Orbit,        // circle the heaviest gravity source at the engage radius
};

class CGame : public Game {
protected:
    SimpleModelRenderer m_simpleModelRenderer;
    ModelRenderer2 m_modelRenderer2;
    StarfieldRenderer m_starfieldRenderer;
    MinimapRenderer m_minimapRenderer;
    AudioSystem m_audioSystem;

    RendererKind m_activeRenderer = RendererKind::Baked;

    Camera m_camera;
    Magnum::Vector2 m_viewportSize{1280.f, 720.f};

    bool m_cameraFollow = true;
    // Dead-zone half-size as a fraction of the visible half-extent: the ship
    // roams the central (2*fraction) of the view before the camera follows.
    // Shrinks toward FRAMING while an enemy is being framed so the view tracks
    // the pair instead of letting it drift in the dead zone.
    static constexpr float DEAD_ZONE_FRACTION = 0.35f;
    static constexpr float DEAD_ZONE_FRACTION_FRAMING = 0.08f;

    // Camera director state (see UpdateCamera). m_cameraZoom is the smoothed
    // zoom actually applied to the camera each frame. The wheel writes
    // m_manualZoom and sets m_manualZoomActive, which locks the zoom there
    // regardless of speed/framing. It stays locked until the player actively
    // controls the ship (thrust/rotate) -- m_manualZoomGraceRemaining is a
    // grace period (CameraParams::manualHold) after a wheel nudge during
    // which control input doesn't cancel it, so scrolling while already
    // moving doesn't instantly undo the pick. Once cancelled, m_cameraZoom
    // eases back to the dynamic target via the usual tau-smoothing below.
    float m_cameraZoom = Defaults::cameraZoom;
    float m_manualZoom = Defaults::cameraZoom;
    bool m_manualZoomActive = false;
    float m_manualZoomGraceRemaining = 0.f;
    std::chrono::steady_clock::time_point m_lastCameraTime{};
    bool m_cameraTimeValid = false;

    // Eased 0..1: how "framed" the view currently is. FollowWithDeadZone moves
    // the camera instantly to keep its target inside the dead zone, so feeding
    // it a target/dead-zone-size that itself jumps (enemy appears/leaves
    // radius) would snap the camera in one frame. Easing this amount in/out
    // instead means both the framing bias and the dead-zone shrink ramp in
    // smoothly, and FollowWithDeadZone's per-frame correction stays small.
    float m_framingAmount = 0.f;
    // Which enemy the camera is framing. Sticky: with several enemies in
    // range the raw "nearest" flips identity constantly as ships orbit, so
    // the framed target only switches when a rival is decisively closer
    // (FRAMING_SWITCH_FACTOR) -- pure nearest-wins would snap the pan target
    // between ships every few frames.
    flecs::entity m_framedEnemy{};
    // Eased enemy-relative offset (world units) used for the pan bias.
    // Smoothed toward the framed enemy's true offset so a target switch that
    // does happen glides instead of stepping; kept after the enemy leaves
    // range so the bias fades back out along the direction it faded in from.
    Magnum::Vector2 m_framedEnemyOffset{0.f, 0.f};
    // Eased distance (world units) from the player to the farthest in-range
    // enemy, driving the zoom-fit so *all* nearby enemies fit in view (not
    // just the framed one). Separate from the offset: pan follows one sticky
    // target, zoom must contain the whole group. Smoothed so a far enemy
    // entering/leaving range doesn't snap the zoom.
    float m_framedReach = 0.f;

    // A rival enemy must be closer than (this * current target's distance)
    // to steal the framing. Exit hysteresis: the current target is kept
    // until it exceeds enemyRadius by 15%, so a ship hovering right at the
    // radius doesn't strobe the framing on/off.
    static constexpr float FRAMING_SWITCH_FACTOR = 0.7f;
    static constexpr float FRAMING_EXIT_RADIUS_FACTOR = 1.15f;

public:
    // Tunables for the camera director (exposed in the Camera debug tab).
    struct CameraParams {
        bool dynamicZoom = true;
        float minZoom = 0.5f;       // most zoomed-out (fast / framing) end
        float maxZoom = 5.f;        // most zoomed-in (at rest) end
        float speedFalloff = 220.f; // world units/sec at which zoom noticeably backs off
        float zoomTau = 2.f;        // zoom smoothing time constant (s), for the dynamic (speed/framing) target
        float manualZoomTau = 0.25f;// zoom smoothing time constant (s), for a wheel-driven target -- snappier
                                    // than zoomTau so a manual scroll reads immediately, without touching
                                    // the dynamic-zoom feel (enemy framing, speed falloff).
        float manualHold = 5.f;     // grace period (s) after a wheel nudge before ship control can cancel it
        float scrollSensitivity = 1.08f; // zoom multiplier per wheel notch (was 1.15 -- felt oversensitive)

        bool enemyFraming = true;
        float enemyRadius = 1100.f; // consider enemies within this for framing
        float framingBias = 0.35f;  // 0 = stay on player, 1 = midpoint between player and enemy
        float framingMargin = 220.f;// extra world units kept around the framed pair
        float framingTau = 1.2f;    // time constant for easing framing in/out as an enemy appears/leaves
    };

    // Tunables for the off-screen target arrows (exposed in the HUD debug tab).
    // Distances are world units; sizes are logical pixels (scaled by the HiDPI
    // pixel scale like line width is).
    struct IndicatorParams {
        bool enabled = true;
        float ringRadiusPx = 120.f;  // arrow ring radius around screen center
        float arrowSizePx = 13.f;    // arrow width, and height at long range (see maxHeightFactor)
        float enemyRange = 2500.f;   // show enemies within this
        float edgeMarginPx = 24.f;   // treat as off-screen this far inside the view edge
        float fadeBandPx = 90.f;     // px past the edge over which an arrow fades fully in
        float minStrength = 0.35f;   // brightness floor at max range (never fully invisible while in range)
        // Height-only stretch at point-blank range (width never changes with
        // distance, only with the edge-appear fade) -- 1 = no stretch, taller
        // as the target closes in. Intentionally allowed to look "squished".
        float maxHeightFactor = 2.5f;
        // Multiplies the proximity fed into the height stretch, so it reaches
        // maxHeightFactor within a 1/heightRampFactor fraction of the range --
        // arrows stay flat over most of the range and only stretch tall right
        // at the end, instead of ramping linearly across the whole range.
        float heightRampFactor = 4.f;
        int maxEnemies = 8;
    };

protected:
    CameraParams m_cameraParams;
    IndicatorParams m_indicatorParams;

    // Kept alive so the arrow glyph stays baked in ModelRenderer2 (loading a
    // Model is what fires the renderer's OnCreate<Model>); overlays aren't
    // entities, so nothing else holds a reference to it.
    ResourcePtr<const Model> m_arrowModel;

    // framebuffer-pixels per logical-pixel; needed here (not just forwarded to
    // the renderers) to size the HiDPI-independent indicator ring/arrows.
    float m_pixelScale = 1.f;

    // Shared line-thickness setting (pixels), forwarded to whichever
    // renderer is active; each converts it to its own internal units.
    float m_lineWidthPixels = Defaults::lineWidth;

    // How much ModelRenderer2's line width grows with zoom: 0 = constant
    // pixel width, 1 = constant world-space width (scales linearly with zoom).
    float m_zoomWidthFactor = Defaults::zoomWidthFactor;

    // Deterministic per-(tick, spawn) seed for SpawnRandomAIShip's preset pick
    // (ADR 0001: no std::rand -- it mutates sim state, so it must be
    // reproducible under replay). Incremented per call so repeated presses
    // within one tick still diverge.
    std::uint32_t m_randomAIShipSpawnCount = 0;

    // Event cursor for the hit-flash consumer (see UpdateHitFlashes); audio
    // keeps its own inside AudioSystem -- each GameEventQueue consumer tracks
    // its own position independently.
    std::uint32_t m_flashEventCursor = 0;

    // Debug/tuning only (temporary, for calibrating gameplay feel -- see the
    // Physics debug tab). 1 = unmodified in both cases; 0.667 is this game's
    // tuned default (a lighter ship reads better against the solar system's
    // gravity wells).
    // Gravity is a PhysicsSystem-wide setting, applied every ApplyGravity
    // call; ship weight scales the player's live Chipmunk mass off its
    // resource-authored base (PhysicsSystem::SetMassMultiplier), reapplied
    // every Render() call so it survives a respawn's fresh body without
    // extra bookkeeping.
    float m_shipWeightMultiplier = 0.667f;

    AutopilotMode m_autopilotMode = AutopilotMode::Off;
    Magnum::Math::Vector2<double> m_autopilotAnchor;
    FlightControllerParams m_flightParams;

    GuidanceParams m_guidanceParams;
    Magnum::Math::Vector2<double> m_gotoTarget;
    Magnum::Math::Vector2<double> m_orbitCenter;
    double m_orbitMass = 0.0;
    double m_orbitRadius = 0.0;
    double m_orbitDirection = 1.0;

    struct GravitySource {
        Magnum::Math::Vector2<double> pos;
        double mass;
    };
    std::optional<GravitySource> FindHeaviestGravitySource();

    // Updates m_framedEnemy (sticky nearest hostile, see field comment) and
    // returns the framed enemy's current position, or nullopt when nothing is
    // in range. `outCoverDist` receives the distance to the farthest in-range
    // enemy (0 if none), for the group zoom-fit. Will also feed the planned
    // enemy/planet arrow indicators.
    std::optional<Magnum::Vector2> SelectFramedEnemy(const Magnum::Vector2& from, TeamId playerTeam,
                                                     float& outCoverDist);

    // Per-frame camera director: eases position (with enemy framing) and zoom
    // (speed-driven, enemy-fit, or manual override) toward their targets.
    void UpdateCamera(float dtSeconds);

    // Submits an arrow overlay per nearby-but-off-screen enemy/planet, on a
    // ring around screen center, pointing at the target. Call after UpdateCamera
    // (needs the final camera pos/zoom) and before the renderer draws.
    void UpdateIndicators();

    // Client-side hit-flash: sets HitFlash.amount = 1 on entities named by
    // Impact/LandingCrash events (resolved via the NetId registry), then
    // decays every entity's flash with the rendered frame's dt.
    void UpdateHitFlashes(float dtSeconds);

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
        m_starfieldRenderer.SetViewportSize(size);
    }

    // framebuffer-pixels per logical-pixel; keeps line thickness constant in
    // logical units across HiDPI/Retina displays.
    void SetPixelScale(float scale)
    {
        m_pixelScale = scale;
        m_modelRenderer2.SetPixelScale(scale);
        m_starfieldRenderer.SetPixelScale(scale);
    }

    StarfieldRenderer& GetStarfieldRenderer() { return m_starfieldRenderer; }
    MinimapRenderer& GetMinimapRenderer() { return m_minimapRenderer; }

    // Renders the minimap into its offscreen texture. Runs its own
    // framebuffer pass, so the app calls it before the glow pass claims the
    // scene target (not from within Render()).
    void RenderMinimap();

    Camera& GetCamera() { return m_camera; }

    CameraParams& GetCameraParams() { return m_cameraParams; }
    IndicatorParams& GetIndicatorParams() { return m_indicatorParams; }
    [[nodiscard]] float GetCameraZoom() const { return m_cameraZoom; }
    [[nodiscard]] bool IsManualZoomActive() const { return m_manualZoomActive; }

    // Mouse-wheel zoom: multiplicatively nudges a manual zoom target that
    // overrides the dynamic zoom until the player next thrusts/rotates (after
    // an initial CameraParams::manualHold grace period), then eases back.
    // `notches` is the scroll delta (positive = zoom in).
    void NudgeManualZoom(float notches);

    // Framebuffer pixels; world->screen mapping is ppu = zoom (renderers use
    // 1 px/unit at zoom 1), camera-centered.
    [[nodiscard]] const Magnum::Vector2& GetViewportSize() const { return m_viewportSize; }

    void ToggleCameraFollow() { m_cameraFollow = !m_cameraFollow; }

    void SetActiveRenderer(RendererKind kind) { m_activeRenderer = kind; }
    [[nodiscard]] RendererKind GetActiveRenderer() const { return m_activeRenderer; }

    void SetAudioBackendPreference(AudioBackendPreference preference)
    { m_audioSystem.SetBackendPreference(preference); }
    [[nodiscard]] AudioBackendPreference GetAudioBackendPreference() const
    { return m_audioSystem.GetBackendPreference(); }
    [[nodiscard]] const char* GetAudioBackendName() const { return m_audioSystem.GetBackendName(); }
    [[nodiscard]] bool IsAudioEnabled() const { return m_audioSystem.IsEnabled(); }

    [[nodiscard]] float GetLineWidth() const { return m_lineWidthPixels; }

    void SetLineWidth(float pixels)
    {
        m_lineWidthPixels = std::clamp(pixels, MIN_LINE_WIDTH, MAX_LINE_WIDTH);
    }

    void AddLineWidth(float deltaPixels) { SetLineWidth(m_lineWidthPixels + deltaPixels); }

    // --- Debug/tuning: gravity + ship weight multipliers (see field comments) ---

    [[nodiscard]] float GetGravityMultiplier() const { return m_physicsSystem.GetGravityMultiplier(); }
    void SetGravityMultiplier(float multiplier) { m_physicsSystem.SetGravityMultiplier(multiplier); }

    [[nodiscard]] float GetShipWeightMultiplier() const { return m_shipWeightMultiplier; }
    void SetShipWeightMultiplier(float multiplier) { m_shipWeightMultiplier = multiplier; }

    [[nodiscard]] float GetZoomWidthFactor() const { return m_zoomWidthFactor; }

    void SetZoomWidthFactor(float factor)
    {
        m_zoomWidthFactor = std::clamp(factor, MIN_ZOOM_WIDTH_FACTOR, MAX_ZOOM_WIDTH_FACTOR);
    }

    void ToggleDebugForceFacetedCircles()
    {
        m_modelRenderer2.SetDebugForceFacetedCircles(!m_modelRenderer2.GetDebugForceFacetedCircles());
    }

    // Spawns an AI fighter near the player with a random personality preset;
    // shared by the Spawn debug tab's button and the J shortcut.
    void SpawnRandomAIShip();

    [[nodiscard]] AutopilotMode GetAutopilotMode() const { return m_autopilotMode; }

    // Engaging HoldPosition captures the player's current position as anchor.
    void SetAutopilotMode(AutopilotMode mode);

    void ToggleAutopilotMode(AutopilotMode mode)
    {
        SetAutopilotMode(m_autopilotMode == mode ? AutopilotMode::Off : mode);
    }

    [[nodiscard]] const Magnum::Math::Vector2<double>& GetAutopilotAnchor() const { return m_autopilotAnchor; }

    FlightControllerParams& GetFlightParams() { return m_flightParams; }

    GuidanceParams& GetGuidanceParams() { return m_guidanceParams; }

    [[nodiscard]] const Magnum::Math::Vector2<double>& GetGotoTarget() const { return m_gotoTarget; }

    void SetGotoTarget(const Magnum::Math::Vector2<double>& target) { m_gotoTarget = target; }

    [[nodiscard]] const Magnum::Math::Vector2<double>& GetOrbitCenter() const { return m_orbitCenter; }

    [[nodiscard]] double GetOrbitRadius() const { return m_orbitRadius; }

    // This tick's autopilot command, or nullopt when off / no player. Fire
    // bits are false; the caller merges keyboard fire.
    std::optional<ControlFlags> ComputeAutopilotControls();

    void Render(double delta);
};

} // namespace Gravitaris
