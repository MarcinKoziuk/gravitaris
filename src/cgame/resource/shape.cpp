#include <cstring>

#include <Magnum/Math/Vector3.h>
#include <Magnum/Math/Matrix4.h>

#include <nanosvg/nanosvg.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/cgame/resource/shape.hpp>
#include <gravitaris/cgame/team-color.hpp>

#include "game/resource/detail/shape-common.hpp"
#include "game/resource/detail/casteljau.hpp"

namespace Gravitaris {

using Magnum::Vector3d;
using Magnum::Vector4d;

static std::vector<Shape::Path> ShapeToPaths(const NSVGshape* shape, Shape::Style style, const Matrix4d& transform, id_t group);

ResourcePtr<const Shape> Shape::placeholder = Shape::MakePlaceholder();

std::size_t Shape::CalculateSize() const
{
    return sizeof(*this) + SizeAwareContainerSize(m_paths);
}

ResourcePtr<const Shape> Shape::Placeholder()
{
    return Shape::placeholder;
}

ResourcePtr<const Shape> Shape::Create(id_t id, LoadingContext& context)
{
    auto shape = MakeResourcePtr<Shape>(id);

    std::string path;
    if (!context.filesystem.GetPathForId(id, path)) {
        return nullptr;
    }

    std::unique_ptr<ShapeFiles> shapeFiles = GetShapeFiles(context.filesystem, path);
    Matrix4d transform = GetTransformMatrix(*shapeFiles);

    if (const auto renderOrder = shapeFiles->cfg["render_order"].value<int>()) {
        shape->m_renderOrder = *renderOrder;
    }

    NSVGimage& svg = *shapeFiles->svg;

    // Runs across shapes, not within one, because a plate index has to mean
    // the same path to the renderer as it does to Body::GetPlates -- which
    // walks the same SVG paths in the same order.
    std::size_t plateIndex = 0;

    for (const NSVGshape* svgShape = svg.shapes; svgShape != nullptr; svgShape = svgShape->next) {
        const char* group = svgShape->groupLabel;

        if (std::strlen(group) > 0 && group[0] == GROUP_LABEL_PREFIX) {
            if (std::strcmp(group, SLOTS_GROUP_LABEL) == 0) shape->AddMarker(svgShape, transform);
            continue;
        }

        const bool plating = std::strcmp(group, PLATING_GROUP_LABEL) == 0;
        shape->AddPaths(svgShape, transform, ID(group), group[0] == FX_LABEL_PREFIX,
                        plating ? &plateIndex : nullptr);
    }

    return shape;
}

id_t PlatingTag(std::size_t index)
{
    const std::string label = std::string(PLATING_GROUP_LABEL) + std::to_string(index);
    return ID(label.c_str());
}

void Shape::AddPaths(const NSVGshape* shape, const Matrix4d& transform, id_t group, bool fxLayer,
                     std::size_t* plateIndex)
{
    Shape::Style style;
    style.thickness = shape->strokeWidth;
    style.color = Color4(0.f, 0.f, 1.f, 1.f);
    style.color.w() = shape->opacity;

    // A '+' layer is drawn in whatever color the game decides for it that
    // frame (team color, hit glow, per-plate charge), so its authored stroke
    // is discarded here rather than being kept in sync by hand in Inkscape.
    if (fxLayer) {
        style.color = Color4(TEAM_COLOR_PLACEHOLDER, style.color.w());
    }
    else if (shape->stroke.type == NSVG_PAINT_COLOR) {
        const unsigned currentColor = shape->stroke.color;
        if (currentColor != 0xff000000 && currentColor != 0xffffffff) {
            style.color = Hex3ToNormalizedColor4(shape->stroke.color, style.color.w());
        }
    }

    // Fill is read verbatim -- unlike the stroke above, black is meaningful
    // (a black planet interior that blocks the starfield), so it isn't skipped.
    if (shape->fill.type == NSVG_PAINT_COLOR) {
        style.fillColor = Hex3ToNormalizedColor4(shape->fill.color, shape->opacity);
        style.hasFill = true;
    }

    auto paths = ShapeToPaths(shape, style, transform, group);

    // Each plate becomes its own group, so the renderer can hand it its own
    // instance color -- that is what lets one plate light while its neighbour
    // stays dark, without any per-plate machinery in the shader.
    if (plateIndex != nullptr) {
        for (Shape::Path& path : paths) {
            path.group = PlatingTag((*plateIndex)++);
        }
    }

    m_paths.insert(m_paths.end(), paths.begin(), paths.end());
}

void Shape::AddMarker(const NSVGshape* shape, const Matrix4d& transform)
{
    if (std::strlen(shape->label) == 0) {
        LOG(warning) << "Model authors an unlabelled " << SLOTS_GROUP_LABEL
                     << " element (" << shape->id << "); it has no name to be found by";
        return;
    }

    const Vector2d center = GetShapeCenter(shape);
    // Swizzles of a temporary alias its storage -- take the result by value
    // (see CLAUDE.md's Magnum gotcha).
    const Vector4d transformed = transform * Vector4d(center.x(), center.y(), 1., 1.);

    m_markers.push_back(Marker{shape->label, transformed.xy()});
}

static std::vector<Shape::Path> ShapeToPaths(const NSVGshape* shape, Shape::Style style, const Matrix4d& transform, id_t group)
{
    std::vector<Shape::Path> paths;

    for (const NSVGpath* svgPath = shape->paths; svgPath != nullptr; svgPath = svgPath->next) {
        Shape::Path path;
        path.style = style;
        path.closed = svgPath->closed == 1;
        path.group = group;

        if (CircleInfo circleInfo; IsCircleGeometry(svgPath, &circleInfo)) {
            const Vector3d centerT = transform.transformPoint(
                    Vector3d(circleInfo.center.x(), circleInfo.center.y(), 0.));
            const double scale = transform.transformVector(Vector3d(1., 0., 0.)).length();
            path.circle = Shape::Circle{Vector2d(centerT.x(), centerT.y()), circleInfo.radius * scale};
        }

#if 0
        std::vector<Vector2> cubicBezier;
		for (int i = 0; i < svgPath->npts - 1; i += 3) {
			float* p = &svgPath->pts[i * 2];
			cubicBezier.push_back(Vector2(p[0], p[1]));
			cubicBezier.push_back(Vector2(p[2], p[3]));
			cubicBezier.push_back(Vector2(p[4], p[5]));
			cubicBezier.push_back(Vector2(p[6], p[7]));
		}

		path.points = Casteljau(cubicBezier, transform, 0 /*path.closed*/);
		//path.points = CasteljauRaw(cubicBezier);
#endif

        for (int i = 0; i < svgPath->npts - 1; i += 3) {
            float* p = &svgPath->pts[i * 2];
            std::vector<Vector2d> cubicBezier;
            cubicBezier.emplace_back(p[0], p[1]);
            cubicBezier.emplace_back(p[2], p[3]);
            cubicBezier.emplace_back(p[4], p[5]);
            cubicBezier.emplace_back(p[6], p[7]);

            std::vector<Vector2d> approximatedBezier = Casteljau(cubicBezier, transform, false);

            for (const auto& bp : approximatedBezier) {
                if (path.points.empty() || path.points.back() != bp)
                    path.points.push_back(bp);
            }
        }

        if (path.closed && !path.points.empty() && path.points.front() == path.points.back()) {
            path.points.pop_back();
        }

        if (path.points.size() < 2) {
            LOG(error) << "Path in " << shape->groupLabel << ", has less than 2 points (" << path.points.size() << ")";
            continue;
        }

        paths.push_back(path);
    }

    return paths;
}

ResourcePtr<const Shape> Shape::MakePlaceholder()
{
    auto shape = MakeResourcePtr<Shape>("shape-placeholder"_id);

    Style outer;
    outer.color = Color4(1.f, 0.f, 0.f, 1.f);
    outer.thickness = 1.f;

    Style inner;
    inner.color = Color4(1.f, 0.f, 1.f, .8f);
    inner.thickness = .5f;

    // BOX
    Path p1;
    p1.style = outer;
    p1.closed = true;
    p1.points = {
            {-1.0, 1.0},
            {-1.0, -1.0},
            {1.0, -1.0},
            {1.0, 1.0}
    };

    // Diagonal \ of X
    Path p2;
    p2.style = inner;
    p2.closed = false;
    p2.points = {
            { -1, 1 },
            { 1, -1 }
    };

    // Diagonal / of X
    Path p3;
    p3.style = inner;
    p3.closed = false;
    p3.points = {
        { -1, -1 },
        { 1, 1 }
    };

    shape->m_paths.push_back(p1);
    shape->m_paths.push_back(p2);
    shape->m_paths.push_back(p3);

    return shape;
}

std::size_t CalculateSize(const Shape::Path& path)
{
    return sizeof(path) + PodContainerSize(path.points);
}

} // namespace Gravitaris
