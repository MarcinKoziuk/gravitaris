#pragma once

#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <flecs.h>

#include <Magnum/Magnum.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Buffer.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Vector2.h>
#include <Magnum/Math/Vector4.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>

#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/component/renderable.hpp>

#include <gravitaris/cgame/renderer/shader/line2-shader.hpp>

namespace Gravitaris {

using Magnum::Matrix3;
using Magnum::Vector2;
using Magnum::Vector3;

// Baked/instanced line renderer: line->triangle expansion happens once per
// model at load time (static mesh), so instancing is free for per-entity
// transforms. Width is resolved in the shader in pixel space.
//
// zoomWidthFactor controls how much the pixel width tracks zoom, normalized so
// that at referenceZoom the line is exactly lineWidthPixels wide regardless of
// the factor (so tuning the factor never changes appearance at the normal zoom
// level). Away from referenceZoom:
//   pixelWidth = lineWidthPixels * (zoom / referenceZoom)^zoomWidthFactor
// 0 = constant pixel width across zoom; 1 = constant world-space width (pixel
// width scales linearly with zoom). See SetZoomWidthFactor / SetReferenceZoom.
//
// Kept alongside ModelRenderer/SimpleModelRenderer for comparison; remove
// those once this is validated.
class ModelRenderer2 {
public:
    // Per-entity overrides for one tagged pass, seeded with what the default
    // sweep would have drawn. Returning false skips the entity entirely, which
    // is how a pass draws only the ships that currently have that thing --
    // a lit shield, a burning thruster.
    struct InstanceStyle {
        Vector3 color;      // the entity's team color
        float flash = 0.f;  // the entity's HitFlash
        // (bearing x, bearing y, glow amount, resting opacity) of a shield
        // strike, the bearing in the ship's own frame. Only a pass whose
        // ShieldGlow is not None reads it.
        Magnum::Vector4 shieldFx{};
    };

    using InstanceStyleFn = std::function<bool(flecs::entity, InstanceStyle&)>;

    // One extra tagged pass, drawn after the standard ones. The shield and
    // plating groups arrive this way rather than being named here: which of
    // them is up, how bright, and in what color are all questions about a
    // ship's loadout, which this renderer has no business knowing.
    struct ExtraPass {
        id_t tag;
        InstanceStyleFn style;
        ShieldGlow shieldGlow = ShieldGlow::None;
    };

private:
    // Per (model, tag) cached mesh. instanceBuffer is refilled per frame.
    // debugFacetedMesh: same group baked without circle special-casing, for
    // SetDebugForceFacetedCircles() A/B toggling; shares instanceBuffer.
    struct BakedGroup {
        Magnum::GL::Mesh mesh;
        Magnum::GL::Mesh debugFacetedMesh;
        Magnum::GL::Buffer instanceBuffer;
        Magnum::Int vertexCount = 0;
        Magnum::Int debugFacetedVertexCount = 0;
    };

    // Matches Line2Shader's per-instance layout (Matrix3 transform + team
    // color + hit-flash amount + shield strike).
    struct InstanceData {
        Matrix3 transform;
        Vector3 teamColor;
        float flash;
        Magnum::Vector4 shieldFx;
    };

    flecs::world& m_registry;
    ResourceLoader& m_resourceLoader;
    Line2Shader m_shader;

    std::unordered_map<id_t, std::unordered_map<id_t, BakedGroup>> m_baked;
    // How many loaded models carry each tag. A pass whose tag nothing has --
    // most of the sixteen plate passes, on a fleet of hulls with no plating
    // authored -- skips its registry sweep entirely.
    std::unordered_map<id_t, int> m_tagRefCount;
    std::unordered_map<id_t, std::vector<InstanceData>> m_instanceScratch;

    // (Model::GetRenderOrder, model id), kept sorted as models come and go.
    // Draws follow this order rather than m_baked's hash order: fills are
    // opaque, so a planet interior drawn after the structures nested inside
    // it simply erases them -- and hash order shifts whenever the loaded
    // model set changes, which makes that an intermittent "where did my
    // complex go" bug rather than a permanent one.
    std::vector<std::pair<int, id_t>> m_drawOrder;

