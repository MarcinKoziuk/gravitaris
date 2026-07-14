#pragma once

#include <flecs.h>

#include <Magnum/Magnum.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/id.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

class EntitySpawner {
protected:
    flecs::world& m_registry;

    ResourceLoader& m_resourceLoader;

    virtual void AddRenderable(flecs::entity entity, id_t modelId);

public:
    explicit EntitySpawner(flecs::world& registry, ResourceLoader& resourceLoader);

    virtual ~EntitySpawner() = default;

    flecs::entity SpawnPlayer(id_t modelId, Vector2d position);

    flecs::entity SpawnAIShip(id_t modelId, Vector2d position);

    flecs::entity SpawnPlanet(id_t modelId, Vector2d position);

    // sensor: true for bullets whose hits are resolved by DamageSystem's
    // segment query rather than Chipmunk collision response (see RigidBodyDesc).
    flecs::entity SpawnBullet(id_t modelId, Vector2d position, Vector2d velocity, bool sensor = false);
};

} // namespace Gravitaris
