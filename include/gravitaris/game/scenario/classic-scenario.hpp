#pragma once

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Populates the "classic mode" solar system: two still suns, each with a few
// green planets on pre-calculated circular orbits. Assumes the player is
// already spawned (or not -- this touches nothing player-related).
void BuildClassicScenario(EntitySpawner& entitySpawner);

} // namespace Gravitaris
