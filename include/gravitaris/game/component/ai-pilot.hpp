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

// Tactical/temperament knobs for one AI pilot -- the tunable "personality"
// behind presets like Aggressive/Cautious (see gnc/ai-personality-presets.hpp).
// GuidanceParams/FlightControllerParams (also stored per-pilot on AIPilot)
// cover flight-dynamics tuning; this covers the decision layer above them.
struct AIPersonality {
    double engageRange = 500.0;      // pursue the target inside this
    double standoffDistance = 50.0;  // desired range to hold during Intercept
    double fireRange = 250.0;
    double fireTolerance = 0.12;     // rad off the lead solution, still fires

    // Gravity-well danger avoidance. A predicted approach inside evadeRadius
    // triggers Evade; once evading, the ship must clear evadeRadius*evadeMargin
    // before handing control back (hysteresis, avoids flapping at the boundary).
    double evadeRadius = 90.0;
    double evadeMargin = 1.5;
    int dangerLookaheadSteps = 120;  // ~2s at the fixed tick

    std::uint32_t decisionInterval = 15; // ticks between tactical re-evaluations
    std::uint32_t fireInterval = 30;     // ticks between shots

    // "Fuzzy"/character knobs -- 0 disables each. Deterministic per (entity,
    // tick) seed (see AIPilotSystem), so replays stay bit-exact.
    double reactionJitter = 0.0;     // +/- fraction of decisionInterval
    double aimJitter = 0.0;          // +/- rad added to fireTolerance per shot
    double dangerIgnoreChance = 0.0; // odds [0,1) of shrugging off a fresh
                                     // danger episode entirely (Reckless) --
                                     // rolled once per episode, not every
                                     // tick, so it can actually commit to
                                     // flying into the well rather than
                                     // re-rolling itself into evading anyway.
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
    AIPersonality personality;

    // Transient danger-episode tracking for AIPersonality::dangerIgnoreChance;
    // not part of the personality itself.
    bool wasInDanger = false;
    bool dangerSuppressed = false;
};

} // namespace Gravitaris
