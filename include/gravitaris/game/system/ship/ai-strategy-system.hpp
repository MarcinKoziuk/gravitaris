#pragma once

#include <flecs.h>

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

public:
    explicit AIStrategySystem(flecs::world& registry);

    void Update();
};

} // namespace Gravitaris
