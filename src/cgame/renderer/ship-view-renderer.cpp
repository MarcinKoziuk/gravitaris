#include <Magnum/Math/Angle.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Math/Functions.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/GL/TextureFormat.h>

#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>
#include <gravitaris/cgame/renderer/ship-view-renderer.hpp>

namespace Gravitaris {

using namespace Magnum;

namespace {

// Opaque, for the same reason MinimapRenderer's is: with alpha 1 everywhere,
// straight-vs-premultiplied blending in the RmlUi pass can't tint the panel.
constexpr Color4 BACKGROUND{0.f, 0.f, 0.f, 1.f};

// The schematic is authored entirely in TEAM_COLOR_PLACEHOLDER, so the whole
// drawing takes this. The board's accent (#56d9f2) rather than a team color:
// this is a diagram of the hull, not a sighting of it.
const Vector3 LINE_COLOR{0.337f, 0.851f, 0.949f};

Range2D GroupBounds(const Model& model, id_t tag)
{
    const auto& groups = model.GetModelGroups();
    const auto it = groups.find(tag);
    if (it == groups.end()) return Range2D{Vector2{-1.f}, Vector2{1.f}};

    bool any = false;
    Vector2 min{0.f};
    Vector2 max{0.f};
    const auto extend = [&](const Vector2& lo, const Vector2& hi) {
        if (!any) {
            min = lo;
            max = hi;
            any = true;
            return;
        }
        min = Math::min(min, lo);
        max = Math::max(max, hi);
    };

    for (const Model::VertexLineStrip& strip : it->second.lineStrips) {
        // Circle strips carry their extent in the hint rather than in the
        // (redundant, for them) polyline -- see Model::CircleHint.
        if (strip.circle) {
            const Vector2 r{strip.circle->radius};
            extend(strip.circle->center - r, strip.circle->center + r);
            continue;
        }
        for (std::size_t i = 0; i < strip.count; ++i) {
            const Vector2& v = it->second.vertexBuffer[strip.offset + i];
            extend(v, v);
        }
    }

    if (!any) return Range2D{Vector2{-1.f}, Vector2{1.f}};
    return Range2D{min, max};
}

} // namespace

ShipViewRenderer::ShipViewRenderer(ModelRenderer2& modelRenderer, float contentScale)
        : m_modelRenderer(modelRenderer)
        , m_textureSize(TextureSizeFor(contentScale))
        , m_contentScale(static_cast<float>(m_textureSize.x()) / TEXTURE_SIZE)
        , m_framebuffer({{}, m_textureSize})
{
    m_texture.setStorage(1, GL::TextureFormat::RGBA8, TextureSize())
             .setMinificationFilter(GL::SamplerFilter::Linear)
             .setMagnificationFilter(GL::SamplerFilter::Linear)
             .setWrapping(GL::SamplerWrapping::ClampToEdge);
    m_framebuffer.attachTexture(GL::Framebuffer::ColorAttachment{0}, m_texture, 0);
}

Matrix3 ShipViewRenderer::FitTransform(const Model& model, id_t modelId)
{
    if (modelId != m_fittedModelId) {
        m_fittedModelId = modelId;
        m_fittedBounds = GroupBounds(model, ID(DETAIL_GROUP));
    }

    // The schematic is drawn nose-up, the way the hull flies; the board shows
    // it in profile, nose to the right. Turning it here rather than in the
    // asset keeps the drawing's own space the one the slots are authored in.
    const Matrix3 turn = Matrix3::rotation(Magnum::Deg{90.f});

    // Post-turn half-extents: the rotation swaps them.
    const Vector2 half = m_fittedBounds.size() * 0.5f;
    const Vector2 reach = Magnum::Math::max(Vector2{half.y(), half.x()}, Vector2{1e-3f});

    // Whichever axis runs out of panel first, the hull lying across a wide
    // panel being the height.
    const float scale = m_params.fit * std::min(ASPECT / reach.x(), 1.f / reach.y());

    return Matrix3::scaling({scale, scale}) * turn * Matrix3::translation(-m_fittedBounds.center());
}

Vector2 ShipViewRenderer::PanelUV(const Model& model, id_t modelId, const Vector2& pos)
{
    // The fit lands the drawing in +-ASPECT by +-1; the projection's Y flip
    // and RmlUi's own cancel, so panel-down is fitted-down and only the range
    // changes here.
    const Vector2 fitted = FitTransform(model, modelId).transformPoint(pos);
    return Vector2{fitted.x() / ASPECT + 1.f, fitted.y() + 1.f} * 0.5f;
}

void ShipViewRenderer::Render(const Subject& subject)
{
    m_framebuffer.setViewport({{}, TextureSize()})
                 .clearColor(0, BACKGROUND)
                 .bind();

    if (!m_params.enabled || !subject.model) return; // blank panel

    // Y is negated for the same reason the compass dial's is: an FBO's row 0
    // is the GL bottom, but RmlUi shows row 0 at the top of the <img>. The
    // width carries the aspect so a panel wider than it is tall doesn't
    // stretch the hull across it.
    const Matrix3 viewProjection = Matrix3::projection({2.f * ASPECT, -2.f});

    GL::Renderer::enable(GL::Renderer::Feature::Blending);
    GL::Renderer::setBlendFunction(GL::Renderer::BlendFunction::SourceAlpha,
                                    GL::Renderer::BlendFunction::OneMinusSourceAlpha);

    m_modelRenderer.RenderStandalone(subject.modelId, FitTransform(*subject.model, subject.modelId),
                                     viewProjection, Vector2{TextureSize()},
                                     m_params.lineWidth * m_contentScale, LINE_COLOR,
                                     ID(DETAIL_GROUP));
}

} // namespace Gravitaris
