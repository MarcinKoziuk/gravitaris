#pragma once

#include <algorithm>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/game.hpp>

#include <gravitaris/cgame/camera.hpp>
#include <gravitaris/cgame/renderer/simple-model-renderer.hpp>
#include <gravitaris/cgame/renderer/model-renderer.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>

namespace Gravitaris {

class CGame : public Game {
protected:
    SimpleModelRenderer m_simpleModelRenderer;
    ModelRenderer m_modelRenderer;
    ModelRenderer2 m_modelRenderer2;

    Camera m_camera;
    Magnum::Vector2 m_viewportSize{1280.f, 720.f};

    bool m_cameraFollow = true;
    // Dead-zone half-size as a fraction of the visible half-extent: the ship
    // roams the central (2*fraction) of the view before the camera follows.
    static constexpr float DEAD_ZONE_FRACTION = 0.35f;

    // Shared line-thickness setting (pixels), forwarded to whichever
    // renderer is active; each converts it to its own internal units.
    float m_lineWidthPixels = 2.f;
    static constexpr float MIN_LINE_WIDTH = 0.5f;
    static constexpr float MAX_LINE_WIDTH = 16.f;

    void UpdateCameraFollow();

    std::unique_ptr<EntitySpawner> CreateEntitySpawner() override;
public:
    explicit CGame(IFilesystem& filesystem);

    void SetViewportSize(const Magnum::Vector2& size)
    {
        m_viewportSize = size;
        m_modelRenderer2.SetViewportSize(size);
    }

    // framebuffer-pixels per logical-pixel; keeps line thickness constant in
    // logical units across HiDPI/Retina displays.
    void SetPixelScale(float scale) { m_modelRenderer2.SetPixelScale(scale); }

    Camera& GetCamera() { return m_camera; }

    void ToggleCameraFollow() { m_cameraFollow = !m_cameraFollow; }

    [[nodiscard]] float GetLineWidth() const { return m_lineWidthPixels; }

    void AddLineWidth(float deltaPixels)
    {
        m_lineWidthPixels = std::clamp(m_lineWidthPixels + deltaPixels, MIN_LINE_WIDTH, MAX_LINE_WIDTH);
    }

    void ToggleDebugForceFacetedCircles()
    {
        m_modelRenderer2.SetDebugForceFacetedCircles(!m_modelRenderer2.GetDebugForceFacetedCircles());
    }

    void Render(double delta);
};

} // namespace Gravitaris
