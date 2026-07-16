#pragma once

#include <functional>
#include <unordered_map>
#include <vector>

#include <flecs.h>

#include <Magnum/Magnum.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>

#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/component/renderable.hpp>

#include <gravitaris/cgame/renderer/shader/line2-shader.hpp>

namespace Gravitaris {

using Magnum::Matrix3;
using Magnum::Vector2;
using Magnum::Vector3;

// Baked/instanced line renderer: line->triangle expansion happens once per
// model at load time (static mesh), so instancing is free for per-entity
// transforms. Width is resolved in the shader in pixel space.
//
// zoomWidthFactor controls how much the pixel width tracks zoom, normalized so
// that at referenceZoom the line is exactly lineWidthPixels wide regardless of
// the factor (so tuning the factor never changes appearance at the normal zoom
// level). Away from referenceZoom:
//   pixelWidth = lineWidthPixels * (zoom / referenceZoom)^zoomWidthFactor
// 0 = constant pixel width across zoom; 1 = constant world-space width (pixel
// width scales linearly with zoom). See SetZoomWidthFactor / SetReferenceZoom.
//
// Kept alongside ModelRenderer/SimpleModelRenderer for comparison; remove
// those once this is validated.
class ModelRenderer2 {
private:
    // Per (model, tag) cached mesh. instanceBuffer is refilled per frame.
    // debugFacetedMesh: same group baked without circle special-casing, for
    // SetDebugForceFacetedCircles() A/B toggling; shares instanceBuffer.
    struct BakedGroup {
        Magnum::GL::Mesh mesh;
        Magnum::GL::Mesh debugFacetedMesh;
        Magnum::GL::Buffer instanceBuffer;
        Magnum::Int vertexCount = 0;
        Magnum::Int debugFacetedVertexCount = 0;
    };

    // Matches Line2Shader's per-instance layout (Matrix3 transform + team
    // color + hit-flash amount).
    struct InstanceData {
        Matrix3 transform;
        Vector3 teamColor;
        float flash;
    };

    flecs::world& m_registry;
    ResourceLoader& m_resourceLoader;
    Line2Shader m_shader;

    std::unordered_map<id_t, std::unordered_map<id_t, BakedGroup>> m_baked;
    std::unordered_map<id_t, std::vector<InstanceData>> m_instanceScratch;

    // Per-frame, non-entity instances (HUD arrows etc.), keyed by model id.
    // Merged into the "model" group's pass alongside real entities and cleared
    // at the end of Render(); see SubmitOverlay.
    std::unordered_map<id_t, std::vector<InstanceData>> m_overlayScratch;

    Vector2 m_viewportSize{1280.f, 720.f};
    Vector2 m_cameraPos{0.f, 0.f};
    float m_pixelsPerUnit = 1.f; // zoom 1.0 matches ModelRenderer's zoom 1.0
    float m_zoom = 1.f;
    float m_lineWidthPixels = 2.f;
    float m_zoomWidthFactor = 0.f; // 0 = width constant across zoom, 1 = width scales linearly with zoom
    float m_referenceZoom = 1.f;   // zoom at which lineWidthPixels is the literal pixel width
    float m_pixelScale = 1.f; // framebuffer-pixels per logical-pixel (HiDPI)
    bool m_debugForceFacetedCircles = false;

    void HandleModelAdded(const Model& model, id_t id);
    void HandleModelRemoved(const Model& model, id_t id);

    [[nodiscard]] Matrix3 ViewProjection() const;

    void RenderTag(id_t tag, const std::function<bool(flecs::entity)>& filter);

public:
    ModelRenderer2(flecs::world& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader);

    ~ModelRenderer2();

    void SetViewportSize(const Vector2& size) { m_viewportSize = size; }
    void SetCameraPosition(const Vector2& pos) { m_cameraPos = pos; }
    void SetZoom(float zoom) { m_zoom = zoom; }
    void SetLineWidth(float pixels) { m_lineWidthPixels = pixels; }
    // See class comment: 0 keeps pixel width constant across zoom, 1 makes it scale linearly with zoom.
    void SetZoomWidthFactor(float factor) { m_zoomWidthFactor = factor; }
    [[nodiscard]] float GetZoomWidthFactor() const { return m_zoomWidthFactor; }
    // Zoom at which the width matches lineWidthPixels exactly (usually the default camera zoom).
    void SetReferenceZoom(float zoom) { m_referenceZoom = zoom; }
    void SetPixelScale(float scale) { m_pixelScale = scale; }

    void SetDebugForceFacetedCircles(bool force) { m_debugForceFacetedCircles = force; }
    [[nodiscard]] bool GetDebugForceFacetedCircles() const { return m_debugForceFacetedCircles; }

    // Draws `modelId`'s "model" group once more this frame with an arbitrary
    // transform, for things that aren't entities (HUD arrows, markers). Rides
    // the same instanced draw as real entities, so overlays get the same line
    // width and glow for free. `transform` is world space -- a caller wanting
    // screen-anchored placement converts px to world itself (see
    // CGame::UpdateIndicators). `color` fills the same slot as an entity's team
    // color, i.e. it only reaches strokes/fills authored in
    // TEAM_COLOR_PLACEHOLDER. Submissions last one frame: call every frame the
    // overlay should be visible, before Render().
    void SubmitOverlay(id_t modelId, const Matrix3& transform, const Vector3& color, float flash = 0.f);

    void Render(double delta);
};

} // namespace Gravitaris
