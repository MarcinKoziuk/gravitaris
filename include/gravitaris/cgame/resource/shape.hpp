#pragma once

#include <vector>
#include <memory>
#include <optional>

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

private:
    std::vector<Path> m_paths;

    static ResourcePtr<const Shape> placeholder;

    void AddPaths(const NSVGshape* shape, const Matrix4d& transform, id_t group);

public:
    ~Shape() override = default;

    [[nodiscard]] const std::vector<Path>& GetPaths() const
    { return m_paths; }

    [[nodiscard]] std::size_t CalculateSize() const override;

    [[nodiscard]] const char* GetResourceName() const override
    { return "shape"; }

    static ResourcePtr<const Shape> Placeholder();

    static ResourcePtr<const Shape> MakePlaceholder();

    static ResourcePtr<const Shape> Create(id_t id, LoadingContext& context);
};

[[nodiscard]] std::size_t CalculateSize(const Shape::Path& path);

} // namespace Gravitaris
