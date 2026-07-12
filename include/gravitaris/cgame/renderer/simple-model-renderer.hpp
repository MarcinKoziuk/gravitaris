#pragma once

#include <unordered_map>
#include <unordered_set>

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

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

    entt::registry& m_registry;

    ResourceLoader& m_resourceLoader;

    SimpleLineShader m_shader;

    std::unordered_map<id_t,
        std::unordered_map<id_t, std::vector<MeshColor>>> m_meshes;

    // Same convention as ModelRenderer2 (1 px/unit at zoom 1.0), so switching
    // the active renderer at runtime doesn't change the visible framing.
    Vector2 m_viewportSize{1280.f, 720.f};
    Vector2 m_cameraPos{0.f, 0.f};
    float m_pixelsPerUnit = 1.f;
    float m_zoom = 1.f;

    void HandleModelAdded(const Model& model, id_t id);

    void HandleModelRemoved(const Model& model, id_t id);

    [[nodiscard]] Matrix3 ViewProjection() const;

    void RenderGroup(id_t tag, std::unordered_map<id_t, std::vector<MeshColor>>& m_meshGroup, const Transform& transf);

public:
    SimpleModelRenderer(entt::registry& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader);

    ~SimpleModelRenderer();

    void SetViewportSize(const Vector2& size) { m_viewportSize = size; }
    void SetCameraPosition(const Vector2& pos) { m_cameraPos = pos; }
    void SetZoom(float zoom) { m_zoom = zoom; }

    void Render(double delta);
};

} // namespace Gravitaris
