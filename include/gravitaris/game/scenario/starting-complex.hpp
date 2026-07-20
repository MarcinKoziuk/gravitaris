#pragma once

#include <flecs.h>

#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Spawns a hand-assembled starting complex (docs/gravity-well-mode-plan.md
// Phase 2) at `planet` for `team`: Base/Colony/Lab/Comm Center planetside,
// High Port + its Space Dock/Sensor Array in orbit -- loosely matching the
// layout in docs/gwell/screenshots/start-game.png. Sector generation
// (Phase 6) will eventually pick a planet per faction; until then this is
// called once, at BuildClassicScenario's designated home planet.
void BuildStartingComplex(EntitySpawner& entitySpawner, flecs::entity planet, TeamId team);

} // namespace Gravitaris
