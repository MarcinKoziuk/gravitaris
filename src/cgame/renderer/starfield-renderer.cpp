#include <algorithm>
#include <cmath>

#include <Corrade/Containers/ArrayView.h>

#include <Magnum/Mesh.h>
#include <Magnum/GL/Renderer.h>

#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/renderer/starfield-renderer.hpp>

namespace Gravitaris {

using namespace Magnum;

namespace {

// Quad corners in [-1,1]^2 as two triangles; matches the fragment shader's
// radial falloff on `corner`.
constexpr Vector2 QUAD[] = {
        {-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f},
        {-1.f, -1.f}, {1.f, 1.f},  {-1.f, 1.f},
};

// FNV-1a over the three ints, then an avalanche so nearby cells decorrelate.
std::uint32_t HashCell(int cx, int cy, int layer)
{
    std::uint32_t h = 2166136261u;
    const auto mix = [&h](std::uint32_t v) {
        h ^= v;
        h *= 16777619u;
    };
    mix(static_cast<std::uint32_t>(cx));
    mix(static_cast<std::uint32_t>(cy));
    mix(static_cast<std::uint32_t>(layer));
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h;
}

// xorshift32 producing floats in [0,1); seeded per cell.
struct CellRng {
    std::uint32_t state;

    explicit CellRng(std::uint32_t seed) : state(seed ? seed : 0x1u) {}

    float Next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state >> 8) / 16777216.f;
    }
};

} // namespace

StarfieldRenderer::StarfieldRenderer(IFilesystem& filesystem)
    : m_shader(filesystem)
{
    m_layers = {
            //parallax density sizeMin sizeMax bright
            {0.15f, 1.2f, 0.8f, 1.4f, 0.35f},
            {0.35f, 0.9f, 1.0f, 2.0f, 0.55f},
            {0.60f, 0.6f, 1.4f, 2.8f, 0.80f},
            {0.95f, 0.35f, 2.0f, 4.2f, 1.00f},
    };

    m_mesh.setPrimitive(MeshPrimitive::Triangles)
            .addVertexBuffer(m_vertexBuffer, 0,
                             StarfieldShader::Center{},
                             StarfieldShader::Corner{},
                             StarfieldShader::Params{},
                             StarfieldShader::StarColor{});
}

Matrix3 StarfieldRenderer::ViewProjection() const
{
    const float ppu = m_pixelsPerUnit * m_zoom;
    const Vector2 extent = m_viewportSize / ppu;
    return Matrix3::projection(extent) * Matrix3::translation(-m_cameraPos);
}

void StarfieldRenderer::Rebuild()
{
    m_scratch.clear();

    const float ppu = m_pixelsPerUnit * m_zoom;
    const Vector2 halfExtent = m_viewportSize / (2.f * ppu);

    for (std::size_t li = 0; li < m_layers.size(); ++li) {
        const Layer& layer = m_layers[li];

        // Visible region in this layer's space is centered on the parallax-
        // scaled camera; pad by a cell so quads near the edge don't pop in/out.
        const Vector2 effCam = m_cameraPos * layer.parallax;
        const Vector2 margin{m_cellSize, m_cellSize};
        const Vector2 lo = effCam - halfExtent - margin;
        const Vector2 hi = effCam + halfExtent + margin;

        int minCellX = static_cast<int>(std::floor(lo.x() / m_cellSize));
        int minCellY = static_cast<int>(std::floor(lo.y() / m_cellSize));
        int maxCellX = static_cast<int>(std::ceil(hi.x() / m_cellSize));
        int maxCellY = static_cast<int>(std::ceil(hi.y() / m_cellSize));

        // Clamp span so an extreme zoom-out can't explode the vertex count.
        if (maxCellX - minCellX > MAX_CELLS_PER_AXIS) maxCellX = minCellX + MAX_CELLS_PER_AXIS;
        if (maxCellY - minCellY > MAX_CELLS_PER_AXIS) maxCellY = minCellY + MAX_CELLS_PER_AXIS;

        // Fold parallax into position so all layers share one view-projection:
        // worldPos = layerPos + cameraPos*(1 - parallax) reproduces the parallax
        // offset when drawn through the real-camera view-projection.
        const Vector2 parallaxShift = m_cameraPos * (1.f - layer.parallax);

        for (int cy = minCellY; cy < maxCellY; ++cy) {
            for (int cx = minCellX; cx < maxCellX; ++cx) {
                CellRng rng(HashCell(cx, cy, static_cast<int>(li)));

                // Fractional density: the fractional part is a per-cell chance
                // of one extra star, so density averages out across the grid.
                int starCount = static_cast<int>(layer.density);
                if (rng.Next() < layer.density - std::floor(layer.density)) ++starCount;

                for (int s = 0; s < starCount; ++s) {
                    const float fx = (static_cast<float>(cx) + rng.Next()) * m_cellSize;
                    const float fy = (static_cast<float>(cy) + rng.Next()) * m_cellSize;
                    const Vector2 center = Vector2{fx, fy} + parallaxShift;

                    const float size = (layer.sizeMin + rng.Next() * (layer.sizeMax - layer.sizeMin)) * m_pixelScale;
                    const float bright = layer.brightness * (0.6f + 0.4f * rng.Next());

                    // Mostly white with a faint cool/warm tint for variety.
                    const float tint = rng.Next();
                    const Vector3 color{
                            0.85f + 0.15f * (1.f - tint),
                            0.9f,
                            0.85f + 0.15f * tint};

                    for (const Vector2& corner : QUAD) {
                        m_scratch.push_back(Vertex{center, corner, Vector2{size, bright}, color});
                    }
                }
            }
        }
    }

    m_lastStarCount = static_cast<int>(m_scratch.size() / 6);

    if (!m_scratch.empty()) {
        m_vertexBuffer.setData(Containers::ArrayView<const Vertex>{m_scratch.data(), m_scratch.size()});
    }
    m_mesh.setCount(static_cast<Int>(m_scratch.size()));
}

void StarfieldRenderer::Render()
{
    if (!m_enabled) return;

    Rebuild();
    if (m_scratch.empty()) return;

    // Additive over black: overlapping stars accumulate, and the post-process
    // bloom makes the brightest ones glow.
    GL::Renderer::enable(GL::Renderer::Feature::Blending);
    GL::Renderer::setBlendFunction(GL::Renderer::BlendFunction::SourceAlpha, GL::Renderer::BlendFunction::One);

    m_shader.setViewportSize(m_viewportSize)
            .setViewProjection(ViewProjection());

    m_shader.draw(m_mesh);
}

} // namespace Gravitaris
