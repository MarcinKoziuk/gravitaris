#pragma once

#include <cstdint>
#include <vector>

#include <Magnum/Magnum.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Vector2.h>
#include <Magnum/Math/Vector3.h>

#include <gravitaris/game/fwd.hpp>

#include <gravitaris/cgame/renderer/shader/starfield-shader.hpp>

namespace Gravitaris {

using Magnum::Matrix3;
using Magnum::Vector2;
using Magnum::Vector3;

// Procedural parallax starfield drawn behind the scene. Stars are not stored:
// world space is divided into a grid, each cell hashed to a deterministic RNG
// that produces its stars, so the field is infinite, non-repeating, stable
// frame-to-frame and costs no memory. Each frame only the visible cells are
// regenerated into one vertex buffer and drawn in a single call.
//
// Parallax: each layer scrolls at cameraPos * parallax, folded into per-vertex
// positions so all layers share one view-projection (one draw). Depth alone
// (near layers move faster than far ones) sells the sense of speed; stars are
// plain dots, not stretched toward the direction of motion.
class StarfieldRenderer {
public:
    // A depth layer. Deeper (small parallax) layers scroll slowly and are
    // dimmer/smaller; near layers scroll fast and are bright.
    struct Layer {
        float parallax;    // 0 = infinitely far (still), 1 = moves with the world
        float density;     // average stars per grid cell
        float sizeMin;     // dot radius in logical pixels
        float sizeMax;
        float brightness;
    };

    explicit StarfieldRenderer(IFilesystem& filesystem);

    void SetViewportSize(const Vector2& size) { m_viewportSize = size; }
    void SetCameraPosition(const Vector2& pos) { m_cameraPos = pos; }
    void SetZoom(float zoom) { m_zoom = zoom; }
    void SetPixelScale(float scale) { m_pixelScale = scale; }

    void Render();

    // --- debug/tuning accessors ---
    [[nodiscard]] bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    [[nodiscard]] float GetCellSize() const { return m_cellSize; }
    void SetCellSize(float size) { m_cellSize = size; }
    [[nodiscard]] std::vector<Layer>& Layers() { return m_layers; }
    [[nodiscard]] int GetLastStarCount() const { return m_lastStarCount; }

    struct Defaults {
        static constexpr float cellSize = 96.f;
    };

private:
    // Must match StarfieldShader's attribute layout.
    struct Vertex {
        Vector2 center;
        Vector2 corner;
        Vector2 params; // x = size (px), y = brightness
        Vector3 color;
    };

    [[nodiscard]] Matrix3 ViewProjection() const;
    void Rebuild();

    StarfieldShader m_shader;
    Magnum::GL::Mesh m_mesh;
    Magnum::GL::Buffer m_vertexBuffer;

    Vector2 m_viewportSize{1280.f, 720.f};
    Vector2 m_cameraPos{0.f, 0.f};
    float m_pixelsPerUnit = 1.f;
    float m_zoom = 1.f;
    float m_pixelScale = 1.f;

    bool m_enabled = true;
    float m_cellSize = Defaults::cellSize;
    std::vector<Layer> m_layers;

    std::vector<Vertex> m_scratch;
    int m_lastStarCount = 0;

    // Bounds the per-axis cell count so an extreme zoom-out can't blow up the
    // vertex count; far edges may stay unfilled past this (acceptable for a
    // background). Common zoom levels fill the screen well within it.
    static constexpr int MAX_CELLS_PER_AXIS = 96;
};

} // namespace Gravitaris
