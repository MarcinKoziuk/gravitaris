#pragma once

#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/ai-strategy.hpp>
#include <gravitaris/game/component/freighter.hpp>

namespace Gravitaris {

// Short display names for the AI state enums, shared by the AI debug tab and
// the world-space label overlay. Switches rather than indexed tables: adding
// an enumerator then reads as a compiler warning instead of a silent
// out-of-range name.
inline const char* AIBehaviorName(AIBehavior behavior)
{
    switch (behavior) {
        case AIBehavior::Idle:      return "Idle";
        case AIBehavior::Evade:     return "Evade";
        case AIBehavior::Intercept: return "Intercept";
        case AIBehavior::Orbit:     return "Orbit";
        case AIBehavior::Land:      return "Land";
        case AIBehavior::Landed:    return "Landed";
        case AIBehavior::Depart:    return "Depart";
        case AIBehavior::Flee:      return "Flee";
    }
    return "?";
}

inline const char* AIGoalName(AIGoal goal)
{
    switch (goal) {
        case AIGoal::Dogfight:           return "Dogfight";
        case AIGoal::ClaimPlanet:        return "Claim";
        case AIGoal::AttackComplex:      return "AttackComplex";
        case AIGoal::InterceptFreighter: return "InterceptFrtr";
        case AIGoal::DefendComplex:      return "Defend";
    }
    return "?";
}

inline const char* AIOrderKindName(AIOrderKind kind)
{
    switch (kind) {
        case AIOrderKind::None:   return "-";
        case AIOrderKind::Attack: return "Attack";
        case AIOrderKind::Land:   return "Land";
        case AIOrderKind::Patrol: return "Patrol";
    }
    return "?";
}

inline const char* BuildOrderName(BuildOrder order)
{
    switch (order) {
        case BuildOrder::Base:     return "Base";
        case BuildOrder::Colony:   return "Colony";
        case BuildOrder::HighPort: return "High Port";
    }
    return "?";
}

} // namespace Gravitaris
