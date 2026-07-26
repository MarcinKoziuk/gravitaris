#pragma once

#include <cstdint>

#include <flecs.h>

namespace Gravitaris {

// What an AI leader is currently trying to achieve (docs/ai-ships.md's
// Strategy layer, docs/gravity-well-mode-plan.md Phase 5). One goal is
// active at a time; AIStrategySystem re-scores them on a slow cadence and
// hands the winner to the tactical layer as an AIOrder.
enum class AIGoal : std::uint8_t {
    Dogfight,           // nothing beyond the nearest enemy fighter
    ClaimPlanet,        // land on an unowned planet and hold it
    AttackComplex,      // destroy an enemy planet's structures
    InterceptFreighter, // kill an enemy freighter in transit
    DefendComplex,      // patrol an own planet an enemy is closing on
};

// Per-goal multipliers on the utility scores, so two leaders in the same
// position play differently. 0 disables a goal for that personality.
struct AIStrategyWeights {
    double dogfight = 1.0;
    double claim = 1.0;
    double attackComplex = 1.0;
    double interceptFreighter = 1.0;
    double defend = 1.0;
};

// Server-only leader state (ADR 0001), on the AI fighters that play the mode
// rather than only fight in it. An AIPilot without one keeps its own
// nearest-enemy tactics, which is all dogfight fodder needs.
struct AIStrategy {
    AIGoal goal = AIGoal::Dogfight;
    flecs::entity subject;
    std::uint32_t decisionCooldown = 0;
    std::uint32_t decisionInterval = 120; // 2s at the fixed tick
    // Ticks a ClaimPlanet goal is held without re-scoring: a claim is a full
    // descent plus ConquestSystem::CLAIM_TICKS parked, and a leader that
    // re-decides halfway down never finishes one. Bounded so an unreachable
    // planet cannot strand a leader for the round.
    std::uint32_t commitCooldown = 0;
    AIStrategyWeights weights;
};

} // namespace Gravitaris
