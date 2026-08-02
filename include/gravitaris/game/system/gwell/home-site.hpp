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

// The same, restricted to homes hosting one of the faction's own labs -- the
// only places an upgrade can actually be collected (ResearchSystem's collector
// rule), so a trip made for one has to end at one of these or it is wasted.
// Dead entity when the faction has no lab on a home planet; callers fall back
// to FindHomePlanet.
flecs::entity FindResearchPlanet(flecs::world& registry, TeamId team, const Magnum::Vector2d& from);

} // namespace Gravitaris
