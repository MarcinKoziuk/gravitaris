#include <Magnum/Mesh.h>
#include <Magnum/Math/Matrix3.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/controls.hpp>

#include <gravitaris/cgame/component/renderable.hpp>
#include <gravitaris/cgame/renderer/simple-model-renderer.hpp>

namespace Gravitaris {

using namespace Magnum;

SimpleModelRenderer::SimpleModelRenderer(flecs::world& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader)
    : m_registry(registry)
    , m_resourceLoader(resourceLoader)
    , m_shader(filesystem)
{
    m_resourceLoader.OnCreate<Model>().connect(&SimpleModelRenderer::HandleModelAdded, this);
    m_resourceLoader.OnDestroy<Model>().connect(&SimpleModelRenderer::HandleModelRemoved, this);
}

SimpleModelRenderer::~SimpleModelRenderer()
{
    m_resourceLoader.OnCreate<Model>().disconnect(&SimpleModelRenderer::HandleModelAdded, this);
    m_resourceLoader.OnDestroy<Model>().disconnect(&SimpleModelRenderer::HandleModelRemoved, this);
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
            // No per-entity team data here; show placeholder strokes as white.
            meshColor.color = lineStrip.teamColor ? Color3{1.f} : lineStrip.color;

            meshList.emplace_back(std::move(meshColor));
        }

        meshGroup.try_emplace(tag, std::move(meshList));
    }
}

void SimpleModelRenderer::HandleModelRemoved(const Model&, const id_t id)
{
    m_meshes.erase(id);
}

Matrix3 SimpleModelRenderer::ViewProjection() const
{
    // World -> NDC. Extent shrinks as zoom grows, so more pixels per world
    // unit -- same convention ModelRenderer2 uses (1 px/unit at zoom 1.0).
    const float ppu = m_pixelsPerUnit * m_zoom;
    const Vector2 extent = m_viewportSize / ppu;
    return Matrix3::projection(extent) * Matrix3::translation(-m_cameraPos);
}

void SimpleModelRenderer::RenderGroup(id_t tag
                                     , std::unordered_map<id_t, std::vector<MeshColor>>& meshGroups
                                     , const Transform& transf)
{
    if (!meshGroups.contains(tag)) return;
    auto& meshes = meshGroups.at(tag);

    Matrix3 matrix =
            ViewProjection() *
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

    // find + skip (not .at()): a Renderable can reference a model with no
    // baked meshes, e.g. a failed load's placeholder. ModelRenderer2 skips
    // those the same way.
    m_registry.each([&](flecs::entity, Transform& transf, Renderable& rend) {
        auto it = m_meshes.find(rend.model.Id());
        if (it == m_meshes.end()) return;
        RenderGroup("model"_id, it->second, transf);
    });

    m_registry.each([&](flecs::entity, Transform& transf, Renderable& rend, Controls& controls) {
        if (!controls.actionFlags.thrustForward) return;

        auto it = m_meshes.find(rend.model.Id());
        if (it == m_meshes.end()) return;
        RenderGroup("_thrust"_id, it->second, transf);
    });
}

} // Gravitaris
