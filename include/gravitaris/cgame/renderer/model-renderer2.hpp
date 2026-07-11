#pragma once

#include <unordered_map>
#include <vector>

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

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
// transforms. Width is resolved in the shader in pixel space, so thickness
// stays constant across zoom.
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

    // Matches Line2Shader's per-instance layout (Matrix3 transform + tint).
    struct InstanceData {
        Matrix3 transform;
        Vector3 tint;
    };

    entt::registry& m_registry;
    ResourceLoader& m_resourceLoader;
    Line2Shader m_shader;

    std::unordered_map<id_t, std::unordered_map<id_t, BakedGroup>> m_baked;
    std::unordered_map<id_t, std::vector<InstanceData>> m_instanceScratch;

    Vector2 m_viewportSize{1280.f, 720.f};
    Vector2 m_cameraPos{0.f, 0.f};
    float m_pixelsPerUnit = 1.f; // zoom 1.0 matches ModelRenderer's zoom 1.0
    float m_zoom = 1.f;
    float m_lineWidthPixels = 2.f;
    float m_pixelScale = 1.f; // framebuffer-pixels per logical-pixel (HiDPI)
    bool m_debugForceFacetedCircles = false;

    void HandleModelAdded(const Model& model, id_t id);
    void HandleModelRemoved(const Model& model, id_t id);

    [[nodiscard]] Matrix3 ViewProjection() const;

    void RenderTag(id_t tag, const std::function<bool(entt::entity)>& filter);

public:
    ModelRenderer2(entt::registry& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader);

    ~ModelRenderer2();

    void SetViewportSize(const Vector2& size) { m_viewportSize = size; }
    void SetCameraPosition(const Vector2& pos) { m_cameraPos = pos; }
    void SetZoom(float zoom) { m_zoom = zoom; }
    void SetLineWidth(float pixels) { m_lineWidthPixels = pixels; }
    void SetPixelScale(float scale) { m_pixelScale = scale; }

    void SetDebugForceFacetedCircles(bool force) { m_debugForceFacetedCircles = force; }
    [[nodiscard]] bool GetDebugForceFacetedCircles() const { return m_debugForceFacetedCircles; }

    void Render(double delta);
};

} // namespace Gravitaris
