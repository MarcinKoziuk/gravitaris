#pragma once

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/component/team.hpp>

namespace Gravitaris {

struct AIPreset;

// Debug/dev spawning, shared by the server console and any in-game debug UI:
// `count` AI fighters on a ring around the team's own spawn site (or the
// world origin if that team holds nothing), evenly spaced so a wave doesn't
// pile up inside one hull. Not part of the mode -- FactionSystem owns real
// spawning.
void SpawnAIWave(Game& game, int count, const AIPreset& preset, TeamId team);

} // namespace Gravitaris
