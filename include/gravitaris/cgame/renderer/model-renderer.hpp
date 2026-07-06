#pragma once

#include <unordered_map>
#include <unordered_set>

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include <Magnum/GL/Mesh.h>
#include <Magnum/Shaders/VertexColor.h>

#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>

#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/component/renderable.hpp>

#include <gravitaris/cgame/renderer/shader/line-segments-shader.hpp>
#include <gravitaris/cgame/renderer/shader/line-miter-shader.hpp>

namespace Gravitaris {

class ModelRenderer {
private:
    struct MeshColor {
        Magnum::GL::Mesh segment;
        Magnum::GL::Mesh join;
        Color3 color;
    };

    entt::registry& m_registry;

    ResourceLoader& m_resourceLoader;

    LineSegmentsShader m_lineSegmentsShader;

    LineMiterShader m_lineMiterShader;

    Magnum::GL::Mesh m_segmentsMesh;

    std::unordered_map<id_t,
        std::unordered_map<id_t, std::vector<MeshColor>>> m_meshes;

    Magnum::Vector2 m_cameraPos{0.f, 0.f};
    float m_zoom = 1.f;
    float m_lineWidthPixels = 2.f;

    void HandleModelAdded(const Model& model, id_t id);

    void HandleModelRemoved(const Model& model, id_t id);

    void RenderGroup(id_t tag, std::unordered_map<id_t, std::vector<MeshColor>>& m_meshGroup, const Transform& transf);

public:
    ModelRenderer(entt::registry& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader);

    ~ModelRenderer();

    void SetCameraPosition(const Magnum::Vector2& pos) { m_cameraPos = pos; }
    void SetZoom(float zoom) { m_zoom = zoom; }
    void SetLineWidth(float pixels) { m_lineWidthPixels = pixels; }

    void Render(double delta);
};

} // namespace Gravitaris
