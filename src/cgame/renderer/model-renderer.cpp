#include <Magnum/Mesh.h>
#include <Magnum/Math/Matrix3.h>

#include <Magnum/GL/GL.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/controls.hpp>

#include <gravitaris/cgame/component/renderable.hpp>
#include <gravitaris/cgame/renderer/model-renderer.hpp>

namespace Gravitaris {

using namespace Magnum;

static const Vector2 SEGMENT_GEOMETRY[] = {
        Vector2{0.f, -0.5f},
        Vector2{1.f, -0.5f},
        Vector2{1.f,  0.5f},
        Vector2{0.f, -0.5f},
        Vector2{1.f,  0.5f},
        Vector2{0.f,  0.5f}
};

static const Vector3 MITER_GEOMETRY[] = {
        Vector3{0.f, 0.f, 0.f},
        Vector3{1.f, 0.f, 0.f},
        Vector3{0.f, 1.f, 0.f},
        Vector3{0.f, 0.f, 0.f},
        Vector3{0.f, 1.f, 0.f},
        Vector3{0.f, 0.f, 1.f}
};

static void openglDebugCallback(GLenum source, GLenum type, GLuint id,
                                    GLenum severity, GLsizei length,
                                    const GLchar* message, const void* userParam) {
    std::cerr << "GL Debug Message: " << message << std::endl;

    // Optionally, you can filter out non-warning messages:
    /*if (severity == GL_DEBUG_SEVERITY_HIGH || severity == GL_DEBUG_SEVERITY_MEDIUM || severity == GL_DEBUG_SEVERITY_LOW) {
        std::cerr << "Source: " << source << ", Type: " << type << ", ID: " << id << std::endl;
        std::cerr << "Severity: " << severity << ", Message: " << message << std::endl;
    }*/
}

ModelRenderer::ModelRenderer(entt::registry& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader)
    : m_registry(registry)
    , m_resourceLoader(resourceLoader)
    , m_lineSegmentsShader(filesystem)
    , m_lineMiterShader(filesystem)
{
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(openglDebugCallback, nullptr);
    // Suppress NVIDIA's "Buffer object will use VIDEO memory"
    glDebugMessageControl(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_OTHER,
                          GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);

    m_resourceLoader.OnCreate<Model>().connect<&ModelRenderer::HandleModelAdded>(*this);
    m_resourceLoader.OnDestroy<Model>().connect<&ModelRenderer::HandleModelRemoved>(*this);
}

ModelRenderer::~ModelRenderer()
{
    m_resourceLoader.OnCreate<Model>().disconnect<&ModelRenderer::HandleModelAdded>(*this);
    m_resourceLoader.OnDestroy<Model>().disconnect<&ModelRenderer::HandleModelRemoved>(*this);
}

void ModelRenderer::HandleModelAdded(const Model& model, const id_t id)
{
    auto& meshGroup = m_meshes[id];

    for (const auto& [ tag, group ] : model.GetModelGroups()) {
        std::vector<MeshColor> meshList;
        auto& vertexBuffer = group.vertexBuffer;
        auto& lineStrips = group.lineStrips;
        meshList.reserve(lineStrips.size());

        auto makeBuf = [](const void* data, std::size_t byteCount) -> GL::Buffer {
            GL::Buffer buf;
            buf.setData(Corrade::Containers::ArrayView<const void>{data, byteCount});
            return buf;
        };
        auto makeVecBuf = [&makeBuf](const std::vector<Vector2>& v) -> GL::Buffer {
            return makeBuf(v.data(), v.size() * sizeof(v[0]));
        };

        for (auto& lineStrip : lineStrips) {
            const GLsizei elementSize = sizeof(Vector2);
            const auto bufferOffset = static_cast<GLintptr>(lineStrip.offset * elementSize);

            MeshColor meshColor;
            GL::Mesh segment;
            segment.setPrimitive(MeshPrimitive::Triangles)
                    .setCount(Containers::arraySize(SEGMENT_GEOMETRY))
                    .addVertexBuffer(
                            makeBuf(SEGMENT_GEOMETRY, sizeof(SEGMENT_GEOMETRY)),
                            0L,
                            LineSegmentsShader::Position{});

            segment.setInstanceCount(static_cast<Int>(lineStrip.count - 1))
                    .addVertexBufferInstanced(
                            makeVecBuf(vertexBuffer),
                            1UL,
                            bufferOffset,
                            0,
                            LineSegmentsShader::InstancePointA{})
                    .addVertexBufferInstanced(
                            makeVecBuf(vertexBuffer),
                            1UL,
                            static_cast<GLintptr>(bufferOffset + elementSize),
                            0,
                            LineSegmentsShader::InstancePointB{});

            GL::Mesh join;
            join.setPrimitive(MeshPrimitive::Triangles)
                    .setCount(Containers::arraySize(SEGMENT_GEOMETRY))
                    .addVertexBuffer(
                            makeBuf(MITER_GEOMETRY, sizeof(MITER_GEOMETRY)),
                            0L,
                            LineMiterShader::Position{});
            join.setInstanceCount(static_cast<Int>(lineStrip.count - 2))
                    .addVertexBufferInstanced(
                            makeVecBuf(vertexBuffer),
                            1UL,
                            bufferOffset,
                            0,
                            LineMiterShader::InstancePointA{})
                    .addVertexBufferInstanced(
                            makeVecBuf(vertexBuffer),
                            1UL,
                            static_cast<GLintptr>(bufferOffset + elementSize),
                            0,
                            LineMiterShader::InstancePointB{})
                    .addVertexBufferInstanced(
                            makeVecBuf(vertexBuffer),
                            1UL,
                            static_cast<GLintptr>(bufferOffset + elementSize * 2),
                            0,
                            LineMiterShader::InstancePointC{});


            meshColor.segment = std::move(segment);
            meshColor.join = std::move(join);
            meshColor.color = lineStrip.color;

            meshList.emplace_back(std::move(meshColor));
        }

        meshGroup.try_emplace(tag, std::move(meshList));
    }
}

void ModelRenderer::HandleModelRemoved(const Model&, const id_t id)
{
    m_meshes.erase(id);
}


void ModelRenderer::RenderGroup(id_t tag
        , std::unordered_map<id_t, std::vector<MeshColor>>& meshGroups
        , const Transform& transf)
{
    if (!meshGroups.contains(tag)) return;
    auto& meshes = meshGroups.at(tag);

    Matrix3 matrix =
            Matrix3::projection({1280/4, 720/4}) *
            Matrix3::translation({static_cast<float>(transf.pos.x()), static_cast<float>(transf.pos.y())}) *
            Matrix3::rotation(Rad(transf.rot)) *
            Matrix3::scaling({static_cast<float>(transf.scale.x()), static_cast<float>(transf.scale.y())});

    m_lineSegmentsShader.setWidth(0.5f);
    m_lineSegmentsShader.setTransformationProjectionMatrix(matrix);
    for (auto& meshColor : meshes) {
        m_lineSegmentsShader.setColor(meshColor.color);
        m_lineSegmentsShader.draw(meshColor.segment);
    }

    m_lineMiterShader.setWidth(0.5f);
    m_lineMiterShader.setTransformationProjectionMatrix(matrix);
    for (auto& meshColor : meshes) {
        m_lineMiterShader.setColor(meshColor.color);
        m_lineMiterShader.draw(meshColor.join);
    }
}

void ModelRenderer::Render(double)
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
