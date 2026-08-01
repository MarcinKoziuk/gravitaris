#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/GL/Framebuffer.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/Texture.h>
#include <Magnum/Math/Angle.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/fwd.hpp>

#include <gravitaris/cgame/fwd.hpp>
#include <gravitaris/cgame/renderer/shader/line2-shader.hpp>

namespace Gravitaris {

using Magnum::Vector2;
using Magnum::Vector2i;
using Magnum::Vector3;

// The heading gauge: a plain ring with the subject's own ship drawn inside it,
// turning as the ship turns. The silhouette is the reading -- no bezel, no
// tick marks, nothing the ship itself already tells you.
//
// The ship is the real baked model (ModelRenderer2::RenderStandalone), not a
// stand-in, so it carries the same strokes, fills and team color it has in the
// world. Everything is drawn into an offscreen texture and handed to RmlUi
// through the live-texture bridge exactly as MinimapRenderer is; an <img> in
// ui/hud.rml places it, in the telemetry row beside the HDG readout.
class CompassRenderer {
public:
    // Fixed texture resolution. Deliberately larger than the on-screen dial
    // (~44dp) so the panel's downsample supersamples the strokes -- but only
    // ~3x: further out and lineWidth has to grow to survive the shrink, which
    // costs more than the extra sampling buys.
    static constexpr int TEXTURE_SIZE = 128;

    struct Params {
        bool enabled = true;
        // Where the ship is actually travelling, which its facing doesn't say
        // -- the one reading the silhouette can't give you, so it stays on.
        bool showVelocity = true;
        // Which way the field pulls. Off by default: useful, but a second dot
        // is the point where the gauge starts becoming an instrument again.
        bool showGravity = false;
        // Below these the marker is dropped rather than drawn: the direction
        // of a near-zero vector is numerical noise, and a marker jittering
        // around the ring reads as a fault in the gauge.
        float minSpeed = 8.f;     // world units/s
        float minGravity = 0.4f;  // world units/s^2
        float lineWidth = 5.f;    // stroke width in texture px, before the panel's downsample
        float shipFit = 0.60f;    // ship's bounding radius, in the dial's own -1..1 space
    };

    // What the gauge is reporting on this frame. A null `model` (no subject,
    // or between death and respawn) draws the empty ring rather than blanking
    // the panel, the way the minimap keeps drawing the sector with no marker.
    struct Subject {
        const Model* model = nullptr;
        id_t modelId{};
        Magnum::Rad rot{0.f};
        Vector3 teamColor{1.f, 1.f, 1.f};
        Vector2 velocity;
        Vector2 gravity;
    };

private:
    // Draws the ship: the panel borrows the world renderer's bake rather than
    // keeping a second copy of every hull's geometry. Either ModelRenderer2
    // would do -- a model bakes into every one alive when it loads, which is
    // both of them (see CGame's constructor) -- so multiplayer's mirror-world
    // ships need no special case here.
    ModelRenderer2& m_modelRenderer;

    Line2Shader m_shader;
    Magnum::GL::Texture2D m_texture;
    Magnum::GL::Framebuffer m_framebuffer;

    Magnum::GL::Mesh m_mesh;
    Magnum::GL::Buffer m_vertexBuffer;
    Magnum::GL::Buffer m_instanceBuffer;

    Params m_params;

    // Bounding radius of the model last fitted, cached against its id: it's a
    // sweep of every vertex in the hull, and the answer only changes when the
    // subject switches to a different ship.
    id_t m_fittedModelId{};
    float m_fittedModelRadius = 1.f;

    // Dial-space units per world unit for `model`, so it fills shipFit of the
    // ring whatever size the hull is authored at.
    [[nodiscard]] float FitScale(const Model& model, id_t modelId);

public:
    CompassRenderer(IFilesystem& filesystem, ModelRenderer2& modelRenderer);

    Params& GetParams() { return m_params; }

    // Raw GL id + size, for registering with the RmlUi live-texture bridge.
    [[nodiscard]] unsigned TextureId() { return m_texture.id(); }
    [[nodiscard]] static Vector2i TextureSize() { return {TEXTURE_SIZE, TEXTURE_SIZE}; }

    // Binds its own framebuffer; the caller binds whatever it draws to next
    // (the app runs this before the glow pass claims the scene target).
    void Render(const Subject& subject);
};

} // namespace Gravitaris
