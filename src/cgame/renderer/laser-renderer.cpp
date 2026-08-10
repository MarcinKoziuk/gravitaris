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

// How solid a beam is where it leaves the mount. Short of opaque on purpose:
// it is light, and the hull behind it should still read through the wedge.
static constexpr float BEAM_NEAR_ALPHA = 0.85f;

// Floors on how thin a beam may get ON SCREEN, in framebuffer pixels. The
// authored widths are world units, and world units vanish as the camera pulls
// back: at cruise zoom the wedge was under a pixel across at the muzzle and
// faded to nothing before it got wide enough to see, which read as the weapon
// not drawing at all. Every other line in this renderer is sized in pixels for
// the same reason (see ModelRenderer2's own line width) -- a vector display
// draws a beam a beam's width, not a beam's distance.
static constexpr float MIN_NEAR_PIXELS = 2.f;
static constexpr float MIN_FAR_PIXELS = 9.f;

LaserRenderer::LaserRenderer(IFilesystem& filesystem)
        : m_shader(filesystem)
{
    m_mesh.setPrimitive(Magnum::MeshPrimitive::Triangles)
          .addVertexBuffer(m_buffer, 0, LaserShader::Position{}, LaserShader::Color{});
}

void LaserRenderer::Render(const std::vector<Beam>& beams)
{
    if (beams.empty()) return;

    m_vertices.clear();
    m_vertices.reserve(beams.size() * 6);

    const float pixelsPerUnit = std::max(m_zoom * m_contentScale, 1e-6f);
    const float minNear = MIN_NEAR_PIXELS / pixelsPerUnit;
    const float minFar = MIN_FAR_PIXELS / pixelsPerUnit;

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

        const Color4 hot{beam.color, BEAM_NEAR_ALPHA};
        const Color4 cold{beam.color, BEAM_NEAR_ALPHA * std::clamp(beam.endStrength, 0.f, 1.f)};

        const Vertex a{beam.from - nearOffset, hot};
        const Vertex b{beam.from + nearOffset, hot};
        const Vertex c{beam.to + farOffset, cold};
        const Vertex d{beam.to - farOffset, cold};

        m_vertices.insert(m_vertices.end(), {a, b, c, a, c, d});
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
