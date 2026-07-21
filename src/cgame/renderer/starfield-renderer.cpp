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
                             StarfieldShader::Parallax{},
                             StarfieldShader::Corner{},
                             StarfieldShader::Params{},
                             StarfieldShader::StarColor{});
}

Matrix3 StarfieldRenderer::ViewProjection() const
{
    // Projection only -- no camera translation. Parallax/camera are applied
    // per-vertex in the shader instead (see starfield.v.glsl's own comment
    // on why: baking camera position into vertex data the way this used to
    // work meant any camera movement invalidated the whole vertex buffer,
    // forcing a full re-upload every frame).
    const float ppu = m_pixelsPerUnit * m_zoom;
    const Vector2 extent = m_viewportSize / ppu;
    return Matrix3::projection(extent);
}

bool StarfieldRenderer::NeedsRebuild() const
{
    if (m_scratch.empty()) return true;

    // Zoom changes the visible extent (zooming out reveals more world space,
    // which may reach past the padded region below) and the fade level
    // (baked into vertex brightness at rebuild time) -- re-check on any
    // meaningfully large change rather than tracking exact extent math.
    if (std::fabs(m_zoom - m_lastRebuildZoom) > m_lastRebuildZoom * 0.2f) return true;

    // How far the camera has moved since the region below was generated,
    // scaled per layer by its own parallax (a layer's own effective
    // "camera" moves at cameraPos * parallax -- see the shader). Once any
    // layer's effective movement eats too far into GENERATION_MARGIN_UNITS'
    // padding, it's time to regenerate before cells actually run out.
    const float movedDistance = (m_cameraPos - m_lastRebuildCameraPos).length();
    for (const Layer& layer : m_layers) {
        if (movedDistance * layer.parallax > GENERATION_MARGIN_UNITS * 0.5f) return true;
    }
    return false;
}

void StarfieldRenderer::Rebuild()
{
    m_scratch.clear();
    m_lastRebuildCameraPos = m_cameraPos;
    m_lastRebuildZoom = m_zoom;

    const float ppu = m_pixelsPerUnit * m_zoom;
    // Generously over-padded past the actual viewport so the camera can move
    // for a while (NeedsRebuild's threshold) before any cell is missing --
    // this, not per-frame rebuilding, is what keeps this cheap: see this
    // class's own doc comment.
    const Vector2 halfExtent =
            m_viewportSize / (2.f * ppu) + Vector2{GENERATION_MARGIN_UNITS, GENERATION_MARGIN_UNITS};

    // Zooming out past this fades the whole field toward transparent (see the
    // brightness multiply below) -- both so the additive blending of a much
    // denser-looking field (same world density, far more world visible per
    // pixel) doesn't wash the screen white, and so a viewer rarely scrolls
    // far enough to see the MAX_CELLS_PER_AXIS cutoff below kick in at all.
    const float zoomFade = std::clamp((m_zoom - FADE_END_ZOOM) / (FADE_START_ZOOM - FADE_END_ZOOM), 0.f, 1.f);
    if (zoomFade <= 0.f) return;

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
        // Centered on the camera cell rather than the min bound, so past the
        // cutoff the field stays a centered patch that shrinks in from every
        // edge -- clamping maxCellX/Y alone anchored the covered area to the
        // (essentially arbitrary) floor()'d min corner instead, which read as
        // the whole field collapsing into one corner of the screen once the
        // camera scrolled far enough out.
        if (maxCellX - minCellX > MAX_CELLS_PER_AXIS) {
            const int centerCellX = static_cast<int>(std::floor(effCam.x() / m_cellSize));
            minCellX = centerCellX - MAX_CELLS_PER_AXIS / 2;
            maxCellX = centerCellX + MAX_CELLS_PER_AXIS / 2;
        }
        if (maxCellY - minCellY > MAX_CELLS_PER_AXIS) {
            const int centerCellY = static_cast<int>(std::floor(effCam.y() / m_cellSize));
            minCellY = centerCellY - MAX_CELLS_PER_AXIS / 2;
            maxCellY = centerCellY + MAX_CELLS_PER_AXIS / 2;
        }

        for (int cy = minCellY; cy < maxCellY; ++cy) {
            for (int cx = minCellX; cx < maxCellX; ++cx) {
                CellRng rng(HashCell(cx, cy, static_cast<int>(li)));

                // Fractional density: the fractional part is a per-cell chance
                // of one extra star, so density averages out across the grid.
                int starCount = static_cast<int>(layer.density);
                if (rng.Next() < layer.density - std::floor(layer.density)) ++starCount;

                for (int s = 0; s < starCount; ++s) {
                    // Absolute world-space position -- no parallax baked in
                    // here (see ViewProjection's and the shader's own
                    // comments); the shader applies parallax per-vertex from
                    // the `parallax` field below plus a live camera uniform.
                    const float fx = (static_cast<float>(cx) + rng.Next()) * m_cellSize;
                    const float fy = (static_cast<float>(cy) + rng.Next()) * m_cellSize;
                    const Vector2 center{fx, fy};

                    const float size = (layer.sizeMin + rng.Next() * (layer.sizeMax - layer.sizeMin)) * m_pixelScale;
                    const float bright = layer.brightness * (0.6f + 0.4f * rng.Next()) * zoomFade;

                    // Mostly white with a faint cool/warm tint for variety.
                    const float tint = rng.Next();
                    const Vector3 color{
                            0.85f + 0.15f * (1.f - tint),
                            0.9f,
                            0.85f + 0.15f * tint};

                    for (const Vector2& corner : QUAD) {
                        m_scratch.push_back(Vertex{center, layer.parallax, corner, Vector2{size, bright}, color});
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

    if (NeedsRebuild()) Rebuild();
    if (m_scratch.empty()) return;

    // Additive over black: overlapping stars accumulate, and the post-process
    // bloom makes the brightest ones glow.
    GL::Renderer::enable(GL::Renderer::Feature::Blending);
    GL::Renderer::setBlendFunction(GL::Renderer::BlendFunction::SourceAlpha, GL::Renderer::BlendFunction::One);

    // Camera position is a plain uniform update, every frame, regardless of
    // whether a rebuild happened -- cheap (not a buffer reupload), and it's
    // what keeps the camera tracking smoothly between rebuilds (see
    // NeedsRebuild/the shader's own comment).
    m_shader.setViewportSize(m_viewportSize)
            .setViewProjection(ViewProjection())
            .setCameraPos(m_cameraPos);

    m_shader.draw(m_mesh);
}

} // namespace Gravitaris
