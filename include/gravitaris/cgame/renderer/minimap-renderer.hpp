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
        float worldRadius = 12000.f; // world units from the map center to the map edge
        float shipDotPx = 3.f;      // ship dot radius, minimap texture px
        float playerDotPx = 1.5f;   // player marker dot radius, minimap texture px
        float planetMinPx = 2.f;    // floor for a planet ring that'd map below this
        float starMinPx = 3.5f;     // floor for a sun ring that'd map below this (bigger than a planet)
        bool showViewRect = false;  // outline the main camera's visible extent
    };

private:
    flecs::world& m_registry;

    Line2Shader m_shader;
    Magnum::GL::Texture2D m_texture;
    Magnum::GL::Framebuffer m_framebuffer;

    Magnum::GL::Mesh m_mesh;
    Magnum::GL::Buffer m_vertexBuffer;
    Magnum::GL::Buffer m_instanceBuffer;

    Params m_params;

public:
    MinimapRenderer(flecs::world& registry, IFilesystem& filesystem);

    Params& GetParams() { return m_params; }

    // Raw GL id + size, for registering with the RmlUi live-texture bridge.
    [[nodiscard]] unsigned TextureId() { return m_texture.id(); }
    [[nodiscard]] static Vector2i TextureSize() { return {TEXTURE_SIZE, TEXTURE_SIZE}; }

    // Renders the map: static, centered on `mapCenter` (not the player) so
    // panning the ship doesn't scroll the map. `playerPos` places the player
    // marker within that static view. viewCenter/viewHalfExtent describe the
    // main camera's world-space extent for the (optional) view rectangle.
    // `remoteWorld`, when non-null, is swept alongside `registry` for planets
    // and ships -- multiplayer's mirror world, so remote entities the player
    // never locally simulates still show up exactly like a single-player
    // registry entity would. Binds its own framebuffer; the caller is
    // expected to bind whatever it renders to next itself (the app runs this
    // before the glow pass claims the scene target).
    void Render(const Vector2& mapCenter, const Vector2& playerPos,
               const Vector2& viewCenter, const Vector2& viewHalfExtent,
               flecs::world* remoteWorld = nullptr);
};

} // namespace Gravitaris
