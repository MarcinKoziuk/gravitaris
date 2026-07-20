#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/scenario/starting-complex.hpp>

namespace Gravitaris {

void BuildStartingComplex(EntitySpawner& entitySpawner, flecs::entity planet, TeamId team)
{
    // Planetside: small offsets from the planet's center, well within its
    // ~60-unit true radius (data/models/planets/simple) so they render
    // nested inside the outline like the original's screenshots.
    entitySpawner.SpawnStructure(StructureType::Base, "models/structures/base"_id, planet, team,
                                 Vector2d{-15., -10.});
    entitySpawner.SpawnStructure(StructureType::Colony, "models/structures/colony"_id, planet, team,
                                 Vector2d{15., -10.});
    entitySpawner.SpawnStructure(StructureType::Lab, "models/structures/lab"_id, planet, team,
                                 Vector2d{-15., 15.});
    entitySpawner.SpawnStructure(StructureType::CommCenter, "models/structures/comm-center"_id, planet, team,
                                 Vector2d{15., 15.});

    // Orbital: High Port plus its Space Dock/Sensor Array on the same orbit
    // radius at small phase offsets, past the planet's own outline.
    constexpr double orbitRadius = 90.;
    entitySpawner.SpawnOrbitingStructure(StructureType::HighPort, "models/structures/high-port"_id, planet, team,
                                         orbitRadius, 1.0, 0.0);
    entitySpawner.SpawnOrbitingStructure(StructureType::SpaceDock, "models/structures/space-dock"_id, planet, team,
                                         orbitRadius, 1.0, 0.4);
    entitySpawner.SpawnOrbitingStructure(StructureType::SensorArray, "models/structures/sensor-array"_id, planet,
                                         team, orbitRadius, 1.0, -0.4);
}

} // namespace Gravitaris
