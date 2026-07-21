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
// frame-to-frame and costs no memory.
//
// The vertex buffer is NOT rebuilt/re-uploaded every frame (2026-07-21):
// it used to be, generating a fresh set of visible cells around the camera
// each Render() call -- under WASM specifically, profiling a real
// playtesting session found Emscripten's WebGL bindings wrap each such
// upload (up to MAX_CELLS_PER_AXIS^2 cells x 4 layers x several stars each x
// 6 verts) in a fresh JS typed-array view, and doing that every single
// frame (the camera is essentially always moving) was large/frequent enough
// to trigger periodic 100ms+ "Major GC" pauses on the browser's main
// thread -- which, in net-client mode, then showed up as ClientPrediction
// reconciliation corrections (the predicted-tick drift/resync guard in
// CGame::TickNetClient exists specifically to catch main-thread stalls like
// this -- see its own comment). Fixed at the root rather than by
// throttling on a timer: parallax is applied per-vertex in the shader (see
// starfield.v.glsl) against a live `cameraPos` uniform (cheap, not a buffer
// reupload) instead of being baked into vertex positions on the CPU side,
// so camera movement alone no longer touches the vertex buffer at all --
// Rebuild() only runs (see NeedsRebuild) once the camera has drifted far
// enough that the deliberately over-padded generated region (see
// GENERATION_MARGIN_UNITS) no longer safely covers what's visible, which at
// ordinary flight speeds is far less often than every frame.
//
// Depth (near layers move faster than far ones) sells the sense of speed;
// stars are plain dots, not stretched toward the direction of motion.
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
        Vector2 center;   // absolute world-space position (no parallax baked in)
        float parallax;   // 0 = infinitely far (still), 1 = moves with the world
        Vector2 corner;
        Vector2 params; // x = size (px), y = brightness
        Vector3 color;
    };

    [[nodiscard]] Matrix3 ViewProjection() const;
    [[nodiscard]] bool NeedsRebuild() const;
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

    // State from the last actual Rebuild() call, for NeedsRebuild's movement
    // /zoom-delta checks.
    Vector2 m_lastRebuildCameraPos{0.f, 0.f};
    float m_lastRebuildZoom = 0.f;

    // How far past the actual viewport Rebuild() generates cells in every
    // direction -- the camera can drift this far (see NeedsRebuild's 0.5x
    // threshold, i.e. half of this) before another rebuild is needed. Big
    // enough that ordinary flight only triggers a rebuild occasionally, not
    // every frame; small enough that MAX_CELLS_PER_AXIS's clamp still isn't
    // reached at normal zoom levels.
    static constexpr float GENERATION_MARGIN_UNITS = 600.f;

    // Bounds the per-axis cell count so an extreme zoom-out can't blow up the
    // vertex count; far edges may stay unfilled past this (acceptable for a
    // background). Common zoom levels fill the screen well within it.
    static constexpr int MAX_CELLS_PER_AXIS = 96;

    // Zoom (see Camera::MIN_ZOOM/MAX_ZOOM, ~0.1..8, 1 = neutral) below which
    // the field starts fading to transparent, down to fully invisible at
    // FADE_END_ZOOM. FADE_START_ZOOM matches CameraDirector's normal dynamic
    // zoom-out floor (Params::minZoom), so ordinary flying never fades the
    // field -- only scrolling out further manually does.
    static constexpr float FADE_START_ZOOM = 0.5f;
    static constexpr float FADE_END_ZOOM = 0.15f;
};

} // namespace Gravitaris
