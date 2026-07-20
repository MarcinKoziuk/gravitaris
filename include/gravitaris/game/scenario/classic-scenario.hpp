#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Populates the "classic mode" solar system: two still suns, each with a few
// green planets on pre-calculated circular orbits. Assumes the player is
// already spawned (or not -- this touches nothing player-related). Returns
// the first planet spawned (sunA's innermost orbiter) as a designated "home"
// planet -- gravity-well-mode-plan.md's starting complex goes there until
// Phase 6's sector generation picks starting planets per faction properly.
flecs::entity BuildClassicScenario(EntitySpawner& entitySpawner);

} // namespace Gravitaris
