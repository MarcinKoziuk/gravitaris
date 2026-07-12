#include <algorithm>
#include <functional>
#include <vector>

#include <Corrade/Containers/ArrayView.h>

#include <Magnum/Mesh.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Color.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/Renderer.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/controls.hpp>

#include <gravitaris/cgame/component/renderable.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>

namespace Gravitaris {

using namespace Magnum;

namespace {

// Must match Line2Shader's vertex layout.
struct LineVertex {
    Vector2 pointA;
    Vector2 pointB;
    Vector2 pointC;
    Vector4 param; // xyz weights, w = type (0 segment, 1 join, 2 circle)
    Vector3 color;
};

// Segment quad: t along A->B, side offset.
constexpr Vector2 SEGMENT[] = {
        {0.f, -0.5f}, {1.f, -0.5f}, {1.f, 0.5f},
        {0.f, -0.5f}, {1.f, 0.5f},  {0.f, 0.5f},
};

// Miter corner weights.
constexpr Vector3 MITER[] = {
        {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f},
        {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f},
};

// Circle billboard quad, corners in [-1,1]^2.
constexpr Vector2 CIRCLE_QUAD[] = {
        {-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f},
        {-1.f, -1.f}, {1.f, 1.f},  {-1.f, 1.f},
};

// Bakes a group's strips into one vertex buffer: quad per segment, miter fan
// per interior vertex, single billboard per circle strip (see
// Model::VertexLineStrip::circle) unless forceFaceted forces plain
// segments/miters instead (SetDebugForceFacetedCircles A/B toggle).
std::vector<LineVertex> BakeGroup(const Model::Group& group, bool forceFaceted)
{
    std::vector<LineVertex> out;

    // 6 verts per segment (count-1) + 6 per join (count-2) + 6 per circle.
    std::size_t total = 0;
    for (const auto& strip : group.lineStrips) {
        if (strip.circle && !forceFaceted) {
            total += 6;
        } else {
            if (strip.count >= 2) total += (strip.count - 1) * 6;
            if (strip.count >= 3) total += (strip.count - 2) * 6;
        }
    }
    out.reserve(total);

    for (const auto& strip : group.lineStrips) {
        const Vector3 color = strip.color.toSrgb();

        if (strip.circle && !forceFaceted) {
            const Vector2& center = strip.circle->center;
            const Vector2 radiusCarrier{strip.circle->radius, 0.f};
            for (const Vector2& corner : CIRCLE_QUAD) {
                out.push_back(LineVertex{center, radiusCarrier, Vector2{},
                                          Vector4{corner.x(), corner.y(), 0.f, 2.f}, color});
            }
            continue;
        }

        if (strip.count < 2) continue;
        const Vector2* pts = group.vertexBuffer.data() + strip.offset;

        // Segments: one quad between consecutive points.
        for (std::size_t i = 0; i + 1 < strip.count; ++i) {
            const Vector2 a = pts[i];
            const Vector2 b = pts[i + 1];
            for (const Vector2& g : SEGMENT) {
                out.push_back(LineVertex{a, b, Vector2{}, Vector4{g.x(), g.y(), 0.f, 0.f}, color});
            }
        }

        // Joins: miter at each interior vertex (needs A, B=join, C).
        for (std::size_t i = 0; i + 2 < strip.count; ++i) {
            const Vector2 a = pts[i];
            const Vector2 b = pts[i + 1];
            const Vector2 c = pts[i + 2];
            for (const Vector3& g : MITER) {
                out.push_back(LineVertex{a, b, c, Vector4{g.x(), g.y(), g.z(), 1.f}, color});
            }
        }
    }

    return out;
}

// glBufferData occasionally raises a first-chance SEH exception in the
// NVIDIA driver on this machine (root cause unknown); catch it so it
// doesn't kill the process. Upload still succeeds either way.
unsigned long SafeUpload(Magnum::GL::Buffer& buf, const void* data, std::size_t bytes)
{
#if defined(_WIN323)
    __try {
        buf.setData(Containers::ArrayView<const void>{data, bytes});
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
#else
    buf.setData(Containers::ArrayView<const void>{data, bytes});
    return 0;
#endif
}

} // namespace

ModelRenderer2::ModelRenderer2(flecs::world& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader)
    : m_registry(registry)
    , m_resourceLoader(resourceLoader)
    , m_shader(filesystem)
{
    m_resourceLoader.OnCreate<Model>().connect(&ModelRenderer2::HandleModelAdded, this);
    m_resourceLoader.OnDestroy<Model>().connect(&ModelRenderer2::HandleModelRemoved, this);
}

ModelRenderer2::~ModelRenderer2()
{
    m_resourceLoader.OnCreate<Model>().disconnect(&ModelRenderer2::HandleModelAdded, this);
    m_resourceLoader.OnDestroy<Model>().disconnect(&ModelRenderer2::HandleModelRemoved, this);
}

void ModelRenderer2::HandleModelAdded(const Model& model, const id_t id)
{
    auto& groups = m_baked[id];

    for (const auto& [tag, group] : model.GetModelGroups()) {
        std::vector<LineVertex> vertices = BakeGroup(group, false);
        if (vertices.empty()) continue;

        GL::Buffer vertexBuffer;
        if (unsigned long ex = SafeUpload(vertexBuffer, vertices.data(), vertices.size() * sizeof(LineVertex))) {
            LOG(error) << "[MR2] vertex buffer upload raised exception 0x" << std::hex << ex
                       << " for model " << id << " tag " << tag;
            continue;
        }

        BakedGroup baked;
        baked.vertexCount = static_cast<Int>(vertices.size());
        baked.mesh.setPrimitive(MeshPrimitive::Triangles)
                .setCount(baked.vertexCount)
                .addVertexBuffer(std::move(vertexBuffer), 0,
                                 Line2Shader::PointA{},
                                 Line2Shader::PointB{},
                                 Line2Shader::PointC{},
                                 Line2Shader::Param{},
                                 Line2Shader::VertexColor{})
                .addVertexBufferInstanced(baked.instanceBuffer, 1, 0,
                                          Line2Shader::InstanceTransform{},
                                          Line2Shader::InstanceTint{});

        const bool hasCircle = std::any_of(group.lineStrips.begin(), group.lineStrips.end(),
                                            [](const Model::VertexLineStrip& strip) { return strip.circle.has_value(); });
        if (hasCircle) {
            std::vector<LineVertex> debugVertices = BakeGroup(group, true);
            GL::Buffer debugVertexBuffer;
            if (unsigned long ex = SafeUpload(debugVertexBuffer, debugVertices.data(),
                                               debugVertices.size() * sizeof(LineVertex))) {
                LOG(error) << "[MR2] debug faceted vertex buffer upload raised exception 0x" << std::hex << ex
                           << " for model " << id << " tag " << tag;
            } else {
                baked.debugFacetedVertexCount = static_cast<Int>(debugVertices.size());
                baked.debugFacetedMesh.setPrimitive(MeshPrimitive::Triangles)
                        .setCount(baked.debugFacetedVertexCount)
                        .addVertexBuffer(std::move(debugVertexBuffer), 0,
                                         Line2Shader::PointA{},
                                         Line2Shader::PointB{},
                                         Line2Shader::PointC{},
                                         Line2Shader::Param{},
                                         Line2Shader::VertexColor{})
                        .addVertexBufferInstanced(baked.instanceBuffer, 1, 0,
                                                  Line2Shader::InstanceTransform{},
                                                  Line2Shader::InstanceTint{});
            }
        }

        groups.emplace(tag, std::move(baked));
    }
}

void ModelRenderer2::HandleModelRemoved(const Model&, const id_t id)
{
    m_baked.erase(id);
}

Matrix3 ModelRenderer2::ViewProjection() const
{
    // world -> NDC. Extent shrinks as zoom grows, so more pixels per world unit.
    const float ppu = m_pixelsPerUnit * m_zoom;
    const Vector2 extent = m_viewportSize / ppu;
    return Matrix3::projection(extent)
            * Matrix3::translation(-m_cameraPos);
}

void ModelRenderer2::RenderTag(id_t tag, const std::function<bool(flecs::entity)>& filter)
{
    for (auto& [modelId, instances] : m_instanceScratch) instances.clear();

    m_registry.each([&](flecs::entity entity, const Transform& t, const Renderable& rend) {
        if (filter && !filter(entity)) return;

        const id_t modelId = rend.model.Id();

        auto bakedIt = m_baked.find(modelId);
        if (bakedIt == m_baked.end() || !bakedIt->second.contains(tag)) return;

        Matrix3 transform =
                Matrix3::translation({static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())}) *
                Matrix3::rotation(Rad(t.rot)) *
                Matrix3::scaling({static_cast<float>(t.scale.x()), static_cast<float>(t.scale.y())});

        m_instanceScratch[modelId].push_back(InstanceData{transform, Vector3{1.f, 1.f, 1.f}});
    });

