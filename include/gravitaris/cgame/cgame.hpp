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
    // Last enemy-relative offset (world units); kept after the enemy leaves
    // range so the bias fades back out along the direction it faded in from,
    // rather than snapping to zero the instant no enemy is found.
    Magnum::Vector2 m_lastEnemyOffset{0.f, 0.f};

public:
    // Tunables for the camera director (exposed in the Camera debug tab).
    struct CameraParams {
        bool dynamicZoom = true;
        float minZoom = 0.5f;       // most zoomed-out (fast / framing) end
        float maxZoom = 5.f;        // most zoomed-in (at rest) end
        float speedFalloff = 220.f; // world units/sec at which zoom noticeably backs off
        float zoomTau = 2.f;        // zoom smoothing time constant (s)
        float manualHold = 5.f;     // grace period (s) after a wheel nudge before ship control can cancel it

        bool enemyFraming = true;
        float enemyRadius = 1100.f; // consider enemies within this for framing
        float framingBias = 0.35f;  // 0 = stay on player, 1 = midpoint between player and enemy
        float framingMargin = 220.f;// extra world units kept around the framed pair
        float framingTau = 1.2f;    // time constant for easing framing in/out as an enemy appears/leaves
    };

protected:
    CameraParams m_cameraParams;

    // Shared line-thickness setting (pixels), forwarded to whichever
    // renderer is active; each converts it to its own internal units.
    float m_lineWidthPixels = Defaults::lineWidth;

    // How much ModelRenderer2's line width grows with zoom: 0 = constant
    // pixel width, 1 = constant world-space width (scales linearly with zoom).
    float m_zoomWidthFactor = Defaults::zoomWidthFactor;

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

    // Nearest hostile ship (real enemy team, damageable) within
    // m_cameraParams.enemyRadius of `from`, or nullopt. Used for camera
    // framing; will also feed the planned enemy/planet arrow indicators.
    std::optional<Magnum::Vector2> FindNearestEnemy(const Magnum::Vector2& from, TeamId playerTeam);

    // Per-frame camera director: eases position (with enemy framing) and zoom
    // (speed-driven, enemy-fit, or manual override) toward their targets.
    void UpdateCamera(float dtSeconds);

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
        m_modelRenderer2.SetPixelScale(scale);
        m_starfieldRenderer.SetPixelScale(scale);
    }

    StarfieldRenderer& GetStarfieldRenderer() { return m_starfieldRenderer; }

    Camera& GetCamera() { return m_camera; }

    CameraParams& GetCameraParams() { return m_cameraParams; }
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
