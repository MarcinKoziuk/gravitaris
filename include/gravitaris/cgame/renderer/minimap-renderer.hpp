#pragma once

#include <flecs.h>

#include <Magnum/Magnum.h>
#include <Magnum/GL/Framebuffer.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/Texture.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/component/team.hpp>

#include <gravitaris/cgame/renderer/shader/line2-shader.hpp>

namespace Gravitaris {

using Magnum::Vector2;
using Magnum::Vector2i;
using Magnum::Vector3;

// Radar-style minimap rendered into an offscreen texture: planets as rings
// (true world radius), ships as team-colored dots, the player as a marked dot,
// plus the camera's current view extent as a rectangle. The texture is handed
// to RmlUi through the render interface's live-texture bridge (see
// RenderInterfaceGL3::RegisterLiveTexture), so an RCSS-styled <img> projects
// it into the UI -- position/size/border all live in ui/hud.rml.
//
// Icon shapes ride Line2Shader's analytic primitives (ring/disc/segment), so
// the map shares the game's vector look and, with the UI drawn in-world, its
// bloom/CRT treatment.
class MinimapRenderer {
public:
    // Fixed texture resolution; RCSS scales the on-screen panel independently.
    static constexpr int TEXTURE_SIZE = 256;

    struct Params {
        bool enabled = true;
        float worldRadius = 3000.f; // world units from the player to the map edge
        float shipDotPx = 3.f;      // ship dot radius, minimap texture px
        float planetMinPx = 4.f;    // floor for a planet ring that'd map below this
        bool showViewRect = true;   // outline the main camera's visible extent
    };

private:
    flecs::world& m_registry;
    PhysicsSystem& m_physicsSystem;

    Line2Shader m_shader;
    Magnum::GL::Texture2D m_texture;
    Magnum::GL::Framebuffer m_framebuffer;

    Magnum::GL::Mesh m_mesh;
    Magnum::GL::Buffer m_vertexBuffer;
    Magnum::GL::Buffer m_instanceBuffer;

    Params m_params;

public:
    MinimapRenderer(flecs::world& registry, PhysicsSystem& physicsSystem, IFilesystem& filesystem);

    Params& GetParams() { return m_params; }

    // Raw GL id + size, for registering with the RmlUi live-texture bridge.
    [[nodiscard]] unsigned TextureId() { return m_texture.id(); }
    [[nodiscard]] static Vector2i TextureSize() { return {TEXTURE_SIZE, TEXTURE_SIZE}; }

    // Renders the map centered on `center` (the player). viewCenter/
    // viewHalfExtent describe the main camera's world-space extent for the
    // view rectangle. Binds its own framebuffer; the caller is expected to
    // bind whatever it renders to next itself (the app runs this before the
    // glow pass claims the scene target).
    void Render(const Vector2& center, const Vector2& viewCenter, const Vector2& viewHalfExtent);
};

} // namespace Gravitaris
