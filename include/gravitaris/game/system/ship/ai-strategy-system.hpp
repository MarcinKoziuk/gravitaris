#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// The strategy layer of docs/ai-ships.md's GNC table (Phase 5 of
// docs/gravity-well-mode-plan.md): scores every goal an AI leader could
// pursue right now, weighted by its personality, and writes the winner to
// its AIPilot as an AIOrder. Never touches flight -- tactics, guidance and
// control below it are unchanged and still own every reflex.
//
// Runs before AIPilotSystem so an order issued this tick is flown this tick.
class AIStrategySystem {
    flecs::world& m_registry;

    // Only for the live gravity multiplier, which no component carries: a
    // claim is a landing, and a landing is only an option on a body whose
    // surface gravity the hull can actually out-thrust.
    PhysicsSystem& m_physicsSystem;

public:
    AIStrategySystem(flecs::world& registry, PhysicsSystem& physicsSystem);

    void Update();
};

} // namespace Gravitaris
