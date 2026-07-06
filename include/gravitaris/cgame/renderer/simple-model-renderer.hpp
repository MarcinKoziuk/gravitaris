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

#include <gravitaris/cgame/renderer/shader/simple-line-shader.hpp>

namespace Gravitaris {

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

    void HandleModelAdded(const Model& model, id_t id);

    void HandleModelRemoved(const Model& model, id_t id);

    void RenderGroup(id_t tag, std::unordered_map<id_t, std::vector<MeshColor>>& m_meshGroup, const Transform& transf);

public:
    SimpleModelRenderer(entt::registry& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader);

    ~SimpleModelRenderer();

    void Render(double delta);
};

} // namespace Gravitaris
