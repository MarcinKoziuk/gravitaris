#pragma once

#include <vector>

#include <Magnum/Magnum.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

#include <gravitaris/cgame/renderer/shader/laser-shader.hpp>

namespace Gravitaris {

// Draws the beams that are burning this frame. Deliberately knows nothing
// about ships, mounts or arcs: it is handed finished world-space wedges and
// puts them on screen, so the same call serves the locally-predicted own ship
// and the replicated ones in the mirror world.
//
// Into the scene target, before the postprocess composite, so a beam picks up
// the phosphor bloom every other bright thing in the world gets.
class LaserRenderer {
public:
    // One beam as the world sees it: narrow where it leaves the mount, wider
    // and fainter where it ends. The width is in world units, so a beam thins
    // out as the camera pulls back like everything else does.
    struct Beam {
        Magnum::Vector2 from;
        Magnum::Vector2 to;
        Magnum::Color3 color;
        float widthNear = 1.f;
        float widthFar = 6.f;
    };

    explicit LaserRenderer(IFilesystem& filesystem);

    void SetViewportSize(const Magnum::Vector2& size) { m_viewportSize = size; }
    void SetContentScale(float scale) { m_contentScale = scale; }
    void SetCameraPosition(const Magnum::Vector2& position) { m_cameraPos = position; }
    void SetZoom(float zoom) { m_zoom = zoom; }

    // Nothing is retained: a beam exists for exactly as long as its trigger is
    // held, so there is no state here worth carrying between frames.
    void Render(const std::vector<Beam>& beams);

private:
    struct Vertex {
        Magnum::Vector2 position;
        Magnum::Color4 color;
    };

    LaserShader m_shader;
    Magnum::GL::Buffer m_buffer;
    Magnum::GL::Mesh m_mesh;
    std::vector<Vertex> m_vertices;

    Magnum::Vector2 m_viewportSize{1280.f, 720.f};
    Magnum::Vector2 m_cameraPos{};
    float m_contentScale = 1.f;
    float m_zoom = 1.f;
};

} // namespace Gravitaris
