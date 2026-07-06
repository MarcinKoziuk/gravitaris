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

// Option-B ("baked") line renderer.
//
// The expensive line->triangle expansion is done once on the CPU at model-load
// time and cached as a static per-model mesh. Segment/miter adjacency is stored
// as regular per-vertex attributes, which frees GPU instancing to do what it is
// good at: drawing many entities that share one model in a single draw call.
//
// Line width is resolved in pixel space inside the shader, so on-screen
// thickness stays constant across zoom levels with no per-zoom cache.
//
// Kept alongside the original ModelRenderer/SimpleModelRenderer so both can be
// compared; the old ones can be removed once this is validated.
class ModelRenderer2 {
private:
    // One cached, ready-to-draw mesh per (model, tag e.g. "model"/"_thrust").
    // The vertex buffer is static; the instance buffer is refilled each frame
    // with the transforms of every entity that uses this model+tag.
    //
    // debugFacetedMesh is a second bake of the same group with circle
    // special-casing disabled (plain segments/miters, like before analytic
    // circles existed) purely for toggling a visual A/B comparison; see
    // SetDebugForceFacetedCircles(). It shares instanceBuffer with `mesh`.
    struct BakedGroup {
        Magnum::GL::Mesh mesh;
        Magnum::GL::Mesh debugFacetedMesh;
        Magnum::GL::Buffer instanceBuffer;
        Magnum::Int vertexCount = 0;
        Magnum::Int debugFacetedVertexCount = 0;
    };

    // Scratch per-entity instance data, matches the interleaved layout the
    // Line2Shader expects (Matrix3 transform + Vector3 tint).
    struct InstanceData {
        Matrix3 transform;
        Vector3 tint;
    };

    entt::registry& m_registry;
    ResourceLoader& m_resourceLoader;
    Line2Shader m_shader;

    std::unordered_map<id_t, std::unordered_map<id_t, BakedGroup>> m_baked;

    // Reused across frames to avoid per-frame allocations.
    std::unordered_map<id_t, std::vector<InstanceData>> m_instanceScratch;

    Vector2 m_viewportSize{1280.f, 720.f};
    Vector2 m_cameraPos{0.f, 0.f};
    // At Camera zoom 1.0 this reproduces the validated baseline framing
    // (ship + full orbit ring visible), matching ModelRenderer's zoom=1.0.
    float m_pixelsPerUnit = 1.f;
    float m_zoom = 1.f;
    float m_lineWidthPixels = 2.f;
    bool m_debugForceFacetedCircles = false;

    void HandleModelAdded(const Model& model, id_t id);
    void HandleModelRemoved(const Model& model, id_t id);

    [[nodiscard]] Matrix3 ViewProjection() const;

    // Collect instances for `tag` grouped by model id into m_instanceScratch,
    // then upload and draw one instanced call per model.
    void RenderTag(id_t tag, const std::function<bool(entt::entity)>& filter);

public:
    ModelRenderer2(entt::registry& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader);

    ~ModelRenderer2();

    void SetViewportSize(const Vector2& size) { m_viewportSize = size; }
    void SetCameraPosition(const Vector2& pos) { m_cameraPos = pos; }
    void SetZoom(float zoom) { m_zoom = zoom; }
    void SetLineWidth(float pixels) { m_lineWidthPixels = pixels; }

    // Debug: when true, circle strips are drawn with the old faceted
    // segment/miter tessellation instead of the analytic SDF circle, so the
    // two can be compared live without restarting.
    void SetDebugForceFacetedCircles(bool force) { m_debugForceFacetedCircles = force; }
    [[nodiscard]] bool GetDebugForceFacetedCircles() const { return m_debugForceFacetedCircles; }

    void Render(double delta);
};

} // namespace Gravitaris
