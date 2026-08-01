#include <vector>

#include <Corrade/Containers/ArrayView.h>

#include <Magnum/Mesh.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Color.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/GL/TextureFormat.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/freighter.hpp>

#include <gravitaris/game/logging.hpp>

#include <gravitaris/cgame/team-color.hpp>
#include <gravitaris/cgame/renderer/gl-safe-upload.hpp>
#include <gravitaris/cgame/renderer/minimap-renderer.hpp>

namespace Gravitaris {

using namespace Magnum;

namespace {

// Line thickness inside the minimap texture, px.
constexpr float MAP_LINE_WIDTH = 1.5f;

// Black backdrop, so the map reads as a framed window onto space rather than a
// tinted panel. Opaque on purpose: with alpha 1 everywhere, straight-vs-
// premultiplied blending in the RmlUi pass can't tint it.
constexpr Color4 BACKGROUND{0.f, 0.f, 0.f, 1.f};

// Match the world assets: yellow suns (data/models/stars/sun), green planets
// (data/models/planets/simple).
const Vector3 SUN_COLOR{1.f, 0.87f, 0.13f};
const Vector3 PLANET_COLOR{0.2f, 1.f, 0.2f};
// The camera-extent box: an unfilled dark-gray outline, dim enough not to
// compete with the icons it encloses.
const Vector3 VIEW_RECT_COLOR{0.28f, 0.31f, 0.34f};
const Vector3 PLAYER_COLOR{1.f, 1.f, 1.f};
// Freighters are drawn in one muted color whoever owns them: the original's
// "commerce is neutral" rule (docs/gravity-well-1997.md), and it keeps the
// map's team colors meaning "combat" at a glance.
const Vector3 FREIGHTER_COLOR{0.45f, 0.5f, 0.55f};

// Must match Line2Shader's vertex layout (same contract as ModelRenderer2's
// baked geometry; the shader doesn't care that this one is rebuilt per frame).
struct LineVertex {
    Vector2 pointA;
    Vector2 pointB;
    Vector2 pointC;
    Vector4 param; // xyz weights, w = type (0 segment, 2 ring, 4 disc fill)
    Vector3 color;
    float teamWeight;
};

// Matches Line2Shader's per-instance layout; the minimap draws exactly one
// identity instance (colors are baked per vertex instead).
struct InstanceData {
    Matrix3 transform;
    Vector3 teamColor;
    float flash;
};

constexpr float PRIM_SEGMENT = 0.f;
constexpr float PRIM_RING = 2.f;
constexpr float PRIM_DISC = 4.f;

constexpr Vector2 CIRCLE_QUAD[] = {
        {-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f},
        {-1.f, -1.f}, {1.f, 1.f},  {-1.f, 1.f},
};

constexpr Vector2 SEGMENT_WEIGHTS[] = {
        {0.f, -0.5f}, {1.f, -0.5f}, {1.f, 0.5f},
        {0.f, -0.5f}, {1.f, 0.5f},  {0.f, 0.5f},
};

void EmitBillboard(std::vector<LineVertex>& out, const Vector2& center, float radius,
                   const Vector3& color, float prim)
{
    const Vector2 radiusCarrier{radius, 0.f};
    for (const Vector2& corner : CIRCLE_QUAD) {
        out.push_back(LineVertex{center, radiusCarrier, Vector2{},
                                 Vector4{corner.x(), corner.y(), 0.f, prim}, color, 0.f});
    }
}

void EmitSegment(std::vector<LineVertex>& out, const Vector2& a, const Vector2& b, const Vector3& color)
{
    for (const Vector2& w : SEGMENT_WEIGHTS) {
        out.push_back(LineVertex{a, b, Vector2{}, Vector4{w.x(), w.y(), 0.f, PRIM_SEGMENT}, color, 0.f});
    }
}

// A point-down outline triangle of circumradius `radius`. Built from three
// segments rather than a new shader primitive: Line2Shader only knows
// segments, rings and discs, and a hollow triangle is three segments.
void EmitTriangle(std::vector<LineVertex>& out, const Vector2& center, float radius, const Vector3& color)
{
    // cos30/sin30 for the two upper corners; the apex hangs straight down.
    const Vector2 apex = center + Vector2{0.f, -radius};
    const Vector2 left = center + Vector2{-0.866f * radius, 0.5f * radius};
    const Vector2 right = center + Vector2{0.866f * radius, 0.5f * radius};

    EmitSegment(out, left, right, color);
    EmitSegment(out, right, apex, color);
    EmitSegment(out, apex, left, color);
}

void EmitRect(std::vector<LineVertex>& out, const Vector2& center, const Vector2& halfExtent,
              const Vector3& color)
{
    const Vector2 tl = center + Vector2{-halfExtent.x(), halfExtent.y()};
    const Vector2 tr = center + halfExtent;
    const Vector2 br = center + Vector2{halfExtent.x(), -halfExtent.y()};
    const Vector2 bl = center - halfExtent;
    EmitSegment(out, tl, tr, color);
    EmitSegment(out, tr, br, color);
    EmitSegment(out, br, bl, color);
    EmitSegment(out, bl, tl, color);
}

} // namespace

MinimapRenderer::MinimapRenderer(IFilesystem& filesystem)
        : m_shader(filesystem)
        , m_framebuffer({{}, TextureSize()})
{
    m_texture.setStorage(1, GL::TextureFormat::RGBA8, TextureSize())
             .setMinificationFilter(GL::SamplerFilter::Linear)
             .setMagnificationFilter(GL::SamplerFilter::Linear)
             .setWrapping(GL::SamplerWrapping::ClampToEdge);
    m_framebuffer.attachTexture(GL::Framebuffer::ColorAttachment{0}, m_texture, 0);

    // Single identity instance, uploaded once: per-icon colors live in the
    // vertex stream, so the instanced attributes are just the shader contract.
    const InstanceData identity{Matrix3{Math::IdentityInit}, Vector3{1.f, 1.f, 1.f}, 0.f};
    if (unsigned long ex = SafeUpload(m_instanceBuffer, &identity, sizeof(identity))) {
        LOG(error) << "[Minimap] instance buffer upload raised exception 0x" << std::hex << ex;
    }

    m_mesh.setPrimitive(MeshPrimitive::Triangles)
          .addVertexBuffer(m_vertexBuffer, 0,
                           Line2Shader::PointA{},
                           Line2Shader::PointB{},
                           Line2Shader::PointC{},
                           Line2Shader::Param{},
                           Line2Shader::VertexColor{},
                           Line2Shader::TeamWeight{})
          .addVertexBufferInstanced(m_instanceBuffer, 1, 0,
                                    Line2Shader::InstanceTransform{},
                                    Line2Shader::InstanceTeamColor{},
                                    Line2Shader::InstanceFlash{})
          .setInstanceCount(1);
}

void MinimapRenderer::FitToSector(const SceneView& view, const Vector2& mapCenter)
{
    // A little air so the outermost orbit isn't drawn flush against the
    // panel edge.
    static constexpr float MARGIN = 1.08f;

    float reach = 0.f;
    view.Each([&](flecs::entity entity, const Transform& t, const Planet& planet) {
        const float bodyRadius = planet.radius * static_cast<float>(t.scale.x());

        // An orbiting body's furthest reach is its orbit's, not wherever it
        // happens to be this tick -- otherwise the fit would pulse as the
        // planet swings toward and away from the map center.
        if (const Orbit* orbit = entity.try_get<Orbit>()) {
            const Vector2 center{static_cast<float>(orbit->center.x()), static_cast<float>(orbit->center.y())};
            reach = std::max(reach, (center - mapCenter).length() + static_cast<float>(orbit->radius) + bodyRadius);
        }
        else {
            const Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
            reach = std::max(reach, (pos - mapCenter).length() + bodyRadius);
        }
    });

    m_params.worldRadius = std::max(m_params.worldRadius, reach * MARGIN);
}

void MinimapRenderer::Render(const SceneView& view, const Vector2& mapCenter, std::optional<Vector2> subjectPos,
                             const Vector2& viewCenter, const Vector2& viewHalfExtent)
{
    m_framebuffer.setViewport({{}, TextureSize()})
                 .clearColor(0, BACKGROUND)
                 .bind();

    if (!m_params.enabled) return; // blank panel

    if (m_params.autoFit) FitToSector(view, mapCenter);

    const float worldRadius = std::max(m_params.worldRadius, 1.f);
    // Minimap px per world unit; fixed-px icon sizes convert through this.
    const float ppu = (TEXTURE_SIZE * 0.5f) / worldRadius;

    std::vector<LineVertex> vertices;

    // Planets/suns: rings at their true world radius (with a visibility
    // floor). A sun has no Orbit (it sits still); an orbiting planet does --
    // that also picks the floor and color to match the world assets. Radius
    // comes straight off the replicated Planet component (see its own doc
    // comment) -- no PhysicsSystem/PhysicsRef needed, so the same query works
    // against every world in the view, mirror world included.
    const auto considerPlanet = [&](flecs::entity entity, const Transform& t, const Planet& planet) {
        const bool isStar = !entity.has<Orbit>();
        const Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};

        const float floorRadius = (isStar ? m_params.starMinPx : m_params.planetMinPx) / ppu;
        const float radius = std::max(floorRadius, planet.radius * static_cast<float>(t.scale.x()));

        if ((pos - mapCenter).length() - radius > worldRadius) return;
        EmitBillboard(vertices, pos, radius, isStar ? SUN_COLOR : PLANET_COLOR, PRIM_RING);
    };
    view.Each(considerPlanet);

