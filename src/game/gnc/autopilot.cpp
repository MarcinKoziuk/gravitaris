#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>

#include <gravitaris/game/gnc/autopilot.hpp>

namespace Gravitaris {

Autopilot::Autopilot(flecs::world& registry, PhysicsSystem& physicsSystem)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
{}

std::optional<Autopilot::GravitySource> Autopilot::FindHeaviestGravitySource()
{
    // GravitySource.mass, not cpBodyGetMass: celestials are kinematic bodies
    // (infinite Chipmunk mass), so their gravitational mass lives in the
    // component instead (see include/gravitaris/game/component/gravity-source.hpp).
    std::optional<GravitySource> best;
    m_registry.each([&](flecs::entity, const Transform& transf, const Gravitaris::GravitySource& gs) {
        const double mass = gs.mass * gs.multiplier;
        if (!best || mass > best->mass) {
            best = GravitySource{transf.pos, mass};
        }
    });
    return best;
}

void Autopilot::SetMode(AutopilotMode mode, std::optional<flecs::entity> player)
{
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;

    if (mode != AutopilotMode::Off) {
        if (!transform) return;
        const PhysicsRef* ref = player->try_get<PhysicsRef>();
        if (ref) {
            const double mass = cpBodyGetMass(m_physicsSystem.GetBody(*ref).cp.body.get());
            m_guidanceParams.accel = ShipControlsSystem::THRUST_FORCE / mass;
        }
    }

    if (mode == AutopilotMode::HoldPosition) {
        m_anchor = transform->pos;
    }

    if (mode == AutopilotMode::Orbit) {
        const std::optional<GravitySource> source = FindHeaviestGravitySource();
        if (!source) return;
        m_orbitCenter = source->pos;
        m_orbitMass = source->mass;

        const Magnum::Math::Vector2<double> r = transform->pos - m_orbitCenter;
        m_orbitRadius = r.length();

        // Keep the current sense of rotation; default counter-clockwise.
        const double cross = r.x() * transform->vel.y() - r.y() * transform->vel.x();
        m_orbitDirection = (cross < 0.0) ? -1.0 : 1.0;
    }

    m_mode = mode;
}

std::optional<ControlFlags> Autopilot::ComputeControls(std::optional<flecs::entity> player)
{
    if (m_mode == AutopilotMode::Off) return std::nullopt;

    const Transform* transform = player ? player->try_get<Transform>() : nullptr;
    if (!transform) return std::nullopt;

    Magnum::Math::Vector2<double> desiredVel{0.0, 0.0};
    switch (m_mode) {
        case AutopilotMode::KillVelocity:
            break;
        case AutopilotMode::HoldPosition:
            desiredVel = HoldPositionDesiredVelocity(*transform, m_anchor, m_flightParams);
            break;
        case AutopilotMode::GotoPoint:
            desiredVel = GotoPoint(*transform, m_gotoTarget, m_guidanceParams);
            break;
        case AutopilotMode::Orbit:
            desiredVel = OrbitBody(*transform, m_orbitCenter, m_orbitMass,
                                   m_orbitRadius, m_orbitDirection, m_guidanceParams);
            break;
        case AutopilotMode::Off:
            return std::nullopt;
    }

    return FlyToVelocity(*transform, desiredVel, m_flightParams);
}

} // namespace Gravitaris
