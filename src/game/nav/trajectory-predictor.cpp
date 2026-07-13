#include <cmath>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/nav/trajectory-predictor.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

TrajectoryPredictor::TrajectoryPredictor(flecs::world& registry, PhysicsSystem& physicsSystem)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
{}

std::vector<Vector2d> TrajectoryPredictor::Predict(flecs::entity ship, int steps, double dt)
{
    const Transform* shipTransf = ship.try_get<Transform>();
    if (!shipTransf) return {};

    // Sample the gravity sources once and hold them static over the horizon:
    // the same set ApplyGravity attracts (every non-bullet body), minus the
    // ship itself.
    struct Source {
        Vector2d pos;
        double mass;
    };
    std::vector<Source> sources;

    m_registry.each([&](flecs::entity ent, Transform& transf, PhysicsRef& ref) {
        if (ent == ship || ent.has<Bullet>()) return;
        PhysicsBody& slot = m_physicsSystem.GetBody(ref);
        sources.push_back({transf.pos, cpBodyGetMass(slot.cp.body.get())});
    });

    Vector2d pos = shipTransf->pos;
    Vector2d vel = shipTransf->vel;

    std::vector<Vector2d> path;
    path.reserve(static_cast<std::size_t>(steps) + 1);
    path.push_back(pos);

    for (int i = 0; i < steps; ++i) {
        Vector2d accel{0.0, 0.0};
        for (const Source& src : sources) {
            const Vector2d d = src.pos - pos;
            const double distSq = d.dot();
            // The sim has no softening either; this guard only avoids inf/NaN
            // at near-contact, where the prediction is meaningless anyway
            // (collision response takes over in the real sim).
            if (distSq < 1e-6) continue;
            // a = F/m_ship = G * m_src / d^2, directed at the source --
            // d / |d| * (G*m/d^2) folded into one multiply.
            accel += d * (PhysicsSystem::GRAVITY_CONSTANT * src.mass / (distSq * std::sqrt(distSq)));
        }

        // Symplectic Euler, matching the effective integration of the sim's
        // cpSpaceStep + apply-forces-after-step sequence.
        vel += accel * dt;
        pos += vel * dt;
        path.push_back(pos);
    }

    return path;
}

} // namespace Gravitaris
