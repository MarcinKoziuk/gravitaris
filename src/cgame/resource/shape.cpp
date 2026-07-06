#include <cstring>

#include <Magnum/Math/Matrix4.h>

#include <nanosvg/nanosvg.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/cgame/resource/shape.hpp>

#include "game/resource/detail/shape-common.hpp"
#include "game/resource/detail/casteljau.hpp"

namespace Gravitaris {

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

    NSVGimage& svg = *shapeFiles->svg;

    for (const NSVGshape* svgShape = svg.shapes; svgShape != nullptr; svgShape = svgShape->next) {
        const char* group = svgShape->groupLabel;

        if (std::strlen(group) > 0 && group[0] == GROUP_LABEL_PREFIX) {
            continue;
        }

        shape->AddPaths(svgShape, transform, ID(group));
    }

    return shape;
}

void Shape::AddPaths(const NSVGshape* shape, const Matrix4d& transform, id_t group)
{
    Shape::Style style;
    style.thickness = shape->strokeWidth;
    style.color = Color4(0.f, 0.f, 1.f, 1.f);
    style.color.w() = shape->opacity;

    if (shape->stroke.type == NSVG_PAINT_COLOR) {
        const unsigned currentColor = shape->stroke.color;
        if (currentColor != 0xff000000 && currentColor != 0xffffffff) {
            style.color = Hex3ToNormalizedColor4(shape->stroke.color, style.color.w());
        }
    }

    const auto paths = ShapeToPaths(shape, style, transform, group);
    m_paths.insert(m_paths.end(), paths.begin(), paths.end());
}

static std::vector<Shape::Path> ShapeToPaths(const NSVGshape* shape, Shape::Style style, const Matrix4d& transform, id_t group)
{
    std::vector<Shape::Path> paths;

    for (const NSVGpath* svgPath = shape->paths; svgPath != nullptr; svgPath = svgPath->next) {
        Shape::Path path;
        path.style = style;
        path.closed = svgPath->closed == 1;
        path.group = group;

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
