#include <algorithm>
#include <vector>

#include <Magnum/Mesh.h>
#include <Magnum/Math/Color.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/GL/TextureFormat.h>

#include <gravitaris/game/logging.hpp>

#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/renderer/gl-safe-upload.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>
#include <gravitaris/cgame/renderer/compass-renderer.hpp>

#include "detail/line2-batch.hpp"

namespace Gravitaris {

using namespace Magnum;

using Line2Batch::LineVertex;
using Line2Batch::InstanceData;
using Line2Batch::EmitBillboard;
using Line2Batch::EmitFillTriangle;
using Line2Batch::PRIM_RING;

namespace {

// Opaque, for the same reason MinimapRenderer's is: with alpha 1 everywhere,
// straight-vs-premultiplied blending in the RmlUi pass can't tint the panel.
constexpr Color4 BACKGROUND{0.f, 0.f, 0.f, 1.f};

// The gauge is drawn in a -1..1 square with +Y matching world +Y, so the ship
// sits at the same angle it does in the main view.
constexpr float RING_RADIUS = 0.80f;
// Markers are wedges standing on the ring and pointing out of it, filled
// rather than outlined: at this size on screen a hollow one is a smudge.
constexpr float MARKER_APEX = 1.f;
constexpr float MARKER_HALF_WIDTH = 0.15f;

// The ring is chrome, not data: div.frame's quiet hairline, in the sidebar's
// terms, leaving the ship as the only bright thing in the circle.
const Vector3 RING_COLOR{0.f, 0.45f, 0.6f};
// The theme's cyan (#0cf), the same accent div.corner's brackets carry.
const Vector3 VELOCITY_COLOR{0.f, 0.8f, 1.f};
const Vector3 GRAVITY_COLOR{1.f, 0.35f, 0.25f};

// How far the model reaches from its own origin, which is what the fit scales
// against. Circle strips carry their extent in the hint rather than in the
// (redundant, for them) polyline -- see Model::CircleHint.
float BoundingRadius(const Model& model)
{
    float reach = 0.f;
    for (const auto& [tag, group] : model.GetModelGroups()) {
        for (const Model::VertexLineStrip& strip : group.lineStrips) {
            if (strip.circle) {
                reach = std::max(reach, strip.circle->center.length() + strip.circle->radius);
                continue;
            }
            for (std::size_t i = 0; i < strip.count; ++i) {
                reach = std::max(reach, group.vertexBuffer[strip.offset + i].length());
            }
        }
    }
    return reach;
}

} // namespace

CompassRenderer::CompassRenderer(IFilesystem& filesystem, ModelRenderer2& modelRenderer, float contentScale)
        : m_modelRenderer(modelRenderer)
        , m_textureSize(TextureSizeFor(contentScale))
        , m_contentScale(static_cast<float>(m_textureSize.x()) / TEXTURE_SIZE)
        , m_shader(filesystem)
        , m_framebuffer({{}, m_textureSize})
{
    m_texture.setStorage(1, GL::TextureFormat::RGBA8, TextureSize())
             .setMinificationFilter(GL::SamplerFilter::Linear)
             .setMagnificationFilter(GL::SamplerFilter::Linear)
             .setWrapping(GL::SamplerWrapping::ClampToEdge);
    m_framebuffer.attachTexture(GL::Framebuffer::ColorAttachment{0}, m_texture, 0);

    const InstanceData identity{Matrix3{Math::IdentityInit}, Vector3{1.f, 1.f, 1.f}, 0.f};
    if (unsigned long ex = SafeUpload(m_instanceBuffer, &identity, sizeof(identity))) {
        LOG(error) << "[Compass] instance buffer upload raised exception 0x" << std::hex << ex;
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

float CompassRenderer::FitScale(const Model& model, id_t modelId)
{
    if (modelId != m_fittedModelId) {
        m_fittedModelId = modelId;
        m_fittedModelRadius = BoundingRadius(model);
    }
    return m_params.shipFit / std::max(m_fittedModelRadius, 1e-3f);
}

void CompassRenderer::Render(const Subject& subject)
{
    m_framebuffer.setViewport({{}, TextureSize()})
                 .clearColor(0, BACKGROUND)
                 .bind();

    if (!m_params.enabled) return; // blank panel

    // Y is negated for the same reason the minimap's projection is: an FBO's
    // row 0 is the GL bottom, but RmlUi shows row 0 at the top of the <img>.
    // The two flips cancel, so the ship reads the same way round as in the
    // main view -- which is what lets it be drawn at its plain world rotation.
    const Matrix3 viewProjection = Matrix3::projection({2.f, -2.f});

    std::vector<LineVertex> vertices;
    EmitBillboard(vertices, Vector2{}, RING_RADIUS, RING_COLOR, PRIM_RING);

    const auto emitMarker = [&vertices](const Vector2& direction, const Vector3& color) {
        const Vector2 side{direction.y(), -direction.x()};
        EmitFillTriangle(vertices, direction * MARKER_APEX,
                         direction * RING_RADIUS - side * MARKER_HALF_WIDTH,
                         direction * RING_RADIUS + side * MARKER_HALF_WIDTH, color);
    };
    if (m_params.showGravity && subject.gravity.length() >= m_params.minGravity) {
        emitMarker(subject.gravity.normalized(), GRAVITY_COLOR);
    }
    if (m_params.showVelocity && subject.velocity.length() >= m_params.minSpeed) {
        emitMarker(subject.velocity.normalized(), VELOCITY_COLOR);
    }

    if (unsigned long ex = SafeUpload(m_vertexBuffer, vertices.data(), vertices.size() * sizeof(LineVertex))) {
        LOG(error) << "[Compass] vertex buffer upload raised exception 0x" << std::hex << ex;
        return;
    }
    m_mesh.setCount(static_cast<Int>(vertices.size()));

    GL::Renderer::enable(GL::Renderer::Feature::Blending);
    GL::Renderer::setBlendFunction(GL::Renderer::BlendFunction::SourceAlpha,
                                    GL::Renderer::BlendFunction::OneMinusSourceAlpha);

    m_shader.setViewportSize(Vector2{TextureSize()})
            .setViewProjection(viewProjection)
            .setWidth(m_params.lineWidth * m_contentScale);

    m_shader.draw(m_mesh);

    if (!subject.model) return;

    const float scale = FitScale(*subject.model, subject.modelId);
    const Matrix3 transform = Matrix3::rotation(subject.rot) * Matrix3::scaling({scale, scale});
    m_modelRenderer.RenderStandalone(subject.modelId, transform, viewProjection, Vector2{TextureSize()},
                                     m_params.lineWidth * m_contentScale, subject.teamColor);
}

} // namespace Gravitaris
