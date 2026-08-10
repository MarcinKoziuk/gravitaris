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

    for (const Beam& beam : beams) {
        const Vector2 along = beam.to - beam.from;
        const float length = along.length();
        if (length < 1e-4f) continue;

        // The wedge: half a width either side of the line, widening as it
        // goes. Both edges fade to nothing at the far end, so the shape and
        // the falloff are the same picture -- which is the whole reason the
        // weapon is drawn this way rather than as a line.
        const Vector2 normal = Vector2{-along.y(), along.x()} / length;
        const Vector2 nearOffset = normal * (beam.widthNear * 0.5f);
        const Vector2 farOffset = normal * (beam.widthFar * 0.5f);

        const Color4 hot{beam.color, BEAM_NEAR_ALPHA};
        const Color4 cold{beam.color, 0.f};

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

    const float pixelsPerUnit = m_zoom * m_contentScale;
    const Vector2 extent = m_viewportSize / (pixelsPerUnit > 0.f ? pixelsPerUnit : 1.f);
    m_shader.setTransformationProjectionMatrix(Matrix3::projection(extent)
                                               * Matrix3::translation(-m_cameraPos));
    m_shader.draw(m_mesh);

    Magnum::GL::Renderer::setBlendFunction(Magnum::GL::Renderer::BlendFunction::SourceAlpha,
                                           Magnum::GL::Renderer::BlendFunction::OneMinusSourceAlpha);
}

} // namespace Gravitaris
