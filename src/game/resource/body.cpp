#include <algorithm>
#include <cstring>
#include <optional>

#include <Magnum/Math/Matrix4.h>
#include <Magnum/Math/Vector4.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/resource/body.hpp>

#include "detail/shape-common.hpp"
#include "detail/casteljau.hpp"

namespace Gravitaris {

using Magnum::Vector4d;

typedef TVector2<cpFloat> cpvec2;

static std::optional<unsigned> MountIndex(const std::string& name, const char* prefix);
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
           + PodContainerContainerSize(m_polygonShapes)
           + PodContainerSize(m_shieldOutline)
           + PodContainerContainerSize(m_plates);
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

    const toml::table& cfg = shapeFiles->cfg;
    if (const toml::table* physicsCfg = cfg["physics"].as_table()) {
        if (const auto mass = (*physicsCfg)["mass"].value<float>()) {
            body->m_mass = *mass;
        }
        if (const auto friction = (*physicsCfg)["friction"].value<float>()) {
            body->m_friction = *friction;
        }
        if (const auto maxSpeed = (*physicsCfg)["max_speed"].value<float>()) {
            body->m_maxSpeed = *maxSpeed;
        }
        if (const auto thrust = (*physicsCfg)["thrust"].value<float>()) {
            body->m_thrust = *thrust;
        }
        if (const auto kinematic = (*physicsCfg)["kinematic"].value<bool>()) {
            body->m_kinematic = *kinematic;
        }
    }

    if (const toml::table* landingCfg = cfg["landing"].as_table()) {
        if (const auto fragility = (*landingCfg)["fragility"].value<float>()) {
            body->m_landingFragility = *fragility;
        }
    }

    if (const toml::table* weaponsCfg = cfg["weapons"].as_table()) {
        if (const auto arc = (*weaponsCfg)["aim_arc_degrees"].value<double>()) {
            body->m_aimArcDegrees = std::clamp(*arc, 0.0, 360.0);
        }
    }

