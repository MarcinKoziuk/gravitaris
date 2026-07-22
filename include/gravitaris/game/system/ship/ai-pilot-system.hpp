#pragma once

#include <cstdint>
#include <optional>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Tactics layer of the pilot stack: a small utility selector picks a guidance
// behavior per AI ship, the GNC stack turns it into control bits, and the
// result is pushed as a tick-stamped InputCommand. Runs before InputSystem so
// commands are consumed the same tick.
class AIPilotSystem {
private:
    flecs::world& m_registry;

    PhysicsSystem& m_physicsSystem;

    TrajectoryPredictor& m_predictor;

public:
    AIPilotSystem(flecs::world& registry, PhysicsSystem& physicsSystem,
                  TrajectoryPredictor& predictor);

    ~AIPilotSystem() = default;

    void Update(std::uint64_t step, std::optional<flecs::entity> player);
};

} // namespace Gravitaris
