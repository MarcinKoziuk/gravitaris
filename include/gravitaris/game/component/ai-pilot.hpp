#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/gnc/control/flight-controller.hpp>
#include <gravitaris/game/gnc/guidance/behaviors.hpp>

namespace Gravitaris {

enum class AIBehavior {
    Idle,
    Evade,     // climbing out of a gravity well
    Intercept, // pursuing the target
    Orbit,     // patrolling the heaviest body
};

// Server-only pilot state, never serialized (ADR 0001). AIPilotSystem
// re-evaluates the behavior on a decision interval and pushes one
// InputCommand per tick through the ship's InputQueue -- the same seam
// human input uses.
struct AIPilot {
    AIBehavior behavior = AIBehavior::Idle;
    flecs::entity target; // becomes NetId when netcode lands
    std::uint32_t decisionCooldown = 0;
    std::uint32_t fireCooldown = 0;

    // Captured when entering Orbit so the patrol ring stays put.
    double patrolRadius = 0.0;
    double patrolDirection = 1.0;

    FlightControllerParams flight;
    GuidanceParams guidance;
};

} // namespace Gravitaris
