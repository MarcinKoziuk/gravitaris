#pragma once

#include <optional>
#include <vector>

#include <flecs.h>

#include <ankerl/unordered_dense.h>

#include <Magnum/Magnum.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/Math/Vector2.h>
#include <Magnum/Math/Vector3.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/id.hpp>

#include <gravitaris/cgame/scene-view.hpp>
#include <gravitaris/cgame/renderer/shader/line2-shader.hpp>

namespace Gravitaris {

// A pair of small bars floating over every damageable hull but the one the
// camera is riding -- hull charge under shield charge -- shown only once
// something is missing, so an untouched complex stays clean.
//
// Geometry is rebuilt every frame and drawn in one call: bars are flat
// quads with per-vertex colours (Line2Batch's fill primitive), which is
// cheaper and far more controllable at this size than scaling a baked model
// per bar through SubmitOverlay -- a stroked model would put a line width
// around a three-pixel bar and make a nearly-empty one read as half full.
//
// Into the scene target before the composite, like the beams, so the bars sit
// in the same phosphor as everything else. Whether they actually bloom is a
// colour choice: GlowPostProcess only blooms past its bright-pass threshold,
// so Params::brightness below it keeps them crisp hairlines and above it lets
// them halo (see the debug panel).
class HealthBarRenderer {
public:
    struct Params {
        bool enabled = true;

        // Design pixels: the bars keep their size on screen whatever the
        // camera is doing, same rule the enemy arrows follow.
        float widthPx = 26.f;
        float hullHeightPx = 3.f;
        float shieldHeightPx = 2.f;
        float gapPx = 1.5f;
        // Air between the top of the hull's own bounding box and the bottom
        // bar. In pixels rather than world units so a fighter and a High Port
        // carry their bars the same distance clear.
        float clearancePx = 9.f;

        // Debug: also bar the hull the camera is riding, which Render's
        // `exclude` otherwise drops.
        bool includeSelf = false;

        // GlowPostProcess's bright-pass threshold is 0.35, which is the whole
        // control here: under it the bars are crisp and do not bloom, over it
        // they halo like any other bright thing in the world. `critical` is
        // the same knob for a hull at or below the critical fraction, so
        // "only a dying hull glows" is these two set either side of 0.35.
        float brightness = 0.30f;
        float criticalBrightness = 0.30f;
        // The spent part of each bar, in the same hue as its fill: what is
        // missing should read as missing, not as absent.
        float trackBrightness = 0.07f;

        // Colour steps, matching the sidebar's own hull bar.
        float warnFraction = 0.5f;
        float criticalFraction = 0.25f;

        // Nearest-first cap, for a crowded field. Costs nothing to raise --
        // it is one draw call either way -- but a screen of overlapping bars
        // says less than a handful of them.
        int maxBars = 48;
        // Below this camera zoom nothing is drawn at all. 0 disables the cut.
        float minZoom = 0.f;
    };

    HealthBarRenderer(IFilesystem& filesystem, ResourceLoader& resourceLoader,
                      const UpgradeCatalog& upgradeCatalog);

    Params& GetParams() { return m_params; }

    // `exclude` is the hull the camera is riding (CGame::CameraSubject) --
    // the sidebar already reports that one in full, and a bar pinned over
    // your own ship is the one place this would be in the way. Sweeps both
    // worlds via `view`, so the multiplayer mirror is covered by the same
    // call rather than by a second one somebody has to remember.
    void Render(const SceneView& view, std::optional<flecs::entity> exclude,
                const Magnum::Vector2& cameraPos, float zoom,
                const Magnum::Vector2& designViewportSize, const Magnum::Vector2& viewportSizePx);

private:
    struct Candidate {
        Magnum::Vector2 pos;
        // World units from the hull's origin up to the top of its bounding
        // box in the pose it is actually in.
        float topOffset = 0.f;
        float hullFraction = 0.f;
        float shieldFraction = -1.f; // < 0: no emitter, draw no shield bar
        bool plating = false;
        float distanceSq = 0.f;
    };

    ResourceLoader& m_resourceLoader;
    const UpgradeCatalog& m_upgradeCatalog;

    Line2Shader m_shader;
    Magnum::GL::Mesh m_mesh;
    Magnum::GL::Buffer m_vertexBuffer;
    Magnum::GL::Buffer m_instanceBuffer;

    Params m_params;

    std::vector<Candidate> m_candidates;

    // Model-space bounding box per model id. The Body is already loaded for
    // every hull that can be drawn (RigidBodyDesc locally, HitOutline in the
    // mirror world), so this only saves the per-entity lookup, not the load.
    struct ModelBox {
        Magnum::Vector2 center;
        Magnum::Vector2 halfExtent;
    };

    ankerl::unordered_dense::map<id_t, ModelBox> m_boxByModel;

    [[nodiscard]] ModelBox BoxOf(id_t modelId);
    [[nodiscard]] Magnum::Vector3 HullColor(float fraction) const;
};

} // namespace Gravitaris