    for (auto& [modelId, instances] : m_instanceScratch) {
        if (instances.empty()) continue;

        auto& baked = m_baked.at(modelId).at(tag);
        baked.instanceBuffer.setData(Containers::ArrayView<const void>{
                instances.data(), instances.size() * sizeof(InstanceData)});

        const bool useDebugMesh = m_debugForceFacetedCircles && baked.debugFacetedVertexCount > 0;
        GL::Mesh& meshToDraw = useDebugMesh ? baked.debugFacetedMesh : baked.mesh;
        meshToDraw.setInstanceCount(static_cast<Int>(instances.size()));

        m_shader.draw(meshToDraw);
    }
}

void ModelRenderer2::Render(double)
{
    // Needed for the fragment shader's analytic edge-AA alpha falloff.
    GL::Renderer::enable(GL::Renderer::Feature::Blending);
    GL::Renderer::setBlendFunction(GL::Renderer::BlendFunction::SourceAlpha,
                                    GL::Renderer::BlendFunction::OneMinusSourceAlpha);

    m_shader.setViewportSize(m_viewportSize)
            .setViewProjection(ViewProjection())
            .setWidth(m_lineWidthPixels * m_pixelScale);

    RenderTag("model"_id, {});

    RenderTag("_thrust"_id, [](flecs::entity entity) {
        const auto* controls = entity.try_get<Controls>();
        return controls && controls->actionFlags.thrustForward;
    });
}

} // namespace Gravitaris
