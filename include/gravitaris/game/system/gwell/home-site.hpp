#pragma once

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/team.hpp>

namespace Gravitaris {

// Somewhere a faction can come home to: its own planet carrying both a Base
// and a Colony. The Base is the pad and the Colony is the reason anyone is on
// the rock at all, which is why a respawn, a repair and an AI's trip home all
// ask this same question -- they are the same question.
bool IsHomePlanet(flecs::world& registry, flecs::entity planet, TeamId team);

// The nearest home planet to `from`, or a dead entity when the faction has
// none left. Ties break by NetId so every run picks the same one (ADR 0001).
flecs::entity FindHomePlanet(flecs::world& registry, TeamId team, const Magnum::Vector2d& from);

} // namespace Gravitaris
