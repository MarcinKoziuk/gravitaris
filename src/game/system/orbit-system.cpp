#include <cmath>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/orbit-system.hpp>
#include <gravitaris/game/game.hpp>

namespace Gravitaris {

OrbitSystem::OrbitSystem(flecs::world& registry, PhysicsSystem& physicsSystem)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
{}

void OrbitSystem::Update()
{
    const double gravityMultiplier = m_physicsSystem.GetGravityMultiplier();

    m_registry.each([&](flecs::entity, Transform& transf, PhysicsRef& ref, Orbit& orbit) {
        if (orbit.radius <= 0.0) return;

        // Circular-orbit angular speed at this radius under the current
        // gravity field: v = sqrt(G*mult*M/r), angularSpeed = v/r. Recomputed
        // every tick so a live gravity-multiplier change (debug slider) is
        // reflected immediately, same as it is for a freely flying ship.
        const double angularSpeed = orbit.direction * std::sqrt(
                PhysicsSystem::GRAVITY_CONSTANT * gravityMultiplier * orbit.centerMass
                / (orbit.radius * orbit.radius * orbit.radius));
        orbit.theta += angularSpeed * Game::PHYSICS_DELTA;

        const double c = std::cos(orbit.theta);
        const double s = std::sin(orbit.theta);

        const Vector2d pos = orbit.center + Vector2d{c, s} * orbit.radius;
        const Vector2d vel = Vector2d{-s, c} * (angularSpeed * orbit.radius);

        m_physicsSystem.SetKinematicMotion(ref, pos, vel);
        transf.pos = pos;
        transf.vel = vel;
    });
}

} // namespace Gravitaris
