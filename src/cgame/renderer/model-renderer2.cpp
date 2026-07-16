#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#if defined(_WIN32)
#include <windows.h> // SEH only (EXCEPTION_EXECUTE_HANDLER, GetExceptionCode), see SafeUpload
#endif

#include <poly2tri/poly2tri.h>

#include <Corrade/Containers/ArrayView.h>

#include <Magnum/Mesh.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Color.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/Renderer.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>

#include <gravitaris/cgame/component/renderable.hpp>
#include <gravitaris/cgame/team-color.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>

namespace Gravitaris {

using namespace Magnum;

namespace {

// The visual group every model carries; SubmitOverlay's instances join this
// pass (see RenderTag).
const id_t OVERLAY_TAG = "model"_id;

// Must match Line2Shader's vertex layout.
struct LineVertex {
    Vector2 pointA;
    Vector2 pointB;
    Vector2 pointC;
    Vector4 param; // xyz weights, w = type (0 segment, 1 join, 2 ring, 3 poly-fill, 4 disc-fill)
    Vector3 color;
    float teamWeight; // 1 = replace color with the instance's team color
};

// Circle billboard quad, corners in [-1,1]^2. Shared by the ring stroke and
// the disc fill.
constexpr Vector2 CIRCLE_QUAD[] = {
        {-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f},
        {-1.f, -1.f}, {1.f, 1.f},  {-1.f, 1.f},
};

// param.w primitive tags for fills. Polygon fills are flat triangles whose
// pointA carries the position; disc fills reuse the circle billboard's
// analytic-radius machinery so a filled circle matches its stroke ring exactly
// at any zoom (no faceting seam against the ring).
constexpr float PRIM_POLY_FILL = 3.f;
constexpr float PRIM_DISC_FILL = 4.f;

// Filled disc, emitted exactly like the circle-ring billboard (center + radius
// carrier + [-1,1] quad corners); the shader fills the interior instead of the
// ring. Perfect circle at any zoom, coincident with the stroke ring's radius.
void EmitCircleFill(std::vector<LineVertex>& out, const Vector2& center, float radius,
                    const Vector3& color, float teamWeight)
{
    const Vector2 radiusCarrier{radius, 0.f};
    for (const Vector2& corner : CIRCLE_QUAD) {
        out.push_back(LineVertex{center, radiusCarrier, Vector2{},
                                 Vector4{corner.x(), corner.y(), 0.f, PRIM_DISC_FILL}, color, teamWeight});
    }
}

// Filled simple polygon via constrained Delaunay triangulation (poly2tri).
// `pts`/`count` are the strip's baked points, whose last point duplicates the
// first for closed paths -- dropped here. Bad input (self-intersecting/near-
// degenerate paths poly2tri chokes on) degrades to no fill rather than crashing.
void EmitPolygonFill(std::vector<LineVertex>& out, const Vector2* pts, std::size_t count,
                     const Vector3& color, float teamWeight)
{
    std::size_t n = count;
    if (n >= 2 && pts[0] == pts[n - 1]) --n;
    if (n < 3) return;

    std::vector<p2t::Point> storage;
    storage.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        storage.emplace_back(static_cast<double>(pts[i].x()), static_cast<double>(pts[i].y()));
    }

    std::vector<p2t::Point*> polyline;
    polyline.reserve(n);
    for (p2t::Point& p : storage) polyline.push_back(&p);

    const auto emitFill = [&](const Vector2& pos) {
        out.push_back(LineVertex{pos, Vector2{}, Vector2{},
                                 Vector4{0.f, 0.f, 0.f, PRIM_POLY_FILL}, color, teamWeight});
    };

    try {
        p2t::CDT cdt(polyline);
        cdt.Triangulate();
        for (p2t::Triangle* tri : cdt.GetTriangles()) {
            for (int k = 0; k < 3; ++k) {
                const p2t::Point* p = tri->GetPoint(k);
                emitFill(Vector2{static_cast<float>(p->x), static_cast<float>(p->y)});
            }
        }
    } catch (...) {
        LOG(warning) << "[MR2] polygon fill triangulation failed (" << n << " pts); skipping fill";
    }
}

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

    // Fill pass first, so fills sit underneath the strokes drawn after them
    // (single draw call: primitive order within it is the paint order). A
    // black planet fill thus blocks the starfield, its outline glows on top.
    for (const auto& strip : group.lineStrips) {
        if (!strip.filled) continue;

        const Vector3 fillColor = strip.fillColor.toSrgb();
        const float fillTeamWeight = strip.fillTeamColor ? 1.f : 0.f;

        if (strip.circle) {
            EmitCircleFill(out, strip.circle->center, strip.circle->radius, fillColor, fillTeamWeight);
        } else if (strip.count >= 3) {
            EmitPolygonFill(out, group.vertexBuffer.data() + strip.offset, strip.count,
                            fillColor, fillTeamWeight);
        }
    }

