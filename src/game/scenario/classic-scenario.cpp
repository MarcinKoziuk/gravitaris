#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/scenario/classic-scenario.hpp>

namespace Gravitaris {

ClassicScenarioHomes BuildClassicScenario(EntitySpawner& entitySpawner)
{
    // The suns are the dominant gravity wells; the orbiting planets attract
    // too, far less.
    const id_t sun = "models/stars/sun"_id;
    const id_t planet = "models/planets/simple"_id;

    // Everything here is twice what it was: the bodies themselves doubled in
    // radius (data/models/planets, data/models/stars), so the gaps between
    // them had to follow or the system reads as crowded.
    const Vector2d sunA{-11200., 0.};
    const Vector2d sunB{11200., 0.};

    // Orbit angular speed is derived from centerMass at the actual gravity
    // settings (see OrbitSystem), so this is the star's effective attracting
    // mass -- mass * its own gravity multiplier, matching what ApplyGravity
    // computes for it as a source.
    const auto effectiveMass = [](flecs::entity star) {
        return star.get<GravitySource>().mass * star.get<GravitySource>().multiplier;
    };

    const double sunAMass = effectiveMass(entitySpawner.SpawnStar(sun, sunA));
    const flecs::entity homePlanet = entitySpawner.SpawnOrbitingPlanet(planet, sunA, sunAMass, 4000., 1.0, 0.0);
    entitySpawner.SpawnOrbitingPlanet(planet, sunA, sunAMass, 6800., -1.0, 2.1);
    entitySpawner.SpawnOrbitingPlanet(planet, sunA, sunAMass, 9600., 1.0, 4.0);

    const double sunBMass = effectiveMass(entitySpawner.SpawnStar(sun, sunB));
    const flecs::entity rivalPlanet = entitySpawner.SpawnOrbitingPlanet(planet, sunB, sunBMass, 4400., -1.0, 1.0);
    entitySpawner.SpawnOrbitingPlanet(planet, sunB, sunBMass, 8000., 1.0, 3.5);

    return ClassicScenarioHomes{homePlanet, rivalPlanet};
}

} // namespace Gravitaris
