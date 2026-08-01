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
#include <gravitaris/game/upgrade/upgrade-def.hpp>

#include <chipmunk/chipmunk_types.h>

struct NSVGshape;

namespace Gravitaris {

template<typename T>
using TVector2 = Magnum::Math::Vector2<T>;

using Magnum::Vector2d;
using Magnum::Matrix4d;

class Body : public IResource {
public:
    // Forward thrust force (local -Y) of a hull that doesn't name its own in
    // [physics] thrust. A hull that can land has to be able to lift off
    // again, so this sits above the ~164 units/s^2 surface gravity of a
    // standard planet rather than on a round number: below it a ship on the
    // deck cannot leave, whoever is flying it.
    static constexpr cpFloat DEFAULT_THRUST = 220.0;

    // Speed past which a hull's own thruster stops adding speed, for a hull
    // that doesn't name its own in [physics] max_speed. Well clear of the
    // ~30-50 units/s a circular orbit runs at over the planet radii in use,
    // so ordinary flying never touches it, and above the 70 units/s that
    // starts doing landing damage. Zero means uncapped.
    //
    // Gravity is deliberately NOT subject to this -- see
    // ShipControlsSystem::ApplyMovement. Slingshotting past your engine's
    // limit is the point.
    static constexpr cpFloat DEFAULT_MAX_SPEED = 400.0;

    // Vertex budget for the bubble's collision polygon. The authored outline
    // flattens to far more points than a convex hitbox needs; sampling it down
    // to this many keeps the Chipmunk poly cheap without visibly shrinking it.
    static constexpr std::size_t SHIELD_HULL_POINTS = 20;

    // One '+plating' path, an open polyline in the same post-transform space
    // as the collision shapes. Kept in SVG document order -- the renderer bakes
    // its line strips in that same order, which is the whole reason a plate
    // index means the same thing to the sim and to the shader.
    using Plate = std::vector<TVector2<cpFloat>>;

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
        // The Inkscape layer *label* of the marker (not its id), so a model
        // can name a point the game looks up by meaning -- "spawn" on a High
        // Port is where a fighter's feet go when it launches from the deck.
        std::string name;

        Hardpoint() : size(TINY), supports{false, false, false} {}
    };

private:
    cpFloat m_mass;
    cpFloat m_friction;
    cpFloat m_thrust = DEFAULT_THRUST;
    cpFloat m_maxSpeed = DEFAULT_MAX_SPEED;
    float m_landingFragility = 1.f;
    bool m_kinematic = false;
    bool m_gravitySource = false;
    double m_gravityMultiplier = 1.0;
    std::vector<CircleShape> m_circleShapes;
    std::vector<std::vector<TVector2<cpFloat>>> m_polygonShapes;
    std::vector<Hardpoint> m_hardpoints;
    std::vector<TVector2<cpFloat>> m_shieldOutline;
    std::vector<Plate> m_plates;

    static ResourcePtr<const Body> placeholder;

    void AddShape(const NSVGshape* shape, const Matrix4d& transform);
    void AddHardpoint(const NSVGshape* shape, const Matrix4d& transform);
    void AddShieldOutline(const NSVGshape* shape, const Matrix4d& transform);
    void AddPlates(const NSVGshape* shape, const Matrix4d& transform);

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

    // Force the single rear thruster delivers (ShipControlsSystem::
    // ApplyMovement); divided by the live mass it is the acceleration
    // guidance plans against.
    [[nodiscard]] cpFloat GetThrust() const
    { return m_thrust; }

    // Speed at which this hull's thruster stops being able to add speed
    // along the direction of travel. Zero means uncapped.
    [[nodiscard]] cpFloat GetMaxSpeed() const
    { return m_maxSpeed; }

    // Multiplier on the landing damage this hull takes (DamageSystem) -- not
    // on fire taken, not on rams. Below 1 is heavier gear that shrugs off a
    // hard set-down, above 1 is a hull that folds.
    [[nodiscard]] float GetLandingFragility() const
    { return m_landingFragility; }

    // Kinematic: an immovable body whose motion is driven externally (e.g. by
    // OrbitSystem), unaffected by collisions, forces or gravity.
    [[nodiscard]] bool IsKinematic() const
    { return m_kinematic; }

    // Whether this body attracts others (a star/planet). Its gravitational
    // mass is GetMass(); the physical Chipmunk mass may be infinite (kinematic).
    [[nodiscard]] bool IsGravitySource() const
    { return m_gravitySource; }

    [[nodiscard]] double GetGravityMultiplier() const
    { return m_gravityMultiplier; }

    [[nodiscard]] const std::vector<CircleShape>& GetCircleShapes() const
    { return m_circleShapes; }

    [[nodiscard]] const std::vector<std::vector<TVector2<cpFloat>>>& GetPolygonShapes() const
    { return m_polygonShapes; }

    // The bubble shield's outline (the '+shield' layer), closed and wound as
    // authored, already reduced to SHIELD_HULL_POINTS or fewer. Empty for a
    // hull that has no bubble authored, which is what makes the bubble
    // hitbox opt-in per model rather than a fallback circle for everyone.
    [[nodiscard]] const std::vector<TVector2<cpFloat>>& GetShieldOutline() const
    { return m_shieldOutline; }

    [[nodiscard]] const std::vector<Plate>& GetPlates() const
    { return m_plates; }

    // The named hardpoint, or nullptr when this model has none by that name.
    [[nodiscard]] const Hardpoint* FindHardpoint(const char* name) const;

    [[nodiscard]] const std::vector<Hardpoint>& GetHardpoints() const
    { return m_hardpoints; }

    static ResourcePtr<const Body> Placeholder();

    static ResourcePtr<const Body> Create(id_t id, LoadingContext& context);
};

} // namespace Gravitaris
