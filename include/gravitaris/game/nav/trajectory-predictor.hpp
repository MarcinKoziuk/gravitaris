#pragma once

#include <vector>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// GNC navigation layer (docs/ai-ships.md): forward-integrates a ship as a
// test particle through the gravity field of every other non-bullet body,
// sampled once and held static. Matches the sim's force law and effective
// integration (symplectic Euler), but still drifts from Chipmunk over time --
// consumers must replan from actual state rather than trust a stale path.
class TrajectoryPredictor {
private:
    flecs::world& m_registry;

    PhysicsSystem& m_physicsSystem;

public:
    TrajectoryPredictor(flecs::world& registry, PhysicsSystem& physicsSystem);

    ~TrajectoryPredictor() = default;

    // Returns steps+1 positions: index i = predicted position i ticks from
    // now (index 0 = current). Empty if the entity has no Transform.
    std::vector<Magnum::Math::Vector2<double>> Predict(flecs::entity ship, int steps, double dt);
};

} // namespace Gravitaris
