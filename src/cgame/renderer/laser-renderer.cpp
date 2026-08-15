#include <algorithm>

#include <Corrade/Containers/ArrayView.h>

#include <Magnum/Mesh.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/Math/Matrix3.h>

#include <gravitaris/cgame/renderer/laser-renderer.hpp>

namespace Gravitaris {

using Magnum::Color4;
using Magnum::Matrix3;
using Magnum::Vector2;

// Floors on how thin a beam may get ON SCREEN, in framebuffer pixels. The
// authored widths are world units, and world units vanish as the camera pulls
// back: at cruise zoom the wedge was under a pixel across at the muzzle and
// faded to nothing before it got wide enough to see, which read as the weapon
// not drawing at all. Every other line in this renderer is sized in pixels for
// the same reason (see ModelRenderer2's own line width) -- a vector display
// draws a beam a beam's width, not a beam's distance.
//
// A hair over a pixel at the muzzle is the real floor here: this shader has no
// analytic edge AA (unlike line2.f.glsl), so a wedge allowed below one pixel
// rasterizes as a dotted line rather than a thin one.
static constexpr float MIN_NEAR_PIXELS = 1.f;
static constexpr float MIN_FAR_PIXELS = 4.5f;

// The same floor for a charge's radius: a mount lighting up is worth seeing
// from wherever the camera happens to be sitting.
static constexpr float MIN_CHARGE_PIXELS = 2.f;

// How spent a charge's light is at the rim of its disc, in the fade lengths
// laser.f.glsl reads from the alpha channel. Around three is where the
// exponential has visibly nothing left, so the edge is soft rather than a cut.
static constexpr float CHARGE_RIM_FADE = 3.f;

LaserRenderer::LaserRenderer(IFilesystem& filesystem)
        : m_shader(filesystem)
{
    m_mesh.setPrimitive(Magnum::MeshPrimitive::Triangles)
          .addVertexBuffer(m_buffer, 0, LaserShader::Position{}, LaserShader::Color{});
}

void LaserRenderer::Render(const std::vector<Beam>& beams, const std::vector<Charge>& charges)
{
    if (beams.empty() && charges.empty()) return;

    m_vertices.clear();
    m_vertices.reserve(beams.size() * 6 + charges.size() * CHARGE_SEGMENTS * 3);

    const float pixelsPerUnit = std::max(m_zoom * m_contentScale, 1e-6f);
    const float minNear = MIN_NEAR_PIXELS / pixelsPerUnit;
    const float minFar = MIN_FAR_PIXELS / pixelsPerUnit;
    const float minCharge = MIN_CHARGE_PIXELS / pixelsPerUnit;

    for (const Beam& beam : beams) {
        const Vector2 along = beam.to - beam.from;
        const float length = along.length();
        if (length < 1e-4f) continue;

        // The wedge: half a width either side of the line, widening as it
        // goes. Both edges fade to nothing at the far end, so the shape and
        // the falloff are the same picture -- which is the whole reason the
        // weapon is drawn this way rather than as a line.
        const Vector2 normal = Vector2{-along.y(), along.x()} / length;
        const Vector2 nearOffset = normal * (std::max(beam.widthNear, minNear) * 0.5f);
        const Vector2 farOffset = normal * (std::max(beam.widthFar, minFar) * 0.5f);

        // The alpha channel is not an opacity: it carries how far this corner
        // sits from the mount, in fade lengths, and the fragment shader is
        // what turns that into one. Distance is linear along the wedge, so two
        // corners describe it exactly -- which a curve fitted through vertex
        // opacities could not.
        const Color4 hot{beam.color, beam.fadeStart};
        const Color4 cold{beam.color,
                          beam.fadeStart + length / std::max(beam.fadeLength, 1e-3f)};

        const Vertex a{beam.from - nearOffset, hot};
        const Vertex b{beam.from + nearOffset, hot};
        const Vertex c{beam.to + farOffset, cold};
        const Vertex d{beam.to - farOffset, cold};

        // Counter-clockwise, whichever way the beam points: a wedge is built
        // off one side of its own line, so the naive corner order is wound the
        // same (wrong) way every time and a culling pass would eat every beam.
        m_vertices.insert(m_vertices.end(), {a, c, b, a, d, c});
    }

    for (const Charge& charge : charges) {
        const float radius = std::max(charge.radius, minCharge);
        // Solid at the middle and spent at the rim, through the same falloff
        // the length of a beam uses -- so a charge is lit like the light it is
        // rather than drawn as a disc with an edge.
        const Color4 core{charge.color, 0.f};
        const Color4 rim{charge.color, CHARGE_RIM_FADE};

        Vector2 previous{radius, 0.f};
        for (int i = 1; i <= CHARGE_SEGMENTS; ++i) {
            const auto angle = Magnum::Rad{2.f * Magnum::Constants::pi()
                                           * static_cast<float>(i) / CHARGE_SEGMENTS};
            const Vector2 next{radius * Magnum::Math::cos(angle), radius * Magnum::Math::sin(angle)};
            m_vertices.insert(m_vertices.end(), {Vertex{charge.at, core},
                                                 Vertex{charge.at + previous, rim},
                                                 Vertex{charge.at + next, rim}});
            previous = next;
        }
    }

    if (m_vertices.empty()) return;

    m_buffer.setData(Corrade::Containers::arrayView(m_vertices.data(), m_vertices.size()),
                     Magnum::GL::BufferUsage::DynamicDraw);
    m_mesh.setCount(static_cast<Magnum::Int>(m_vertices.size()));

    // Additive, and depth-free: beams are light. Two of them crossing should
    // read brighter where they meet rather than one occluding the other.
    Magnum::GL::Renderer::enable(Magnum::GL::Renderer::Feature::Blending);
    Magnum::GL::Renderer::setBlendFunction(Magnum::GL::Renderer::BlendFunction::One,
                                           Magnum::GL::Renderer::BlendFunction::One);

    const Vector2 extent = m_viewportSize / pixelsPerUnit;
    m_shader.setTransformationProjectionMatrix(Matrix3::projection(extent)
                                               * Matrix3::translation(-m_cameraPos));
    m_shader.draw(m_mesh);

    Magnum::GL::Renderer::setBlendFunction(Magnum::GL::Renderer::BlendFunction::SourceAlpha,
                                           Magnum::GL::Renderer::BlendFunction::OneMinusSourceAlpha);
}

} // namespace Gravitaris
