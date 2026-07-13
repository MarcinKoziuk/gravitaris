#pragma once

#include <vector>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Navigation layer of the pilot GNC stack (docs/ai-ships.md, phase 1):
// forward-integrates a ship as a test particle through the gravity field of
// every other non-bullet body, sampled once at prediction time and held
// static ("restricted problem" -- planets outweigh ships by orders of
// magnitude, so their own motion over the horizon is negligible).
//
// The force law and integration mirror PhysicsSystem: F = G*m1*m2/d^2
// (PhysicsSystem::GRAVITY_CONSTANT) and symplectic Euler, which is what the
// sim's apply-forces-after-step + cpSpaceStep sequence works out to. The
// prediction still drifts from Chipmunk over time (floats, contact/collision
// effects, moving sources) -- consumers must replan from actual state rather
// than trust a stale path (MPC-style; see docs/ai-ships.md).
class TrajectoryPredictor {
private:
    flecs::world& m_registry;

    PhysicsSystem& m_physicsSystem;

public:
    TrajectoryPredictor(flecs::world& registry, PhysicsSystem& physicsSystem);

    ~TrajectoryPredictor() = default;

    // Predict `steps` ticks of `dt` seconds ahead from the ship's current
    // Transform. Returns steps+1 positions: index i = the ship's predicted
    // position i ticks from now (index 0 is its current position). Empty if
    // the entity has no Transform.
    std::vector<Magnum::Math::Vector2<double>> Predict(flecs::entity ship, int steps, double dt);
};

} // namespace Gravitaris
