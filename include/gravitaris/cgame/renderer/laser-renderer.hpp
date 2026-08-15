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
        // How far the light carries, in world units: full strength at the
        // mount, and down to a few percent of it three of these out. Measured
        // from the mount rather than shared out across the beam, so a beam cut
        // short by a hull is still as bright where it lands as any other beam
        // at that distance -- what it burns and what it lights are two
        // different reaches, and only this one is visible.
        float fadeLength = 1.f;
        // How far this segment's near end already is from the mount, in fade
        // lengths. Zero for a beam leaving a hull; non-zero for one thrown back
        // off a mirrored plate, so the light carries on dying across the kink
        // instead of coming back to full brightness at the bounce.
        float fadeStart = 0.f;
    };

    // The light gathered at a mount: a round glow that swells as the emitter
    // charges and then sits at the beam's root for as long as it burns. A disc
    // rather than a stub of beam, because a beam seen end-on is a point of
    // light and a square reads as a texture nobody drew.
    struct Charge {
        Magnum::Vector2 at;
        // Premultiplied by how far along the charge is: the pass is additive,
        // so a dimmer colour IS the fade in from nothing.
        Magnum::Color3 color;
        float radius = 1.f;
    };

    explicit LaserRenderer(IFilesystem& filesystem);

    void SetViewportSize(const Magnum::Vector2& size) { m_viewportSize = size; }
    void SetContentScale(float scale) { m_contentScale = scale; }
    void SetCameraPosition(const Magnum::Vector2& position) { m_cameraPos = position; }
    void SetZoom(float zoom) { m_zoom = zoom; }

    // Nothing is retained: a beam exists for exactly as long as its trigger is
    // held, so there is no state here worth carrying between frames. Both lists
    // go out in one draw -- they are the same additive light, and a charge is
    // always at the root of a beam that may or may not have arrived yet.
    void Render(const std::vector<Beam>& beams, const std::vector<Charge>& charges);

private:
    struct Vertex {
        Magnum::Vector2 position;
        Magnum::Color4 color;
    };

    // Segments in a charge's disc. Twelve is round at the size these are drawn
    // and cheap enough that a hull firing three of them is not worth counting.
    static constexpr int CHARGE_SEGMENTS = 12;

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