    // Ships: team-colored dots (same "enemy/ship" notion as the HUD arrows:
    // damageable + real team; bullets/shrapnel have neither). Same
    // both-worlds sweep as planets above.
    const auto considerShip = [&](flecs::entity entity, const Transform& t, const Team& team, const Damageable&) {
        if (team.id == TeamId::None) return;
        const Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        if ((pos - mapCenter).length() > worldRadius) return;

        const bool freighter = entity.has<Freighter>();
        const float radius = (freighter ? m_params.freighterTriPx : m_params.shipTriPx) / ppu;
        EmitTriangle(vertices, pos, radius, freighter ? FREIGHTER_COLOR : Vector3{TeamColor(team.id)});
    };
    view.Each(considerShip);

    if (m_params.showViewRect) {
        EmitRect(vertices, viewCenter, viewHalfExtent, VIEW_RECT_COLOR);
    }

    // Subject marker: the same triangle as any other ship, in white and
    // ringed, drawn on top. Absent during round setup, when there is no
    // subject to mark.
    if (subjectPos) {
        EmitTriangle(vertices, *subjectPos, m_params.shipTriPx / ppu, PLAYER_COLOR);
        EmitBillboard(vertices, *subjectPos, (m_params.playerDotPx + 4.f) / ppu, PLAYER_COLOR, PRIM_RING);
    }

    if (vertices.empty()) return;

    if (unsigned long ex = SafeUpload(m_vertexBuffer, vertices.data(), vertices.size() * sizeof(LineVertex))) {
        LOG(error) << "[Minimap] vertex buffer upload raised exception 0x" << std::hex << ex;
        return;
    }
    m_mesh.setCount(static_cast<Int>(vertices.size()));

    GL::Renderer::enable(GL::Renderer::Feature::Blending);
    GL::Renderer::setBlendFunction(GL::Renderer::BlendFunction::SourceAlpha,
                                    GL::Renderer::BlendFunction::OneMinusSourceAlpha);

    // Y is negated: an FBO's row 0 is the GL bottom, but RmlUi displays row 0
    // at the top of the <img>. Flipping the projection here makes the panel's
    // orientation match the main view's.
    const Matrix3 viewProjection =
            Matrix3::projection({2.f * worldRadius, -2.f * worldRadius})
            * Matrix3::translation(-mapCenter);

    m_shader.setViewportSize(Vector2{TextureSize()})
            .setViewProjection(viewProjection)
            .setWidth(MAP_LINE_WIDTH);

    m_shader.draw(m_mesh);
}

} // namespace Gravitaris
