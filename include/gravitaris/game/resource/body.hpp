#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>

#include <Magnum/Magnum.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>
#include <gravitaris/game/resource/common/iresource.hpp>
#include <gravitaris/game/resource/common/resource-ptr.hpp>

#include <chipmunk/chipmunk_types.h>

struct NSVGshape;

namespace Gravitaris {

template<typename T>
using TVector2 = Magnum::Math::Vector2<T>;

using Magnum::Vector2d;
using Magnum::Matrix4d;

class Body : public IResource {
public:
    struct CircleShape {
        TVector2<cpFloat> pos;
        cpFloat radius;

        CircleShape(const TVector2<cpFloat>& pos, cpFloat radius)
                : pos(pos), radius(radius) {}
    };

    struct Hardpoint {
        enum Size { TINY, SMALL, MEDIUM, LARGE, XLARGE };

        Size size;
        struct {
            bool weapons : 1;
            bool sensors : 1;
            bool utility : 1;
        } supports;
        Vector2d pos;

        Hardpoint() : size(TINY), supports{false, false, false} {}
    };

private:
    cpFloat m_mass;
    cpFloat m_friction;
    std::vector<CircleShape> m_circleShapes;
    std::vector<std::vector<TVector2<cpFloat>>> m_polygonShapes;
    std::vector<Hardpoint> m_hardpoints;

    static ResourcePtr<const Body> placeholder;

    void AddShape(const NSVGshape* shape, const Matrix4d& transform);
    void AddHardpoint(const NSVGshape* shape, const Matrix4d& transform);

public:
    Body();

    ~Body() override = default;

    [[nodiscard]] std::size_t CalculateSize() const override;

    [[nodiscard]] const char* GetResourceName() const override
    { return "body"; }

    [[nodiscard]] cpFloat GetMass() const
    { return m_mass; }

    [[nodiscard]] cpFloat GetFriction() const
    { return m_friction; }

    [[nodiscard]] const std::vector<CircleShape>& GetCircleShapes() const
    { return m_circleShapes; }

    [[nodiscard]] const std::vector<std::vector<TVector2<cpFloat>>>& GetPolygonShapes() const
    { return m_polygonShapes; }

    [[nodiscard]] const std::vector<Hardpoint>& GetHardpoints() const
    { return m_hardpoints; }

    static ResourcePtr<const Body> Placeholder();

    static ResourcePtr<const Body> Create(id_t id, LoadingContext& context);
};

} // namespace Gravitaris
