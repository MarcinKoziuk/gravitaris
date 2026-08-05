#pragma once

#include <algorithm>
#include <optional>

#include <flecs.h>

#include <gravitaris/cgame/scene-view.hpp>

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
    // Texture resolution at content scale 1; RCSS scales the on-screen panel
    // independently. The real texture is this times the content scale, so the
    // panel keeps the same supersampling ratio on a dense display instead of
    // going soft (see TextureSizeFor).
    static constexpr int TEXTURE_SIZE = 512;

    // Chosen once, at construction: the live-texture bridge hands RmlUi a raw
    // GL id, and RmlUi caches it when the document loads -- so reallocating
    // later would leave it drawing a texture that no longer exists. A UI-scale
    // change therefore only reaches the map on the next run.
    [[nodiscard]] static Vector2i TextureSizeFor(float contentScale)
    {
        const int size = std::clamp(static_cast<int>(TEXTURE_SIZE * contentScale), TEXTURE_SIZE, 2048);
        return {size, size};
    }

    struct Params {
        bool enabled = true;
        float worldRadius = 24000.f; // world units from the map center to the map edge
        // Rewrites worldRadius each frame to whatever actually fits the
        // sector's bodies -- a generated sector's size is a function of its
        // seed, so no fixed radius suits every round. Derived from orbit
        // parameters rather than live positions, so it doesn't breathe as
        // planets travel. Clear it to take manual control from the debug
        // panel (the last fitted value stays put as the starting point).
        bool autoFit = true;
        // Ships read as markers, not bodies: a fighter is a speck next to a
        // planet in world scale, and drawing it anywhere near a planet's size
        // makes the map lie about what is worth flying to.
        float shipTriPx = 3.5f;     // ship triangle circumradius, minimap texture px
        // A hauler is a filled dot rather than a triangle (the triangle
        // means "armed"), and smaller than one: commerce is background
        // traffic, and only your own is mapped at all.
        float freighterDotPx = 2.f;
        float playerDotPx = 3.f;    // player marker dot radius, minimap texture px
        float planetMinPx = 4.f;    // floor for a planet ring that'd map below this
        float starMinPx = 7.f;      // floor for a sun ring that'd map below this (bigger than a planet)
        bool showViewRect = true;   // shade the main camera's visible extent
    };

private:
    // Before the framebuffer: its constructor is initialized from this.
    Vector2i m_textureSize;
    // Texels per design unit -- what the texture-pixel-authored stroke widths
    // are multiplied by so they stay the same thickness on screen.
    float m_contentScale;

    Line2Shader m_shader;
    Magnum::GL::Texture2D m_texture;
    Magnum::GL::Framebuffer m_framebuffer;

    Magnum::GL::Mesh m_mesh;
    Magnum::GL::Buffer m_vertexBuffer;
    Magnum::GL::Buffer m_instanceBuffer;

    Params m_params;

    // The radius the last Render actually framed: the fitted one, widened if
    // that was what it took to keep the subject on the map.
    float m_frameRadius = 0.f;

    // Grows m_params.worldRadius to cover every celestial's furthest reach
    // from `mapCenter`. Never shrinks within a round -- a body destroyed or
    // not yet replicated shouldn't snap the whole map to a new scale.
    void FitToSector(const SceneView& view, const Vector2& mapCenter);

public:
    explicit MinimapRenderer(IFilesystem& filesystem, float contentScale = 1.f);

    Params& GetParams() { return m_params; }

    // World radius the map currently spans, for turning a click on the panel
    // back into a world position. Not Params::worldRadius: that is the body
    // fit alone, which is not what was drawn once the subject pushed the
    // frame wider.
    [[nodiscard]] float FrameRadius() const { return std::max(m_frameRadius, 1.f); }

    // Drops the fitted radius so auto-fit re-establishes it from scratch.
    // Needed when the world is replaced by a smaller one, which the fit's
    // never-shrink rule would otherwise leave framed for its predecessor.
    void ResetFit() { m_params.worldRadius = 0.f; }

    // Raw GL id + size, for registering with the RmlUi live-texture bridge.
    [[nodiscard]] unsigned TextureId() { return m_texture.id(); }
    [[nodiscard]] Vector2i TextureSize() const { return m_textureSize; }

    // Renders the map: static, centered on `mapCenter` (not the player) so
    // panning the ship doesn't scroll the map. `subjectPos` places the ringed
    // marker within that static view, and is empty during round setup (no
    // subject yet) and between death and respawn -- the camera subject, so it follows a
    // spectated ship rather than always sitting on your own. `subjectTeam` is
    // whose side the map is drawn for: it decides which freighters are
    // mapped, and TeamId::None (no subject) maps none. viewCenter/
    // viewHalfExtent describe the main camera's world-space extent, shaded as
    // the (optional) view rectangle.
    // Every world in `view` is swept for planets and ships, so multiplayer's
    // mirror-world entities show up exactly like single-player registry ones.
    // Binds its own framebuffer; the caller is
    // expected to bind whatever it renders to next itself (the app runs this
    // before the glow pass claims the scene target).
    void Render(const SceneView& view, const Vector2& mapCenter, std::optional<Vector2> subjectPos,
               TeamId subjectTeam, const Vector2& viewCenter, const Vector2& viewHalfExtent);
};

} // namespace Gravitaris
