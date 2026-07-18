#pragma once

#include <optional>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/resource/common/resource-ptr.hpp>

#include <gravitaris/cgame/fwd.hpp>

namespace Gravitaris {

// Off-screen target arrows: a ring around screen center, each arrow pointing
// at a nearby enemy that's currently off-screen. Submits overlays into
// ModelRenderer2 (rides its instanced draw), so it must run after the camera
// director settles the view and before that renderer draws.
class IndicatorRenderer {
public:
    // Tunables (exposed in the HUD debug tab). Distances are world units;
    // sizes are logical pixels (scaled by the HiDPI pixel scale like line
    // width is).
    struct Params {
        bool enabled = true;
        float ringRadiusPx = 120.f;  // arrow ring radius around screen center
        float arrowSizePx = 13.f;    // arrow width, and height at long range (see maxHeightFactor)
        float enemyRange = 2500.f;   // show enemies within this
        float edgeMarginPx = 24.f;   // treat as off-screen this far inside the view edge
        float fadeBandPx = 90.f;     // px past the edge over which an arrow fades fully in
        float minStrength = 0.35f;   // brightness floor at max range (never fully invisible while in range)
        // Height-only stretch at point-blank range (width never changes with
        // distance, only with the edge-appear fade) -- 1 = no stretch, taller
        // as the target closes in. Intentionally allowed to look "squished".
        float maxHeightFactor = 2.5f;
        // Multiplies the proximity fed into the height stretch, so it reaches
        // maxHeightFactor within a 1/heightRampFactor fraction of the range --
        // arrows stay flat over most of the range and only stretch tall right
        // at the end, instead of ramping linearly across the whole range.
        float heightRampFactor = 4.f;
        int maxEnemies = 8;
    };

private:
    flecs::world& m_registry;
    ModelRenderer2& m_modelRenderer2;

    // Kept alive so the arrow glyph stays baked in ModelRenderer2 (loading a
    // Model is what fires the renderer's OnCreate<Model>); overlays aren't
    // entities, so nothing else holds a reference to it.
    ResourcePtr<const Model> m_arrowModel;

    Params m_params;

public:
    IndicatorRenderer(flecs::world& registry, ResourceLoader& resourceLoader, ModelRenderer2& modelRenderer2);

    Params& GetParams() { return m_params; }

    // Submits an arrow overlay per nearby-but-off-screen enemy. `player` may
    // be a dead/invalid entity (between death and respawn) -- the update is
    // then a no-op. `cameraPos`/`zoom` are the camera director's final
    // values for this frame; `pixelScale` is framebuffer-pixels-per-logical
    // -pixel (HiDPI).
    void Update(std::optional<flecs::entity> player, const Magnum::Vector2& cameraPos, float zoom,
               const Magnum::Vector2& viewportSize, float pixelScale);
};

} // namespace Gravitaris
