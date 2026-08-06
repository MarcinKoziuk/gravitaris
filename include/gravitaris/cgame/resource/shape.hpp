#pragma once

#include <vector>
#include <memory>
#include <optional>
#include <string>

#include <Magnum/Magnum.h>
#include <Magnum/Math/Color.h>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>
#include <gravitaris/game/resource/common/iresource.hpp>
#include <gravitaris/game/resource/common/resource-ptr.hpp>

struct NSVGshape;

namespace Gravitaris {

using Magnum::Vector2d;
using Magnum::Color4;
using Magnum::Matrix4d;

class Shape : public IResource {
public:
    struct Style {
        Color4 color;      // stroke color
        float thickness;
        bool useTeamColor;
        // Interior fill (SVG `fill`). Closed filled paths get a solid fill
        // baked behind their stroke -- e.g. black planet interiors that block
        // the starfield, or ship-body fills. hasFill stays false for
        // stroke-only paths.
        Color4 fillColor;
        bool hasFill;

        Style() : thickness(1.f), useTeamColor(true), fillColor(0.f, 0.f, 0.f, 1.f), hasFill(false) {}
    };

    // Present only when the path is (within tolerance) an exact circle, as
    // opposed to an arbitrary closed curve that happens to look round — see
    // IsCircleGeometry(). Center/radius are already in the same
    // post-transform space as `points`.
    struct Circle {
        Vector2d center;
        double radius;
    };

    struct Path {
        std::vector<Vector2d> points;
        Style style;
        bool closed;
        id_t group;
        std::optional<Circle> circle;

        Path()
            : group(0L)
            , closed(false)
            {}
    };

    // A named point authored in the '@slots' layer, in the same transformed
    // space as the paths. `name` is the element's inkscape:label verbatim --
    // what it means (category, index) is the reader's business, not the
    // resource's.
    struct Marker {
        std::string name;
        Vector2d pos;
    };

private:
    std::vector<Path> m_paths;
    std::vector<Marker> m_markers;
    int m_renderOrder = 0;

    static ResourcePtr<const Shape> placeholder;

    // `plateIndex`, when given, is a running count of ablative plates: each
    // path this call emits takes PlatingTag() of the next index rather than
    // the shared group tag.
    void AddPaths(const NSVGshape* shape, const Matrix4d& transform, id_t group, bool fxLayer = false,
                  std::size_t* plateIndex = nullptr);

    void AddMarker(const NSVGshape* shape, const Matrix4d& transform);

public:
    ~Shape() override = default;

    [[nodiscard]] const std::vector<Path>& GetPaths() const
    { return m_paths; }

    [[nodiscard]] const std::vector<Marker>& GetMarkers() const
    { return m_markers; }

    // Paint order across models, low first (`render_order` in the model's
    // toml, 0 if absent). Only matters between models that overlap and fill:
    // a planet's opaque interior has to go down before the structures nested
    // inside its outline, or it paints over them.
    [[nodiscard]] int GetRenderOrder() const
    { return m_renderOrder; }

    [[nodiscard]] std::size_t CalculateSize() const override;

    [[nodiscard]] const char* GetResourceName() const override
    { return "shape"; }

    static ResourcePtr<const Shape> Placeholder();

    static ResourcePtr<const Shape> MakePlaceholder();

    static ResourcePtr<const Shape> Create(id_t id, LoadingContext& context);
};

[[nodiscard]] std::size_t CalculateSize(const Shape::Path& path);

// Group tag of one ablative plate. Indices match Body::GetPlates(): both walk
// the '+plating' layer's paths in SVG document order, which is what lets the
// plate a hit was resolved against be the plate that lights up.
[[nodiscard]] id_t PlatingTag(std::size_t index);

// Group tag of the bubble shield's outline.
inline constexpr id_t SHIELD_TAG = IDC("+shield");

} // namespace Gravitaris
