#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/scenario/structure-layout.hpp>
#include <gravitaris/game/scenario/starting-complex.hpp>

namespace Gravitaris {

void BuildStartingComplex(EntitySpawner& entitySpawner, flecs::entity planet, TeamId team)
{
    // Your home planet is yours from the start (matches the original: you
    // begin with an established complex on a claimed planet) -- also what
    // makes the ownership marker show immediately, no landing needed.
    planet.set<Team>(Team{team});

    entitySpawner.SpawnStructure(StructureType::Base, "models/structures/base"_id, planet, team);
    entitySpawner.SpawnStructure(StructureType::Colony, "models/structures/colony"_id, planet, team);
    entitySpawner.SpawnStructure(StructureType::Lab, "models/structures/lab"_id, planet, team);
    entitySpawner.SpawnStructure(StructureType::CommCenter, "models/structures/comm-center"_id, planet, team);

    const double orbitRadius = StructureLayout::OrbitRadius(planet.get<Planet>().radius);
    entitySpawner.SpawnOrbitingStructure(StructureType::HighPort, "models/structures/high-port-0"_id, planet, team,
                                         orbitRadius, 1.0, 0.0);
}

} // namespace Gravitaris
