#pragma once

#include <algorithm>
#include <optional>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/game.hpp>
#include <gravitaris/game/control/flight-controller.hpp>

#include <gravitaris/cgame/camera.hpp>
#include <gravitaris/cgame/renderer/simple-model-renderer.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>

namespace Gravitaris {

// Which line renderer draws the scene. Mutually exclusive; switchable at
// runtime from the debug UI for A/B comparison.
enum class RendererKind {
    Simple, // SimpleModelRenderer  — GL LineStrip, no thickness control
    Baked,  // ModelRenderer2       — baked/instanced, pixel-space width
};

// Player pilot-assist modes (phase 2 of docs/ai-ships.md): the tuning harness
// for FlightController before any AI uses it. Client-side by design -- it
// produces commands like a human would; the sim only sees the InputQueue.
enum class AutopilotMode {
    Off,
    KillVelocity, // continuously retro-burn toward zero velocity
    HoldPosition, // fly back to (and hover at) the position where engaged
};

class CGame : public Game {
protected:
    SimpleModelRenderer m_simpleModelRenderer;
    ModelRenderer2 m_modelRenderer2;

    RendererKind m_activeRenderer = RendererKind::Baked;

    Camera m_camera;
    Magnum::Vector2 m_viewportSize{1280.f, 720.f};

    bool m_cameraFollow = true;
    // Dead-zone half-size as a fraction of the visible half-extent: the ship
    // roams the central (2*fraction) of the view before the camera follows.
    static constexpr float DEAD_ZONE_FRACTION = 0.35f;

    // Shared line-thickness setting (pixels), forwarded to whichever
    // renderer is active; each converts it to its own internal units.
    float m_lineWidthPixels = Defaults::lineWidth;

    AutopilotMode m_autopilotMode = AutopilotMode::Off;
    Magnum::Math::Vector2<double> m_autopilotAnchor;
    FlightControllerParams m_flightParams;

    void UpdateCameraFollow();

    std::unique_ptr<EntitySpawner> CreateEntitySpawner() override;
public:
    struct Defaults {
        static constexpr float lineWidth = 2.5f;
    };

    static constexpr float MIN_LINE_WIDTH = 0.5f;
    static constexpr float MAX_LINE_WIDTH = 16.f;

    explicit CGame(IFilesystem& filesystem);

    void SetViewportSize(const Magnum::Vector2& size)
    {
        m_viewportSize = size;
        m_simpleModelRenderer.SetViewportSize(size);
        m_modelRenderer2.SetViewportSize(size);
    }

    // framebuffer-pixels per logical-pixel; keeps line thickness constant in
    // logical units across HiDPI/Retina displays.
    void SetPixelScale(float scale) { m_modelRenderer2.SetPixelScale(scale); }

    Camera& GetCamera() { return m_camera; }

    // Framebuffer pixels; world->screen mapping is ppu = zoom (renderers use
    // 1 px/unit at zoom 1), camera-centered.
    [[nodiscard]] const Magnum::Vector2& GetViewportSize() const { return m_viewportSize; }

    void ToggleCameraFollow() { m_cameraFollow = !m_cameraFollow; }

    void SetActiveRenderer(RendererKind kind) { m_activeRenderer = kind; }
    [[nodiscard]] RendererKind GetActiveRenderer() const { return m_activeRenderer; }

    [[nodiscard]] float GetLineWidth() const { return m_lineWidthPixels; }

    void SetLineWidth(float pixels)
    {
        m_lineWidthPixels = std::clamp(pixels, MIN_LINE_WIDTH, MAX_LINE_WIDTH);
    }

    void AddLineWidth(float deltaPixels) { SetLineWidth(m_lineWidthPixels + deltaPixels); }

    void ToggleDebugForceFacetedCircles()
    {
        m_modelRenderer2.SetDebugForceFacetedCircles(!m_modelRenderer2.GetDebugForceFacetedCircles());
    }

    [[nodiscard]] AutopilotMode GetAutopilotMode() const { return m_autopilotMode; }

    // Engaging HoldPosition captures the player's current position as anchor.
    void SetAutopilotMode(AutopilotMode mode);

    // Same mode again = off (for toggle keys).
    void ToggleAutopilotMode(AutopilotMode mode)
    {
        SetAutopilotMode(m_autopilotMode == mode ? AutopilotMode::Off : mode);
    }

    [[nodiscard]] const Magnum::Math::Vector2<double>& GetAutopilotAnchor() const { return m_autopilotAnchor; }

    // Live-tweakable from the debug UI's Flight tab.
    FlightControllerParams& GetFlightParams() { return m_flightParams; }

    // This tick's autopilot command, or nullopt when off / no player. Fire
    // bits are false; the caller merges keyboard fire.
    std::optional<ControlFlags> ComputeAutopilotControls();

    void Render(double delta);
};

} // namespace Gravitaris