    if (const toml::table* gravityCfg = cfg["gravity"].as_table()) {
        if (const auto isSource = (*gravityCfg)["is_source"].value<bool>()) {
            body->m_gravitySource = *isSource;
        }
        if (const auto multiplier = (*gravityCfg)["multiplier"].value<double>()) {
            body->m_gravityMultiplier = *multiplier;
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
        else if (std::strcmp(group, SHIELD_GROUP_LABEL) == 0) {
            body->AddShieldOutline(shape, transform);
        }
        else if (std::strcmp(group, PLATING_GROUP_LABEL) == 0) {
            body->AddPlates(shape, transform);
        }
    }

    if (!body->m_shieldOutline.empty() || !body->m_plates.empty()) {
        LOG(info) << "body " << path << ": shield outline " << body->m_shieldOutline.size()
                  << " pts, " << body->m_plates.size() << " ablative plates";
    }

    return body;
}

double Body::GetAimArcHalfWidth() const
{
    constexpr double PI = 3.141592653589793;
    return m_aimArcDegrees * PI / 360.0; // half of the arc, in radians
}

void Body::AddHardpoint(const NSVGshape* shape, const Matrix4d& transform)
{
    const char* id = shape->id;

    const Vector2d center = GetShapeCenter(shape);

    Hardpoint hardpoint;
    // Swizzles of a temporary alias its storage -- take the result by value
    // (see CLAUDE.md's Magnum gotcha).
    const Vector4d transformed = transform * Vector4d(center.x(), center.y(), 1., 1.);
    hardpoint.pos = transformed.xy();
    hardpoint.supports.weapons = true;
    hardpoint.name = shape->label;

    if (std::strcmp(id, "medium") == 0) {
        hardpoint.size = Body::Hardpoint::MEDIUM;
    }
    else if (std::strcmp(id, "small") == 0) {
        hardpoint.size = Body::Hardpoint::SMALL;
    }

    m_hardpoints.push_back(hardpoint);
}

const Body::Hardpoint* Body::FindHardpoint(const char* name) const
{
    for (const Hardpoint& hardpoint : m_hardpoints) {
        if (hardpoint.name == name) return &hardpoint;
    }
    return nullptr;
}

unsigned Body::CountMounts(const char* prefix) const
{
    unsigned count = 0;
    for (const Hardpoint& hardpoint : m_hardpoints) {
        if (MountIndex(hardpoint.name, prefix)) ++count;
    }
    return count;
}

const Body::Hardpoint* Body::FindMount(const char* prefix, unsigned nth) const
{
    const unsigned count = CountMounts(prefix);
    if (count == 0) return nullptr;

    // The `want`-th smallest index, found by a scan per rank rather than by
    // sorting: a hull carries a handful of mounts, and the authored indices may
    // be sparse (`gun_0`, `gun_2`) or out of document order.
    const unsigned want = nth % count;
    const Hardpoint* chosen = nullptr;
    unsigned chosenIndex = 0;
    for (unsigned rank = 0; rank <= want; ++rank) {
        const Hardpoint* best = nullptr;
        unsigned bestIndex = 0;
        for (const Hardpoint& hardpoint : m_hardpoints) {
            const std::optional<unsigned> index = MountIndex(hardpoint.name, prefix);
            if (!index) continue;
            if (chosen && *index <= chosenIndex) continue;
            if (!best || *index < bestIndex) {
                best = &hardpoint;
                bestIndex = *index;
            }
        }
        if (!best) break;
        chosen = best;
        chosenIndex = bestIndex;
    }
    return chosen;
}

void Body::AddShieldOutline(const NSVGshape* shape, const Matrix4d& transform)
{
    if (!m_shieldOutline.empty()) {
        LOG(warning) << "Model authors more than one " << SHIELD_GROUP_LABEL
                     << " path (" << shape->id << "); only the first is used";
        return;
    }

    const std::vector<Vector2d> outline = PathToPolyline(shape->paths, transform);
    if (outline.size() < 3) {
        LOG(error) << SHIELD_GROUP_LABEL << " path " << shape->id << " has less than 3 points";
        return;
    }

    // Even stride rather than a curvature-aware simplification: the bubble is
    // authored as a smooth convex loop, so every point is as good as any other
    // to keep, and dropping the rest is what keeps the hitbox polygon small.
    const std::size_t stride = (outline.size() + SHIELD_HULL_POINTS - 1) / SHIELD_HULL_POINTS;
    for (std::size_t i = 0; i < outline.size(); i += stride) {
        m_shieldOutline.emplace_back(outline[i]);
    }
}

void Body::AddPlates(const NSVGshape* shape, const Matrix4d& transform)
{
    for (const NSVGpath* path = shape->paths; path != nullptr; path = path->next) {
        if (m_plates.size() >= MAX_SHIELD_PLATES) {
            LOG(warning) << "Model authors more than " << MAX_SHIELD_PLATES << " "
                         << PLATING_GROUP_LABEL << " paths; the rest are ignored";
            return;
        }

        Plate plate;
        for (const Vector2d& pt : PathToPolyline(path, transform)) {
            plate.emplace_back(pt);
        }

        if (plate.size() < 2) {
            LOG(error) << PLATING_GROUP_LABEL << " path " << shape->id << " has less than 2 points";
            continue;
        }

        m_plates.push_back(std::move(plate));
    }
}

void Body::AddShape(const NSVGshape* shape, const Matrix4d& transform)
{
    if (IsCircle(shape)) {
        const CircleShape circle = ShapeToCircle(shape, transform);
        m_boundingRadius = std::max(m_boundingRadius, circle.pos.length() + circle.radius);
        m_circleShapes.push_back(circle);
    }
    else {
        const std::vector<std::vector<cpvec2>> polygons = ShapeToPolygons(shape, transform);

        for (const std::vector<cpvec2>& polygon : polygons) {
            for (const cpvec2& point : polygon) {
                m_boundingRadius = std::max(m_boundingRadius, point.length());
            }
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

// `gun` -> 0, `gun_2` -> 2, anything else -> none. A bare prefix is accepted so
// a hull carrying exactly one mount of a family needn't number it.
static std::optional<unsigned> MountIndex(const std::string& name, const char* prefix)
{
    const std::size_t length = std::strlen(prefix);
    if (name.compare(0, length, prefix) != 0) return std::nullopt;
    if (name.size() == length) return 0u;
    if (name[length] != '_') return std::nullopt;

    unsigned index = 0;
    for (std::size_t i = length + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return std::nullopt;
        index = index * 10 + static_cast<unsigned>(name[i] - '0');
    }
    return name.size() > length + 1 ? std::optional<unsigned>(index) : std::nullopt;
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
