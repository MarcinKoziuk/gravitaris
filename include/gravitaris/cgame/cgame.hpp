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
#include <gravitaris/cgame/camera-director.hpp>
#include <gravitaris/cgame/renderer/simple-model-renderer.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>
#include <gravitaris/cgame/renderer/starfield-renderer.hpp>
#include <gravitaris/cgame/renderer/minimap-renderer.hpp>
#include <gravitaris/cgame/audio/audio-system.hpp>
#include <gravitaris/cgame/fx/hit-flash-system.hpp>

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
    HitFlashSystem m_hitFlashSystem;
    CameraDirector m_cameraDirector;

    RendererKind m_activeRenderer = RendererKind::Baked;

    Magnum::Vector2 m_viewportSize{1280.f, 720.f};

    // Wall-clock dt for the camera director and hit-flash decay -- both are
    // presentation-only and driven by real time, not the fixed sim tick (see
    // Render()). Clamped there so a stall doesn't snap the camera.
    std::chrono::steady_clock::time_point m_lastCameraTime{};
    bool m_cameraTimeValid = false;

public:
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

    // Submits an arrow overlay per nearby-but-off-screen enemy/planet, on a
    // ring around screen center, pointing at the target. Call after the
    // camera director's Update (needs the final camera pos/zoom) and before
    // the renderer draws.
    void UpdateIndicators();

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

    // The camera director owns all zoom/framing state and logic; these
    // forward to it so external callers (the client app, debug panels,
    // WorldToUi) don't need to know it exists as a separate object.
    CameraDirector& GetCameraDirector() { return m_cameraDirector; }
    Camera& GetCamera() { return m_cameraDirector.GetCamera(); }
    CameraDirector::CameraParams& GetCameraParams() { return m_cameraDirector.GetCameraParams(); }
    [[nodiscard]] float GetCameraZoom() const { return m_cameraDirector.GetCameraZoom(); }
    [[nodiscard]] bool IsManualZoomActive() const { return m_cameraDirector.IsManualZoomActive(); }

    IndicatorParams& GetIndicatorParams() { return m_indicatorParams; }

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
