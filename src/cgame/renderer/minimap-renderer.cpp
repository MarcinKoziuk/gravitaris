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
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/resource/body.hpp>

#include <gravitaris/game/logging.hpp>

#include <gravitaris/cgame/team-color.hpp>
#include <gravitaris/cgame/renderer/gl-safe-upload.hpp>
#include <gravitaris/cgame/renderer/minimap-renderer.hpp>

namespace Gravitaris {

using namespace Magnum;

namespace {

// Line thickness inside the minimap texture, px.
constexpr float MAP_LINE_WIDTH = 1.5f;

// Opaque near-black backdrop (the UI theme's #01141a). Opaque on purpose:
// with alpha 1 everywhere, straight-vs-premultiplied blending in the RmlUi
// pass can't tint the panel.
constexpr Color4 BACKGROUND{0.004f, 0.078f, 0.102f, 1.f};

// Match the world assets: yellow suns (data/models/stars/sun), green planets
// (data/models/planets/simple).
const Vector3 SUN_COLOR{1.f, 0.87f, 0.13f};
const Vector3 PLANET_COLOR{0.2f, 1.f, 0.2f};
const Vector3 VIEW_RECT_COLOR{0.12f, 0.42f, 0.48f};
const Vector3 PLAYER_COLOR{1.f, 1.f, 1.f};

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

MinimapRenderer::MinimapRenderer(flecs::world& registry, PhysicsSystem& physicsSystem, IFilesystem& filesystem)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_shader(filesystem)
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

void MinimapRenderer::Render(const Vector2& center, const Vector2& viewCenter, const Vector2& viewHalfExtent)
{
    m_framebuffer.setViewport({{}, TextureSize()})
                 .clearColor(0, BACKGROUND)
                 .bind();

    if (!m_params.enabled) return; // blank panel

    const float worldRadius = std::max(m_params.worldRadius, 1.f);
    // Minimap px per world unit; fixed-px icon sizes convert through this.
    const float ppu = (TEXTURE_SIZE * 0.5f) / worldRadius;

    std::vector<LineVertex> vertices;

    // Planets/suns: rings at their true world radius (with a visibility
    // floor). A sun has no Orbit (it sits still); an orbiting planet does --
    // that also picks the floor and color to match the world assets.
    m_registry.each([&](flecs::entity entity, const Transform& t, const PhysicsRef& ref) {
        if (!entity.has<Planet>()) return;
        const bool isStar = !entity.has<Orbit>();
        const Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};

        float radius = (isStar ? m_params.starMinPx : m_params.planetMinPx) / ppu;
        const PhysicsBody& body = m_physicsSystem.GetBody(ref);
        if (body.body && !body.body->GetCircleShapes().empty()) {
            radius = std::max(radius, static_cast<float>(body.body->GetCircleShapes().front().radius)
                                              * static_cast<float>(t.scale.x()));
        }

        if ((pos - center).length() - radius > worldRadius) return;
        EmitBillboard(vertices, pos, radius, isStar ? SUN_COLOR : PLANET_COLOR, PRIM_RING);
    });

    // Ships: team-colored dots (same "enemy/ship" notion as the HUD arrows:
    // damageable + real team; bullets/shrapnel have neither).
    m_registry.each([&](flecs::entity, const Transform& t, const Team& team, const Damageable&) {
        if (team.id == TeamId::None) return;
        const Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        if ((pos - center).length() > worldRadius) return;
        EmitBillboard(vertices, pos, m_params.shipDotPx / ppu, Vector3{TeamColor(team.id)}, PRIM_DISC);
    });

    // Player marker: brighter, ringed, drawn on top of the team dots.
    EmitBillboard(vertices, center, m_params.playerDotPx / ppu, PLAYER_COLOR, PRIM_DISC);
    EmitBillboard(vertices, center, (m_params.playerDotPx + 3.f) / ppu, PLAYER_COLOR, PRIM_RING);

    if (m_params.showViewRect) {
        EmitRect(vertices, viewCenter, viewHalfExtent, VIEW_RECT_COLOR);
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
            * Matrix3::translation(-center);

    m_shader.setViewportSize(Vector2{TextureSize()})
            .setViewProjection(viewProjection)
            .setWidth(MAP_LINE_WIDTH);

    m_shader.draw(m_mesh);
}

} // namespace Gravitaris
