#pragma once

#include <algorithm>

#include <Magnum/Magnum.h>
#include <Magnum/GL/Framebuffer.h>
#include <Magnum/GL/Texture.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Range.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/fwd.hpp>

#include <gravitaris/cgame/fwd.hpp>

namespace Gravitaris {

using Magnum::Vector2;
using Magnum::Vector2i;
using Magnum::Vector3;

// The refit screen's ship: a cutaway of the hull, drawn large, for the SHIP
// tab of the technology board. Slots go on top of it.
//
// It draws one named group of a model rather than the whole thing -- the
// schematic assets carry their drawing in `model_detail`, which is the layer
// the world never renders. Everything goes into an offscreen texture handed
// to RmlUi through the live-texture bridge, exactly as MinimapRenderer and
// CompassRenderer do; an <img> in ui/tech-tree.rml places it.
class ShipViewRenderer {
public:
    // The layer the schematics are authored in. Sub-groups inside it inherit
    // the name as long as they aren't separately labelled in Inkscape, which
    // is what keeps left and right halves in one group.
    static constexpr const char* DETAIL_GROUP = "model_detail";

    // Landscape, because the hull is drawn lying on its side (nose to the
    // right) and a square panel would be mostly margin. Must match
    // div#ship_view_frame's proportions in tech-tree.rml, or the drawing and
    // the slots positioned over it disagree about where the nose is.
    static constexpr float ASPECT = 16.f / 9.f;

    // Texture height at content scale 1. Generous: this is a page-sized
    // drawing, not a gauge, and the strokes are hairlines that alias badly
    // when they're downsampled from anything tighter.
    static constexpr int TEXTURE_SIZE = 512;

    [[nodiscard]] static Vector2i TextureSizeFor(float contentScale)
    {
        const int height = std::clamp(static_cast<int>(TEXTURE_SIZE * contentScale), TEXTURE_SIZE, 1024);
        return {static_cast<int>(height * ASPECT), height};
    }

    struct Params {
        bool enabled = true;
        float lineWidth = 2.0f;  // stroke width in texture px, before the panel's downsample
        // Fraction of the panel the drawing spans on whichever axis binds
        // first -- its height, for a hull lying on its side in a wide panel.
        float fit = 0.85f;
    };

    // A null `model` blanks the panel: between hulls, or before the schematic
    // for one has been drawn.
    struct Subject {
        const Model* model = nullptr;
        id_t modelId{};
    };

private:
    // Borrows the world renderer's bake rather than keeping a second copy of
    // the geometry, the same bargain CompassRenderer makes.
    ModelRenderer2& m_modelRenderer;

    Vector2i m_textureSize;
    // Texels per design unit, so the stroke width survives the change of
    // resolution at the thickness it was tuned at.
    float m_contentScale;

    Magnum::GL::Texture2D m_texture;
    Magnum::GL::Framebuffer m_framebuffer;

    Params m_params;

    // Bounds of the drawn group, cached against the model it was measured
    // from: a sweep of every vertex, and the answer only changes when the
    // panel switches hulls.
    id_t m_fittedModelId{};
    Magnum::Range2D m_fittedBounds{Vector2{-1.f}, Vector2{1.f}};

    // Lays the hull on its side, centres it in the panel and scales it to
    // `fit`. The schematic is authored wherever it sat on the artboard, with
    // no @origin layer to pull it to zero, so this can't just scale about the
    // origin the way the compass does. Maps into the panel's own space: x
    // spans +-ASPECT, y spans +-1.
    [[nodiscard]] Magnum::Matrix3 FitTransform(const Model& model, id_t modelId);

public:
    ShipViewRenderer(ModelRenderer2& modelRenderer, float contentScale = 1.f);

    Params& GetParams() { return m_params; }

    // Raw GL id + size, for registering with the RmlUi live-texture bridge.
    [[nodiscard]] unsigned TextureId() { return m_texture.id(); }
    [[nodiscard]] Vector2i TextureSize() const { return m_textureSize; }

    // Binds its own framebuffer; the caller binds whatever it draws to next.
    // Only worth calling while the panel is actually on screen -- see
    // CGame::RenderShipView.
    void Render(const Subject& subject);

    // Where a point in model space lands on the panel: 0..1 across the
    // texture, y down, which is the direction RCSS positions things in. The
    // fit is this class's own business, so anything anchored to the drawing
    // (slot markers) has to come back through here rather than repeat it.
    [[nodiscard]] Vector2 PanelUV(const Model& model, id_t modelId, const Vector2& pos);
};

} // namespace Gravitaris
