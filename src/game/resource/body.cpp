#include <cstring>

#include <Magnum/Math/Matrix4.h>
#include <Magnum/Math/Vector4.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/resource/body.hpp>

#include "detail/shape-common.hpp"
#include "detail/casteljau.hpp"

namespace Gravitaris {

using Magnum::Vector4d;

typedef TVector2<cpFloat> cpvec2;

static bool IsCircle(const NSVGshape* shape);
static std::vector<std::vector<cpvec2>> ShapeToPolygons(const NSVGshape* shape, const Matrix4d& transform);
static Body::CircleShape ShapeToCircle(const NSVGshape* shape, const Matrix4d& transform);

ResourcePtr<const Body> Body::placeholder = MakeResourcePtr<Body>("body-placeholder"_id);

Body::Body()
        : m_mass(1.f)
        , m_friction(0.f)
{}

std::size_t Body::CalculateSize() const
{
    return sizeof(*this)
           + PodContainerSize(m_circleShapes)
           + PodContainerContainerSize(m_polygonShapes);
}

ResourcePtr<const Body> Body::Placeholder()
{
    return Body::placeholder;
}

ResourcePtr<const Body> Body::Create(id_t id, LoadingContext& context)
{
    auto body = MakeResourcePtr<Body>(id);
    std::shared_ptr<Body>(nullptr);

    std::string path;
    if (!context.filesystem.GetPathForId(id, path)) {
        return nullptr;
    }

    std::unique_ptr<ShapeFiles> shapeFiles = GetShapeFiles(context.filesystem, path);
    Matrix4d transform = GetTransformMatrix(*shapeFiles);

    const YAML::Node& cfg = shapeFiles->cfg;
    if (cfg["physics"]) {
        const YAML::Node& physicsCfg = cfg["physics"];

        if (physicsCfg["mass"]) {
            body->m_mass = physicsCfg["mass"].as<float>();
        }
        if (physicsCfg["friction"]) {
            body->m_friction = physicsCfg["friction"].as<float>();
        }
    }

    NSVGimage& svg = *shapeFiles->svg;

    for (const NSVGshape* shape = svg.shapes; shape != nullptr; shape = shape->next) {
        const char* group = shape->groupLabel;
        //const char* label = shape->label;

        if (std::strcmp(group, BODY_GROUP_LABEL) == 0) {
            body->AddShape(shape, transform);
        }
        else if (std::strcmp(group, HARDPOINTS_GROUP_LABEL) == 0) {
            body->AddHardpoint(shape, transform);
        }
    }

    return body;
}

void Body::AddHardpoint(const NSVGshape* shape, const Matrix4d& transform)
{
    const char* name = shape->id;

    const Vector2d center = GetShapeCenter(shape);

    Hardpoint hardpoint;
    hardpoint.pos = (transform * Vector4d(center.x(), center.y(), 1., 1.)).xy();
    hardpoint.supports.weapons = true;

    if (std::strcmp(name, "medium") != 0) {
        hardpoint.size = Body::Hardpoint::MEDIUM;
    }
    else if (std::strcmp(name, "small") != 0) {
        hardpoint.size = Body::Hardpoint::SMALL;
    }

    m_hardpoints.push_back(hardpoint);
}

void Body::AddShape(const NSVGshape* shape, const Matrix4d& transform)
{
    if (IsCircle(shape)) {
        const CircleShape circle = ShapeToCircle(shape, transform);
        m_circleShapes.push_back(circle);
    }
    else {
        const std::vector<std::vector<cpvec2>> polygons = ShapeToPolygons(shape, transform);

        for (const std::vector<cpvec2>& polygon : polygons) {
            m_polygonShapes.push_back(polygon);
        }
    }
}

static Body::CircleShape ShapeToCircle(const NSVGshape* shape, const Matrix4d& transform)
{
    const NSVGpath* path = shape->paths;
    const cpvec2 min((transform * Vector4d(path->bounds[0], path->bounds[1], 1.f, 1.f)).xy());
    const cpvec2 max((transform * Vector4d(path->bounds[2], path->bounds[3], 1.f, 1.f)).xy());

    double radius = (max.y() - min.y()) / 2.;
    cpvec2 pos(min.x() + radius, min.y() + radius);

    return {pos, radius};
}

static std::vector<std::vector<cpvec2>> ShapeToPolygons(const NSVGshape* shape, const Matrix4d& transform)
{
    std::vector<std::vector<cpvec2>> lines;

    for (const NSVGpath* path = shape->paths; path != nullptr; path = path->next) {
        std::stringstream pathInfo;
        pathInfo << "(" << shape->id << " of " << path << ")";

        if (!path->closed) {
            LOG(warning) << "SVG body path is not closed in model " << pathInfo.str();
        }

        std::vector<cpvec2> points;

        std::vector<Vector2d> cubicBezier;
        for (int i = 0; i < path->npts - 1; i += 3) {
            float* p = &path->pts[i * 2];
            cubicBezier.emplace_back(p[0], p[1]);
            cubicBezier.emplace_back(p[2], p[3]);
            cubicBezier.emplace_back(p[4], p[5]);
            cubicBezier.emplace_back(p[6], p[7]);
        }

        std::vector<Vector2d> res = Casteljau(cubicBezier, transform, path->closed);
        for (const Vector2d& pt : res) {
            points.emplace_back(pt);
        }

        if (points.size() < 3) {
            LOG(error) << "SVG body path has less than 3 points " << pathInfo.str();
            continue;
        }

        /*if (points.size() > 8) {
            int reduction = std::ceil((double)points.size() / 8.);

            LOG(warning) << "SVG body path has more than 8 points ("
                << points.size() << "), reducing x" << reduction << " " << pathInfo.str();


            std::vector<cpvec2> newpoints;
            for (std::size_t i = 0; i < points.size(); i++) {
                if (i % reduction == 0) {
                    newpoints.push_back(points[i]);
                }
            }

            points.swap(newpoints);
        }*/

        lines.push_back(points);
    }

    return lines;
}

static bool IsCircle(const NSVGshape* shape)
{
    const NSVGpath* p = shape->paths;
    const double diff = std::abs((p->bounds[2] - p->bounds[0]) - (p->bounds[3] - p->bounds[1]));

    return p->closed
           && p->npts == 16
           && p->next == nullptr
           && diff < 0.001;
}

} // namespace Gravitaris
