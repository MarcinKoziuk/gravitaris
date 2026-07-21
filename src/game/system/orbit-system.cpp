#include <cmath>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/orbit-system.hpp>
#include <gravitaris/game/game.hpp>

namespace Gravitaris {

namespace {
constexpr double PI = 3.14159265358979323846;
} // namespace

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
        orbit.angularSpeed = angularSpeed; // cached for GatherSnapshot; see the field's own doc comment
        orbit.theta += angularSpeed * Game::PHYSICS_DELTA;
        // Wrapped into [0, 2*PI) rather than left to grow unbounded for as
        // long as the server process lives: cos/sin only care about theta
        // mod 2*PI, so this changes nothing physically, but it keeps the f32
        // wire truncation (EntityState::orbitTheta) precise indefinitely.
        // Unwrapped, a long-running server's theta grows into the thousands
        // of radians, and float32's ~7 significant digits then land an
        // increasingly coarse LSB on it -- every new snapshot re-bases
        // EvaluateOrbit's closed-form calculation from a freshly, and
        // independently, f32-quantized theta, so each rebase introduces a
        // small but real discontinuity that reads as the planet wobbling,
        // worse the longer the server's been up.
        orbit.theta = std::fmod(orbit.theta, 2.0 * PI);
        if (orbit.theta < 0.0) orbit.theta += 2.0 * PI;

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
