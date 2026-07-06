#include <Magnum/Mesh.h>
#include <Magnum/Math/Matrix3.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/controls.hpp>

#include <gravitaris/cgame/component/renderable.hpp>
#include <gravitaris/cgame/renderer/simple-model-renderer.hpp>

namespace Gravitaris {

using namespace Magnum;

SimpleModelRenderer::SimpleModelRenderer(entt::registry& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader)
    : m_registry(registry)
    , m_resourceLoader(resourceLoader)
    , m_shader(filesystem)
{
    m_resourceLoader.OnCreate<Model>().connect<&SimpleModelRenderer::HandleModelAdded>(*this);
    m_resourceLoader.OnDestroy<Model>().connect<&SimpleModelRenderer::HandleModelRemoved>(*this);
}

SimpleModelRenderer::~SimpleModelRenderer()
{
    m_resourceLoader.OnCreate<Model>().disconnect<&SimpleModelRenderer::HandleModelAdded>(*this);
    m_resourceLoader.OnDestroy<Model>().disconnect<&SimpleModelRenderer::HandleModelRemoved>(*this);
}

void SimpleModelRenderer::HandleModelAdded(const Model& model, const id_t id)
{
    auto& meshGroup = m_meshes[id];

    for (const auto& [ tag, group ] : model.GetModelGroups()) {
        std::vector<MeshColor> meshList;
        auto& vertexBuffer = group.vertexBuffer;
        auto& lineStrips = group.lineStrips;
        meshList.reserve(lineStrips.size());

        for (auto& lineStrip : lineStrips) {
            MeshColor meshColor;
            GL::Buffer buf;
            buf.setData(Corrade::Containers::ArrayView<const void>{
                vertexBuffer.data(),
                vertexBuffer.size() * sizeof(vertexBuffer[0])
            });
            meshColor.mesh.setPrimitive(MeshPrimitive::LineStrip)
                    .setCount(static_cast<Int>(lineStrip.count))
                    .addVertexBuffer(std::move(buf), static_cast<Int>(lineStrip.offset * sizeof(vertexBuffer[0])),
                                     Shaders::VertexColor2D::Position{});
            meshColor.color = lineStrip.color;

            meshList.emplace_back(std::move(meshColor));
        }

        meshGroup.try_emplace(tag, std::move(meshList));
    }
}

void SimpleModelRenderer::HandleModelRemoved(const Model&, const id_t id)
{
    m_meshes.erase(id);
}

void SimpleModelRenderer::RenderGroup(id_t tag
                                     , std::unordered_map<id_t, std::vector<MeshColor>>& meshGroups
                                     , const Transform& transf)
{
    if (!meshGroups.contains(tag)) return;
    auto& meshes = meshGroups.at(tag);

    Matrix3 matrix =
            Matrix3::projection({1280/2, 720/2}) *
            Matrix3::translation({static_cast<float>(transf.pos.x()), static_cast<float>(transf.pos.y())}) *
            Matrix3::rotation(Rad(transf.rot)) *
            Matrix3::scaling({static_cast<float>(transf.scale.x()), static_cast<float>(transf.scale.y())});

    m_shader.setTransformationProjectionMatrix(matrix);
    for (auto& meshColor : meshes) {
        m_shader.setColor(meshColor.color);
        m_shader.draw(meshColor.mesh);
    }
}

void SimpleModelRenderer::Render(double)
{
    using Magnum::Matrix3;

    auto view = m_registry.view<Transform, Renderable>();
    for (auto entity : view) {
        Transform& transf = view.get<Transform>(entity);
        Renderable& rend = view.get<Renderable>(entity);
        auto& meshGroups = m_meshes.at(rend.model.Id());
        RenderGroup("model"_id, meshGroups, transf);
    }

    auto thrustView = m_registry.view<Transform, Renderable, Controls>();
    for (auto entity : thrustView) {
        Transform& transf = thrustView.get<Transform>(entity);
        Renderable& rend = thrustView.get<Renderable>(entity);
        Controls& controls = thrustView.get<Controls>(entity);
        if (!controls.actionFlags.thrustForward) continue;

        auto& meshGroups = m_meshes.at(rend.model.Id());
        RenderGroup("_thrust"_id, meshGroups, transf);

        
    }
}

} // Gravitaris
