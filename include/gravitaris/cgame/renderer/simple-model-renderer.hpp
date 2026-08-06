#pragma once

#include <unordered_map>
#include <unordered_set>

#include <flecs.h>

#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/Shaders/VertexColor.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>

#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/component/renderable.hpp>

#include <gravitaris/cgame/renderer/shader/simple-line-shader.hpp>

namespace Gravitaris {

using Magnum::Matrix3;
using Magnum::Vector2;

class SimpleModelRenderer {
private:
    struct MeshColor {
        Magnum::GL::Mesh mesh;
        Color3 color;
    };

    // Every strip of a group is a range of that group's one vertex buffer, so
    // the buffer is uploaded once and the strips only differ in the offset and
    // count they draw from it. The meshes hold nothing but its GL id
    // (Magnum's AttributeLayout wraps the id, never a pointer to this object),
    // so moving a GroupMeshes is fine -- but the buffer has to outlive them,
    // which is what keeping the two in one struct buys. Declared first so it
    // is destroyed last.
    struct GroupMeshes {
        Magnum::GL::Buffer vertexBuffer;
        std::vector<MeshColor> strips;
    };

    flecs::world& m_registry;

    ResourceLoader& m_resourceLoader;

    SimpleLineShader m_shader;

    std::unordered_map<id_t, std::unordered_map<id_t, GroupMeshes>> m_meshes;

    // Same convention as ModelRenderer2 (1 px/unit at zoom 1.0), so switching
    // the active renderer at runtime doesn't change the visible framing.
    Vector2 m_viewportSize{1280.f, 720.f};
    Vector2 m_cameraPos{0.f, 0.f};
    float m_pixelsPerUnit = 1.f;
    float m_zoom = 1.f;
    float m_contentScale = 1.f;

    void HandleModelAdded(const Model& model, id_t id);

    void HandleModelRemoved(const Model& model, id_t id);

    [[nodiscard]] Matrix3 ViewProjection() const;

    void RenderGroup(id_t tag, std::unordered_map<id_t, GroupMeshes>& m_meshGroup, const Transform& transf);

public:
    SimpleModelRenderer(flecs::world& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader);

    ~SimpleModelRenderer();

    void SetViewportSize(const Vector2& size) { m_viewportSize = size; }
    void SetCameraPosition(const Vector2& pos) { m_cameraPos = pos; }
    void SetZoom(float zoom) { m_zoom = zoom; }
    void SetContentScale(float scale) { m_contentScale = scale; }

    void Render(double delta);
};

} // namespace Gravitaris