    // Per-frame, non-entity instances (HUD arrows etc.), keyed by model id.
    // Merged into the "model" group's pass alongside real entities and cleared
    // at the end of Render(); see SubmitOverlay.
    std::unordered_map<id_t, std::vector<InstanceData>> m_overlayScratch;

    Vector2 m_viewportSize{1280.f, 720.f};
    Vector2 m_cameraPos{0.f, 0.f};
    float m_pixelsPerUnit = 1.f; // zoom 1.0 matches ModelRenderer's zoom 1.0
    float m_zoom = 1.f;
    float m_lineWidthPixels = 2.f;
    float m_zoomWidthFactor = 0.f; // 0 = width constant across zoom, 1 = width scales linearly with zoom
    float m_referenceZoom = 1.f;   // zoom at which lineWidthPixels is the literal pixel width
    float m_contentScale = 1.f; // framebuffer-pixels per design unit (HiDPI + UI-size preference)
    float m_timeSeconds = 0.f; // continuous client clock, for animated colors (see SetTime)
    bool m_debugForceFacetedCircles = false;

    void HandleModelAdded(const Model& model, id_t id);
    void HandleModelRemoved(const Model& model, id_t id);

    [[nodiscard]] Matrix3 ViewProjection() const;

    std::vector<ExtraPass> m_extraPasses;

    void RenderTag(id_t tag, const InstanceStyleFn& style);

public:
    ModelRenderer2(flecs::world& registry, IFilesystem& filesystem, ResourceLoader& resourceLoader);

    ~ModelRenderer2();

    void SetViewportSize(const Vector2& size) { m_viewportSize = size; }
    void SetCameraPosition(const Vector2& pos) { m_cameraPos = pos; }
    void SetZoom(float zoom) { m_zoom = zoom; }
    void SetLineWidth(float pixels) { m_lineWidthPixels = pixels; }
    // See class comment: 0 keeps pixel width constant across zoom, 1 makes it scale linearly with zoom.
    void SetZoomWidthFactor(float factor) { m_zoomWidthFactor = factor; }
    [[nodiscard]] float GetZoomWidthFactor() const { return m_zoomWidthFactor; }
    // Zoom at which the width matches lineWidthPixels exactly (usually the default camera zoom).
    void SetReferenceZoom(float zoom) { m_referenceZoom = zoom; }
    void SetContentScale(float scale) { m_contentScale = scale; }
    // Wall-clock seconds since the client started, for colors that animate on
    // their own (a Lab's ready flash). Render()'s `delta` can't stand in for
    // it: callers pass a fixed-step interpolation fraction there, or 0.
    void SetTime(float seconds) { m_timeSeconds = seconds; }

    // Installed once at startup, not per frame -- these are as fixed a part of
    // the draw as the standard passes are.
    void SetExtraPasses(std::vector<ExtraPass> passes) { m_extraPasses = std::move(passes); }

    void SetDebugForceFacetedCircles(bool force) { m_debugForceFacetedCircles = force; }
    [[nodiscard]] bool GetDebugForceFacetedCircles() const { return m_debugForceFacetedCircles; }

    // Draws `modelId`'s "model" group once more this frame with an arbitrary
    // transform, for things that aren't entities (HUD arrows, markers). Rides
    // the same instanced draw as real entities, so overlays get the same line
    // width and glow for free. `transform` is world space -- a caller wanting
    // screen-anchored placement converts px to world itself (see
    // CGame::UpdateIndicators). `color` fills the same slot as an entity's team
    // color, i.e. it only reaches strokes/fills authored in
    // TEAM_COLOR_PLACEHOLDER. Submissions last one frame: call every frame the
    // overlay should be visible, before Render().
    void SubmitOverlay(id_t modelId, const Matrix3& transform, const Vector3& color, float flash = 0.f);

    // Draws one baked model's "model" group into the currently bound
    // framebuffer under an explicit view-projection, bypassing both the world
    // camera and the entity sweep -- for the offscreen HUD panels (the compass
    // dial) that need a real ship drawn but have no scene to draw it in. Both
    // the transform and the projection are the caller's own space, whatever
    // that is. A no-op for a model that isn't baked here yet.
    void RenderStandalone(id_t modelId, const Matrix3& transform, const Matrix3& viewProjection,
                          const Vector2& viewportSizePx, float lineWidthPx, const Vector3& teamColor);

    void Render(double delta);
};

} // namespace Gravitaris
