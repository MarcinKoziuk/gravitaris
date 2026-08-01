#include <algorithm>
#include <cmath>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/ai/ai-preset-library.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/debug/debug-spawn.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

// Clear of the site itself: a wave dropped on top of a High Port is a wave
// of collisions.
static constexpr double WAVE_RADIUS = 600.0;

void SpawnAIWave(Game& game, int count, const AIPreset& preset, TeamId team)
{
    count = std::clamp(count, 1, 100);

    FactionSystem::SpawnPoint site;
    if (const std::optional<FactionSystem::SpawnPoint> owned = game.GetFactionSystem().SpawnPosition(team)) {
        site = *owned;
    }

    for (int i = 0; i < count; ++i) {
        const double angle = (2. * PI * static_cast<double>(i)) / static_cast<double>(count);
        const Vector2d offset{WAVE_RADIUS * std::cos(angle), WAVE_RADIUS * std::sin(angle)};
        game.GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, site.pos + offset, preset,
                                            site.vel, site.rot, team);
    }
}

} // namespace Gravitaris
