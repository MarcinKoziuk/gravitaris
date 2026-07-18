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
    double engageRange = 6000.0;     // pursue the target inside this
    double standoffDistance = 50.0;  // desired range to hold during Intercept
    double fireRange = 350.0;        // opens fire this far out (was 250)
    double fireTolerance = 0.10;     // rad off the lead solution, still fires

    // Gravity-well danger avoidance. A predicted approach inside evadeRadius
    // triggers Evade; once evading, the ship must clear evadeRadius*evadeMargin
    // before handing control back (hysteresis, avoids flapping at the boundary).
    double evadeRadius = 90.0;
    double evadeMargin = 1.5;
    int dangerLookaheadSteps = 120;  // ~2s at the fixed tick

    std::uint32_t decisionInterval = 15; // ticks between tactical re-evaluations

    // Firing cadence: burstCount shots fired burstShotInterval ticks apart,
    // then fireInterval ticks of cooldown before the next burst starts.
    // burstCount = 1 is a plain single-shot cadence (burstShotInterval
    // unused); fireInterval is what gates the wait between shots either way.
    std::uint32_t fireInterval = 8;       // ticks of cooldown after a burst completes
    std::uint32_t burstCount = 1;         // shots fired back-to-back per opportunity
    std::uint32_t burstShotInterval = 6;  // ticks between shots within a burst

    // "Fuzzy"/character knobs -- 0 disables each. Deterministic per (entity,
    // tick) seed (see AIPilotSystem), so replays stay bit-exact.
    double reactionJitter = 0.0;     // +/- fraction of decisionInterval
    double aimJitter = 0.04;         // +/- rad added to fireTolerance per shot
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

    // Transient per-shot-attempt aim bias for AIPersonality::aimJitter: rolled
    // once when a firing opportunity opens and held steady while waiting for
    // an aligned shot, so a "sloppy" shot is a real, fixed aiming error rather
    // than the fire threshold flickering randomly tick to tick.
    double aimBias = 0.0;
    bool aimBiasRolled = false;

    // Shots left in the burst currently underway; 0 = between bursts (the
    // next successful shot starts a fresh one of personality.burstCount).
    std::uint32_t burstShotsRemaining = 0;
};

} // namespace Gravitaris
