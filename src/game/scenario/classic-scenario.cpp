#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/scenario/classic-scenario.hpp>

namespace Gravitaris {

void BuildClassicScenario(EntitySpawner& entitySpawner)
{
    // The suns are the dominant gravity wells; the orbiting planets attract
    // too, far less.
    const id_t sun = "models/stars/sun"_id;
    const id_t planet = "models/planets/simple"_id;

    const Vector2d sunA{-5600., 0.};
    const Vector2d sunB{5600., 0.};

    // Orbit angular speed is derived from centerMass at the actual gravity
    // settings (see OrbitSystem), so this is the star's effective attracting
    // mass -- mass * its own gravity multiplier, matching what ApplyGravity
    // computes for it as a source.
    const auto effectiveMass = [](flecs::entity star) {
        return star.get<GravitySource>().mass * star.get<GravitySource>().multiplier;
    };

    const double sunAMass = effectiveMass(entitySpawner.SpawnStar(sun, sunA));
    entitySpawner.SpawnOrbitingPlanet(planet, sunA, sunAMass, 2000., 1.0, 0.0);
    entitySpawner.SpawnOrbitingPlanet(planet, sunA, sunAMass, 3400., -1.0, 2.1);
    entitySpawner.SpawnOrbitingPlanet(planet, sunA, sunAMass, 4800., 1.0, 4.0);

    const double sunBMass = effectiveMass(entitySpawner.SpawnStar(sun, sunB));
    entitySpawner.SpawnOrbitingPlanet(planet, sunB, sunBMass, 2200., -1.0, 1.0);
    entitySpawner.SpawnOrbitingPlanet(planet, sunB, sunBMass, 4000., 1.0, 3.5);
}

} // namespace Gravitaris
