#include <cmath>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/gnc/nav/trajectory-predictor.hpp>

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

    struct Source {
        Vector2d pos;
        double mass;
    };
    std::vector<Source> sources;

    m_registry.each([&](flecs::entity, const Transform& transf, const GravitySource& gs) {
        sources.push_back({transf.pos, gs.mass * gs.multiplier});
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
            // Guard against inf/NaN at near-contact; the sim has no softening
            // either, but there collision response takes over.
            if (distSq < 1e-6) continue;
            // a = G * m_src / d^2 toward the source (unit(d) folded in).
            accel += d * (PhysicsSystem::GRAVITY_CONSTANT * src.mass / (distSq * std::sqrt(distSq)));
        }

        vel += accel * dt;
        pos += vel * dt;
        path.push_back(pos);
    }

    return path;
}

} // namespace Gravitaris
