#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// The two designated starting planets, one per sun, for
// gravity-well-mode-plan.md's starting complexes -- placeholders until Phase
// 6's sector generation picks starting planets per faction properly.
struct ClassicScenarioHomes {
    flecs::entity blue; // sunA's innermost orbiter
    flecs::entity red;  // sunB's innermost orbiter, a system away
};

// Populates the "classic mode" solar system: two still suns, each with a few
// green planets on pre-calculated circular orbits. Assumes the player is
// already spawned (or not -- this touches nothing player-related).
ClassicScenarioHomes BuildClassicScenario(EntitySpawner& entitySpawner);

} // namespace Gravitaris
