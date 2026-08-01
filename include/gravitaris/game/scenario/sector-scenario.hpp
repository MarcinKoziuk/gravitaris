#pragma once

#include <cstdint>
#include <vector>

#include <flecs.h>

#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// The knobs a round is generated from (docs/sector-generation-plan.md S1).
// `seed` alone determines the sector for a given set of the others, so a
// round is reproducible from what the setup screen shows the player.
struct SectorParams {
    std::uint32_t seed = 0;
    int factionCount = 4;
    // Two more than the factions by default, so a round opens with a couple of
    // whole solar systems nobody starts in -- somewhere to expand that isn't
    // already someone's back yard, and the planets they carry are what makes
    // the majority of the sector unclaimed at the start.
    int stars = 6;
    int minPlanetsPerStar = 1;
    int maxPlanetsPerStar = 3;

    static constexpr int MIN_FACTIONS = 2;
    static constexpr int MAX_FACTIONS = 6; // the usable TeamId colours
    static constexpr int MIN_STARS = 2;
    static constexpr int MAX_STARS = 8;
    // No round has every sun occupied, however few stars it was asked for.
    static constexpr int MIN_FREE_STARS = 1;

    // Clamps every field into range and raises `stars` past `factionCount` --
    // homes never share a star, and MIN_FREE_STARS of them stay unclaimed.
    void Sanitize();
};

struct SectorLayout {
    // One home planet per faction, in FactionRoster order: faction i starts
    // at homes[i].
    std::vector<flecs::entity> homes;
    // World radius from the origin covering every body's furthest reach --
    // what the minimap sizes itself to, so it tracks the bodies actually
    // spawned rather than the generator's own bounding constant.
    double extent = 0.;
};

// Populates a generated solar sector: `stars` still suns scattered by a
// stream seeded from `params`, each with a few planets on pre-calculated
// circular orbits, and one home planet picked per faction. Spawns bodies
// only -- starting complexes and combatants are the caller's job (see
// Game::BuildWorld).
SectorLayout BuildSectorScenario(EntitySpawner& entitySpawner, const SectorParams& params);

} // namespace Gravitaris