    for (const auto& strip : group.lineStrips) {
        const Vector3 color = strip.color.toSrgb();
        const float teamWeight = strip.teamColor ? 1.f : 0.f;

        if (strip.circle && !forceFaceted) {
            const Vector2& center = strip.circle->center;
            const Vector2 radiusCarrier{strip.circle->radius, 0.f};
            for (const Vector2& corner : CIRCLE_QUAD) {
                out.push_back(LineVertex{center, radiusCarrier, Vector2{},
                                          Vector4{corner.x(), corner.y(), 0.f, 2.f}, color, teamWeight});
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
                out.push_back(LineVertex{a, b, Vector2{}, Vector4{g.x(), g.y(), 0.f, 0.f}, color, teamWeight});
            }
        }

        // Joins: miter at each interior vertex (needs A, B=join, C).
        for (std::size_t i = 0; i + 2 < strip.count; ++i) {
            const Vector2 a = pts[i];
            const Vector2 b = pts[i + 1];
            const Vector2 c = pts[i + 2];
            for (const Vector3& g : MITER) {
                out.push_back(LineVertex{a, b, c, Vector4{g.x(), g.y(), g.z(), 1.f}, color, teamWeight});
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
#if defined(_WIN32)
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
                                 Line2Shader::VertexColor{},
                                 Line2Shader::TeamWeight{})
                .addVertexBufferInstanced(baked.instanceBuffer, 1, 0,
                                          Line2Shader::InstanceTransform{},
                                          Line2Shader::InstanceTeamColor{},
                                          Line2Shader::InstanceFlash{});

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
                                         Line2Shader::VertexColor{},
                                         Line2Shader::TeamWeight{})
                        .addVertexBufferInstanced(baked.instanceBuffer, 1, 0,
                                                  Line2Shader::InstanceTransform{},
                                                  Line2Shader::InstanceTeamColor{},
                                                  Line2Shader::InstanceFlash{});
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

void ModelRenderer2::SubmitOverlay(id_t modelId, const Matrix3& transform, const Vector3& color, float flash)
{
    m_overlayScratch[modelId].push_back(InstanceData{transform, color, flash});
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

        // Only placeholder-authored strokes take this color (team mask);
        // unteamed entities render those strokes white.
        const Team* team = entity.try_get<Team>();
        const Vector3 teamColor = team ? Vector3{TeamColor(team->id)} : Vector3{1.f, 1.f, 1.f};

        const Damageable* dmg = entity.try_get<Damageable>();
        const float flash = dmg ? dmg->flashAmount : 0.f;

        m_instanceScratch[modelId].push_back(InstanceData{transform, teamColor, flash});
    });

    // Overlays are plain extra instances of the same baked group, so they only
    // join the "model" pass -- they have no thruster/other tagged groups.
    if (tag == OVERLAY_TAG) {
        for (const auto& [modelId, overlays] : m_overlayScratch) {
            if (overlays.empty()) continue;
            const auto bakedIt = m_baked.find(modelId);
            if (bakedIt == m_baked.end() || !bakedIt->second.contains(tag)) continue;

            std::vector<InstanceData>& dst = m_instanceScratch[modelId];
            dst.insert(dst.end(), overlays.begin(), overlays.end());
        }
    }

    for (auto& [modelId, instances] : m_instanceScratch) {
        if (instances.empty()) continue;

        auto& baked = m_baked.at(modelId).at(tag);
        if (unsigned long ex = SafeUpload(baked.instanceBuffer, instances.data(),
                                          instances.size() * sizeof(InstanceData))) {
            LOG(error) << "[MR2] instance buffer upload raised exception 0x" << std::hex << ex
                       << " for model " << modelId << " tag " << tag;
            continue;
        }

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

    // Normalized around referenceZoom so the factor never changes the width at
    // the normal zoom level; it only sets how much thicker (zoomed in) or
    // thinner (zoomed out) the line gets. factor=0 -> constant pixel width;
    // factor=1 -> constant world-space width (pixel width tracks zoom 1:1).
    const float refZoom = m_referenceZoom > 0.f ? m_referenceZoom : 1.f;
    const float zoomScale = std::pow(m_zoom / refZoom, m_zoomWidthFactor);
    m_shader.setViewportSize(m_viewportSize)
            .setViewProjection(ViewProjection())
            .setWidth(m_lineWidthPixels * m_pixelScale * zoomScale);

    RenderTag(OVERLAY_TAG, {});

    RenderTag("_thrust"_id, [](flecs::entity entity) {
        const auto* controls = entity.try_get<Controls>();
        return controls && controls->actionFlags.thrustForward;
    });

    // Submissions are per-frame: whoever wants an overlay next frame re-submits.
    for (auto& [modelId, overlays] : m_overlayScratch) overlays.clear();
}

} // namespace Gravitaris
